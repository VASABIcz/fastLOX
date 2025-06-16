#include "../lexing/Token.h"
#include "../lexing/SourceProvider.h"
#include "../lexing/lexerExceptions.h"
#include "../lexing/lexingUnits.h"
#include "../lexing/tokenize.h"
#include "../parsing/Parser.h"
#include "../utils/pdo_utils.h"
#include "../codegen/SSARegister.h"
#include "../codegen/IRGen.h"
#include "../codegen/CodeGen.h"
#include "../codegen/IRGenCtx.h"
#include "../codegen/x86/X86Assembler.h"
#include <filesystem>
#include <cstring>
#include <cstdlib>
#include <unordered_map>
#include <sanitizer/asan_interface.h>
#include "../utils/code_gen.h"

#define VERBOSE 0
bool DEBUG_JIT = false;

enum class AllocType {
    LOX_VALUE, // constant (8B)
    HASH_MAP, // constant (16B)
    CLASS_REF, // constant (40B)
    OBJECT_REF, // constant (40B)
    STRING, // 8B+N
    HASH_MAP_BUCKET, // constant ... for now (4+(12*2)/4+(12*2))
    FUNCTION_REF, // 32B+8*N
    HASH_MAP_BUCKET_ARRAY // 4*8 (32B) -> 8*8 (64B) -> 16*8 (128B)
};
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#define IS_POISONED(...) __asan_address_is_poisoned(__VA_ARGS__)
#else
#define IS_POISONED(...) false
#endif

void assertIsValidPtr(void* ptr);

std::string_view allocToString(AllocType type) {
    switch (type) {
        case AllocType::LOX_VALUE: return "LOX_VALUE";
        case AllocType::HASH_MAP: return "HASH_MAP";
        case AllocType::HASH_MAP_BUCKET_ARRAY: return "HASH_MAP_BUCKET_ARRAY";
        case AllocType::HASH_MAP_BUCKET: return "HASH_MAP_BUCKET";
        case AllocType::FUNCTION_REF: return "FUNCTION_REF";
        case AllocType::CLASS_REF: return "CLASS_REF";
        case AllocType::OBJECT_REF: return "OBJECT_REF";
        case AllocType::STRING: return "STRING";
    }
    UNREACHABLE();
}

struct SimpleArena {
    struct Chunk {
        Chunk* next = nullptr;
        size_t size = 0;
        size_t capacity;
        char data[];

        void free() {
            if (next != nullptr) next->free();
            ::free(this);
        }
    };
    Chunk* root = nullptr;
    Chunk* current = nullptr;

    static constexpr size_t START_SIZE = 128;

    void* allocate(size_t size, size_t align1) {
        if (current == nullptr) {
            root = static_cast<Chunk*>(malloc(sizeof(Chunk) + START_SIZE));
            root->capacity = START_SIZE;
            root->size = 0;
            root->next = nullptr;
            current = root;
        }

        auto availableSize = current->capacity - current->size;
        auto possibleAdr = current->data + current->size;
        auto alignedAdr = align((size_t)possibleAdr, align1);

        auto realSize = (alignedAdr-(size_t)possibleAdr)+size;

        if (availableSize >= realSize) {
            current->size += realSize;
            return (void*)alignedAdr;
        }

        // println("REQUESTING ALLOC! {}", sizeof(Chunk) + current->capacity*2);
        auto* newChunk = static_cast<Chunk*>(malloc(sizeof(Chunk) + current->capacity*2));
        newChunk->capacity = current->capacity*2;
        newChunk->next = nullptr;
        newChunk->size = 0;
        current->next = newChunk;
        current = newChunk;

        return allocate(size, align1);
    }

    template<typename T>
    span<T> allocSpan(size_t size) {
        auto* data = allocate(size*sizeof(T), alignof(T));

        return {data, size};
    }

    string_view allocString(string_view s, bool nullTerminated = true) {
        auto* data = (char*)allocate(s.size()+(nullTerminated ? 1 : 0), 1);
        std::memcpy(data, s.data(), s.size());
        if (nullTerminated) (data)[s.size()] = 0;

        return string_view{data, s.size()};
    }

    template<typename T, typename... Args>
    T* allocateElement(Args&&... arg) {
        auto* data = allocate(sizeof(T), alignof(T));
        return new (data)T(std::forward<Args>(arg)...);
    }

    void free() {
        if (root != nullptr) root->free();
        root = nullptr;
        current = nullptr;
    }

    void reset() {

    }
};

struct Heap;

SimpleArena HEAP_ARENA;


void* allocate(size_t size, AllocType type);

template<typename T>
T* allocateTyped(size_t size, AllocType type) {
    return (T*) allocate(size, type);
}

template<typename T>
T* allocateTypedSimple(AllocType type) {
    return (T*) allocate(sizeof(T), type);
}

SimpleArena AST_ARENA;

enum class TokenType1 {
    Var,
    For,
    To,
    If,
    Array,
    Of,
    Integer,
    Float,
    Const,
    Function,
    Else,
    While,
    Div,
    Mul,
    Modulo,
    Or,
    And,
    True,
    False,
    Return,
    Class,
    Super,
    Print,

    Semicolon,
    Dot,
    This,
    ORB,
    CRB,
    OSB,
    CSB,
    OCB,
    CCB,
    OPB,
    CPB,
    Plus,
    Minus,
    Colon,
    Assign,
    Comma,

    Idntifier,
    NumberLiteral,
    String,
    Continue,
    Break,

    Equals,
    NotEquals,

    Nil,
    Negate,
    GEQ,
    LEQ
};


#define KEYWORD_UNIT(a, b) buf.push_back(make_unique<KeywordLexingUnit<TokenType1>>(b, TokenType1::a))
#define RANGE_UNIT(a, b, c) buf.push_back(make_unique<RangeLexingUnit<TokenType1>>(b, c, TokenType1::a))

constexpr size_t KEYWORD_COUNT = 21;

const extern array<pair<TokenType1, string>, KEYWORD_COUNT> KEYWORDS{
        make_pair<TokenType1, string>(TokenType1::Function, "fun"),
        {TokenType1::If, "if"},
        {TokenType1::Else, "else"},
        {TokenType1::While, "while"},
        {TokenType1::Integer, "integer"},
        {TokenType1::Float, "float"},
        {TokenType1::Var, "var"},
        {TokenType1::True, "true"},
        {TokenType1::False, "false"},
        {TokenType1::Return, "return"},
      {TokenType1::Class, "class"},
      {TokenType1::Super, "super"},
      {TokenType1::Print, "print"},
        {TokenType1::For, "for"},
        {TokenType1::Continue, "continue"},
        {TokenType1::Break, "break"},
    {TokenType1::Nil, "nil"},
    {TokenType1::And, "and"},
    {TokenType1::Or, "or"},
    {TokenType1::Super, "super"},
    {TokenType1::This, "this"},
};

vector<unique_ptr<LexingUnit<TokenType1>>> getLexingUnits() {
    vector<unique_ptr<LexingUnit<TokenType1>>> buf{};

    buf.push_back(make_unique<RangeLexingUnit<TokenType1>>("/*","*/", optional<TokenType1>{}));
    buf.push_back(make_unique<LineCommentLexingUnit<TokenType1>>());

    for (const auto& keyword : KEYWORDS) {
        buf.push_back(make_unique<AlphabeticLexingUnit<TokenType1>>(keyword.second, keyword.first));
    }


    KEYWORD_UNIT(Equals, "==");
    KEYWORD_UNIT(NotEquals, "!=");
    KEYWORD_UNIT(LEQ, "<=");
    KEYWORD_UNIT(GEQ, ">=");
    KEYWORD_UNIT(CPB, ">");
    KEYWORD_UNIT(OPB, "<");
    KEYWORD_UNIT(Assign, "=");
    KEYWORD_UNIT(Colon, ":");
    KEYWORD_UNIT(ORB, "(");
    KEYWORD_UNIT(CRB, ")");
    KEYWORD_UNIT(OSB, "[");
    KEYWORD_UNIT(CSB, "]");
    KEYWORD_UNIT(OCB, "{");
    KEYWORD_UNIT(CCB, "}");
    KEYWORD_UNIT(Plus, "+");
    buf.push_back(make_unique<NumericLexingUnit<TokenType1, TokenType1::NumberLiteral>>());
    KEYWORD_UNIT(Minus, "-");
    KEYWORD_UNIT(Div, "/");
    KEYWORD_UNIT(Modulo, "%");
    KEYWORD_UNIT(Mul, "*");
    KEYWORD_UNIT(Dot, ".");
    KEYWORD_UNIT(Semicolon, ";");
    KEYWORD_UNIT(Negate, "!");
    KEYWORD_UNIT(Comma, ",");

    buf.push_back(make_unique<IdentifierLexingUnit<TokenType1>>(TokenType1::Idntifier));
    buf.push_back(make_unique<WhitespaceLexingUnit<TokenType1>>());
    buf.push_back(make_unique<StringLexingUnit<TokenType1>>(TokenType1::String));

    return buf;
}

auto prog = R"(
var a = "global";

{
  fun assign() {
    var b = 3
    a = "assigned";
    fun lolko() {
        b = 4
    }
  }

  var a = "inner";
  assign();
  print a; // expect: inner
}

print a; // expect: assigned

)";

struct ASTVisitor;

struct ASTNode {
    virtual ~ASTNode() = default;
    // ASTNode() = default;

    vector<Token<TokenType1>> tokens;

    virtual void visit(ASTVisitor& it) = 0;

    virtual string_view className() const = 0;
};

struct Statement: ASTNode {
};

struct Expression: Statement {
};

using ASTExpr = Expression*; // unique_ptr<Expression>;
using ASTStm = Statement*; // unique_ptr<Statement>;
using ASTNode1 = ASTNode*; //unique_ptr<ASTNode>;

struct IntLiteral;
struct Assign;
struct Block;
struct Call;
struct Identifier;
struct Function;
struct Binary;
struct IF;
struct Print;
struct Return;
struct StringLiteral1;
struct VariableDeclaration;
struct SpecializedVariable;
struct BoolLiteral1;
struct While;
struct NilLiteral;
struct Negate;
struct Class;
struct This;
struct Super;
struct FieldAccess;

enum class HookedVariableType {
    LOCAL,
    UPVAL,
    GLOBAL,
    ALLOC_UPVAL,
    // values that don't need to be allocated bcs they are imutable
    ALLOC_CONST_UPVAL,
    CONST_UPVAL
};

struct SpecializedVariable {
    HookedVariableType type;
    size_t id = -1;
    size_t frameId = -1;

    static SpecializedVariable Local(size_t id) {
        return SpecializedVariable{HookedVariableType::LOCAL, id, 0};
    }

    static SpecializedVariable AllocateCaptured(size_t id) {
        return SpecializedVariable{HookedVariableType::ALLOC_UPVAL, id, 0};
    }

    static SpecializedVariable AllocateConstCaptured(size_t id) {
        return SpecializedVariable{HookedVariableType::ALLOC_CONST_UPVAL, id, 0};
    }

    string toString() const {
        using enum HookedVariableType;
        switch (type) {
            case LOCAL:
                return stringify("LOCAL {}", id);
            case UPVAL:
                return stringify("UPVAL {}:{}", id, frameId);
            case GLOBAL:
                return stringify("GLOBAL {}", id);
            case ALLOC_UPVAL:
                return stringify("ALLOC_UPVAL {}:{}", id, frameId);
            case ALLOC_CONST_UPVAL:
                return stringify("ALLOC_CONST_UPVAL {}:{}", id, frameId);
            case CONST_UPVAL:
                return stringify("CONST_UPVAL {}:{}", id, frameId);

        }
        PANIC();
    }
};

using SpecTarget = SpecializedVariable;

struct ASTVisitor {
    virtual void invoke(IntLiteral& it) {}

    virtual void invoke(Assign& it) {}

    virtual void invoke(Block& it) {}

    virtual void invoke(Call& it) {}

    virtual void invoke(Identifier& it) {}

    virtual void invoke(Function& it) {}

    virtual void invoke(Binary& it) {}

    virtual void invoke(IF& it) {}

    virtual void invoke(Print& it) {}

    virtual void invoke(Return& it) {}

    virtual void invoke(StringLiteral1& it) {}

    virtual void invoke(VariableDeclaration& it) {}

    virtual void invoke(BoolLiteral1& it) {}

    virtual void invoke(While& it) {}

    virtual void invoke(NilLiteral& it) {}

    virtual void invoke(Negate& it) {}

    virtual void invoke(Class& it) {}

    virtual void invoke(This& it) {}

    virtual void invoke(Super& it) {}

    virtual void invoke(FieldAccess& it) {}

    virtual void invoke(ASTNode& it) {
        PANIC("UNIMPLEMENTED AST NODE {}", typeid(it).name());
    }
};

EXPR_BASE(Call, {
    ASTExpr fName;
    vector<ASTExpr> args;
})
};

struct Decl: ASTNode {

};

struct ASTExecutor;

struct Function: Statement {
    STRUCT_BEGIN(Function)
    string name;
    vector<string> argz;
    vector<ASTStm> body;
    STRUCT_END(Function)

    vector<bool> locals; // index is local id, value indicates if local escapes
    vector<bool> isModified; // index is local id, value indicates if local is modified
    vector<size_t> captures; // list of parent function up-local-ids that we capture
    // size_t upValCount = 0;
    SpecTarget hookedTarget;
    std::function<void(ASTExecutor&)> native;
    bool isMethod = false;
    void* runtimeData = nullptr; // field holding function pointer to jited/builtin

    size_t getParamUpValOffset() {
        size_t acu = 0;
        for (auto i = 0UL; i < data.argz.size(); i++) {
            if (locals[i]) acu += 1;
        }
        return acu;
    }

    size_t upValCount() {
        size_t acu = 0;
        for (auto i = 0UL; i < locals.size(); i++) {
            if (locals[i]) acu += 1;
        }
        return acu;
    }

    size_t totalUpValCount() {
        return upValCount()+captures.size();
    }

    size_t putCapture(size_t parentLocalId) {
        for (auto i = 0UL; i < captures.size(); i++) {
            if (captures[i] == parentLocalId) return locals.size()+i;
        }
        captures.push_back(parentLocalId);

        return locals.size()+captures.size()-1;
    }

    size_t getUpValId(size_t localId) {
        // assert(localId < totalUpValCount());
        if (localId >= locals.size()) return upValCount()+(localId-locals.size());
        size_t acu = 0;
        for (auto i = 0UL; i < locals.size(); i++) {
            if (i == localId) return acu;

            if (locals[i]) acu += 1;
        }
        PANIC();
    }

    size_t getLocalId(size_t localId) {
        size_t acu = 0;
        for (auto i = 0UL; i < locals.size(); i++) {
            if (i == localId) return acu;

            if (not locals[i]) acu += 1;
        }
        PANIC();
    }

    bool isModifiedLocal(size_t localId) {
        assert(localId < isModified.size());

        return isModified[localId];
    }

    bool isModifiedCaptured(size_t capturedId) {
        return isModifiedLocal(getLocalId(capturedId));
    }
};

struct FunctionRef;

struct Class: Statement {
    STRUCT_BEGIN(Class)
    string name;
    optional<string> superClass;
    vector</*unique_ptr<*/Function*/*>*/> methods;
    STRUCT_END(Class)
    unordered_map<u32, Function*> methodIds;

    SpecTarget hookedSuper;
    SpecTarget hookedDst;

    Function* getMethod(u32 name) {
        auto res = methodIds.find(name);
        if (res == methodIds.end()) return nullptr;
        return res->second;
    }
};

struct This: Expression {
    STRUCT_BEGIN(This)
    STRUCT_END(This)
    SpecTarget hookedTarget{};
};

struct Super: Expression {
    STRUCT_BEGIN(Super)
    STRUCT_END(Super)
    SpecTarget hookedTarget{};
};

enum class BinaryType {
    ADD,
    SUB,
    DIV,
    MOD,
    MUL,
    GT,
    GEQ,
    LEQ,
    LESS,
    EQ,
    NEQ,
    AND,
    OR
};

string binarToString(BinaryType t) {
    switch (t) {
        case BinaryType::ADD: return "+";
        case BinaryType::SUB: return "-";
        case BinaryType::DIV: return "/";
        case BinaryType::MOD: return "%";
        case BinaryType::MUL: return "*";
        case BinaryType::GT: return ">";
        case BinaryType::GEQ: return ">=";
        case BinaryType::LEQ: return "<=";
        case BinaryType::LESS: return "<";
        case BinaryType::EQ: return "==";
        case BinaryType::NEQ: return "!=";
        case BinaryType::AND: return "&&";
        case BinaryType::OR: return "||";
    }
    PANIC();
}


struct Binary: Expression {
    STRUCT_BEGIN(Binary)
    BinaryType type;
    ASTExpr lhs;
    ASTExpr rhs;
    STRUCT_END(Binary)
};

struct IF: Statement {
    STRUCT_BEGIN(IF)
    ASTExpr cond;
    ASTStm ifBody;
    optional<ASTStm> elsBody;
    STRUCT_END(IF)
};

struct StatmentExpr: Statement {
    STRUCT_BEGIN(StatmentExpr)
    ASTExpr inner;
    STRUCT_END(StatmentExpr)
};

struct Identifier: Expression {
    STRUCT_BEGIN(Identifier)
    string value;
    SpecTarget hookedTarget;
    STRUCT_END(Identifier)
};

struct VariableDeclaration: Statement {
    STRUCT_BEGIN(VariableDeclaration)
    string dst;
    optional<ASTExpr> value;
    SpecTarget hookedTarget;
    STRUCT_END(VariableDeclaration)
};

struct Return: Statement {
    STRUCT_BEGIN(Return)
    optional<ASTExpr> value;
    STRUCT_END(Return)
};

struct Print: Statement {
    STRUCT_BEGIN(Print)
    ASTExpr value;
    STRUCT_END(Print)
};

struct Block: Statement {
    STRUCT_BEGIN(Block)
    vector<ASTStm> statements;
    STRUCT_END(Block)
};

struct While: Statement {
    STRUCT_BEGIN(While)
    optional<ASTExpr> cond;
    ASTStm body;
    STRUCT_END(While)
};

/*struct For: Statement {
    STRUCT_BEGIN(For)
    ASTStm setup;
    ASTExpr cond;
    ASTStm post;
    Block statements;
    STRUCT_END(For)
};*/

struct Assign: Expression {
    STRUCT_BEGIN(Assign)
    ASTExpr dst;
    ASTExpr value;
    STRUCT_END(Assign)
};

struct IntLiteral: Expression {
    STRUCT_BEGIN(IntLiteral)
    double value;
    STRUCT_END(IntLiteral)
};

struct StringLiteral1: Expression {
    STRUCT_BEGIN(StringLiteral1)
    string value;
    STRUCT_END(StringLiteral1)
};

struct BoolLiteral1: Expression {
    STRUCT_BEGIN(BoolLiteral1)
    bool value;
    STRUCT_END(BoolLiteral1)
};

struct FieldAccess: Expression {
    STRUCT_BEGIN(FieldAccess)
    ASTExpr subject;
    string fieldName;
    STRUCT_END(FieldAccess)
    u32 fieldId;
};

struct Negate: Expression {
    STRUCT_BEGIN(Negate)
        ASTExpr inner;
    STRUCT_END(Negate)
};

struct NilLiteral: Expression {
    STRUCT_BEGIN(NilLiteral)
    STRUCT_END(NilLiteral)
};

template<typename T, typename... ARGS>
T* makeStuff2(ARGS&&... args) {
    return AST_ARENA.allocateElement<T>(typename T::Data(std::forward<ARGS>(args)...)); // new T(typename T::Data(std::forward<ARGS>(args)...));
}

struct MilaState {

};

struct NotAType {};
struct NotAStatement {};
struct NotAExpr {};
struct InvalidMilaParseError {};
struct MilaParseError: Errorable {
    std::variant<InvalidMilaParseError, ParserError<TokenType1>, NotAType, NotAStatement, NotAExpr> idk;

    MilaParseError(ParserError<TokenType1>&& it): idk(std::move(it)) {
        if (std::holds_alternative<ParserError<TokenType1>>(idk)) {
            if (std::holds_alternative<InvalidError>(std::get<ParserError<TokenType1>>(idk))) PANIC();
        }
    }

    MilaParseError() = delete;

    MilaParseError(NotAType&& it): idk(std::move(it)) {

    }

    MilaParseError(NotAExpr&& it): idk(std::move(it)) {

    }

    MilaParseError(NotAStatement&& it): idk(std::move(it)) {

    }

    std::string_view type() const override { return "asdadad2w"; }
    std::string message(ExceptionContext &) override { return "asdasdsad"; }
    std::string_view category() const override { return "asdasd"; }
    const std::string_view className() const override { return "asda"; }

    string toString() const {
        return Variant(idk).match<string>(
            CASE(ParserError<TokenType1>) {
                return toStringParserError(it);
            },
            CASE(NotAType) {
                return "NotAType"s;
            },
            CASE(NotAStatement) {
                return "NotAStatement"s;
            },
            CASE(NotAExpr) {
                return "NotAExpr"s;
            },
            CASE(InvalidMilaParseError) {
                return "InvalidMilaParseError"s;
            }
        );
    }
};

typedef ParsingUnit<TokenType1, ASTNode1, MilaState, MilaParseError> MilaParsingUnit;

template<typename T>
using MilaResult = std::expected<T, MilaParseError>;

template<>
class BaseParser<TokenType1, ASTNode1, MilaState, MilaParseError> : public Parser<TokenType1, ASTNode1, MilaState, MilaParseError> {
public:
    using Parser::Parser;

    MilaResult<ASTExpr> parseExpression() {
        auto stuff = TRY(parseSomethingTerminator(TokenType1::Semicolon))/*.release()*/;
        auto csted = dynamic_cast<Expression*>(stuff);
        if (csted == nullptr) {
            // delete stuff;
            return unexpected{NotAExpr{}};
        }

        return ASTExpr(csted);
    }

    MilaResult<ASTStm> parseStatement() {
        auto stuff = TRY(parseSomethingTerminator(TokenType1::Semicolon))/*.release()*/;
        auto csted = dynamic_cast<Statement*>(stuff);
        if (csted == nullptr) {
            auto csted1 = dynamic_cast<Expression*>(stuff);
            if (csted1 != nullptr) {
                return makeStuff2<StatmentExpr>(ASTExpr(csted1));
            }
            delete stuff;
            return unexpected{NotAStatement{}};
        }

        return ASTStm(csted);
    }

    MilaResult<vector<ASTStm>> parseBody() {
        vector<ASTStm> body;

        TRY(getAssert(TokenType1::OCB));

        while (!isPeekTypeConsume(TokenType1::CCB)) {
            body.push_back(TRY(parseStatement()));
        }

        return body;
    }

    MilaResult<string> assertIdent() {
        return string(TRY(this->getAssert(TokenType1::Idntifier)).content);
    }

    MilaResult<vector<ASTStm>> parseStatementsUntil(initializer_list<TokenType1> toks) {
        vector<ASTStm> body;

        while (!isPeekTypeOneOf(toks)) {
            body.push_back(TRY(parseStatement()));
        }

        TRY(this->consumeToken());

        return body;
    }

    template<typename T>
    bool isPrev() const {
        return isPeekStack([&](auto& it) { return dynamic_cast<const T*>(it/*.get()*/); });
    }

    bool isPrevExp() const {
        return isPrev<Expression>();
    }

    MilaResult<ASTExpr> popExpr() {
        if (not isPrevExp()) return unexpected{NotAExpr{}};

        auto preExpr = TRY(prevPop());
        auto* ptr = preExpr;/*.release()*/;
        return ASTExpr(dynamic_cast<Expression*>(ptr));
    }

    void pushExp(ASTExpr exp) {
        this->buffer.push_back(std::move(exp));
    }
};

typedef BaseParser<TokenType1, ASTNode1, MilaState, MilaParseError> MilaParser;

struct FunctionParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "FunctionParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Ahead;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPeekType(TokenType1::Function);
    }

    std::expected<ASTNode1, MilaParseError> parse(MilaParser& parser) const override {
        assertToken(TokenType1::Function);

        auto name = TRY(parser.assertIdent());


        TRY(parser.getAssert(TokenType1::ORB));

        auto params = TRY(parser.parseManyWithSeparatorUntil3(TokenType1::Comma, TokenType1::CRB,[&] {
            return parser.assertIdent();
        }));

        TRY(parser.getAssert(TokenType1::CRB));

        auto body = TRY(parser.parseBody());

        return makeStuff2<Function>(string(name), std::move(params), std::move(body));
    }
};

struct IFParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "IFParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Ahead;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPeekType(TokenType1::If);
    }

    MilaResult<ASTNode1> parse(MilaParser& parser) const override {
        assertToken(TokenType1::If);

        auto cond = TRY(parser.parseExpression());

        auto ifBody  = TRY(parser.parseStatement());
        optional<ASTStm> elsBody;

        if (parser.isPeekTypeConsume(TokenType1::Else)) {
            elsBody = TRY(parser.parseStatement());
        }

        return makeStuff2<IF>(std::move(cond), std::move(ifBody), std::move(elsBody));
    }
};

struct FunctionCallParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "FunctionCallParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Behind;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPrevExp() && parser.isPeekType(TokenType1::ORB);
    }

    MilaResult<ASTNode1> parse(MilaParser& parser) const override {
        auto subject = TRY(parser.popExpr());

        assertToken(TokenType1::ORB);

        auto argz = TRY(parser.parseManyWithSeparatorUntil3(TokenType1::Comma, TokenType1::CRB, [&] {
           return parser.parseExpression();
        }));

        assertToken(TokenType1::CRB);

        return makeStuff2<Call>(std::move(subject), std::move(argz));
    }
};

struct WhileParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "WhileParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Ahead;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPeekType(TokenType1::While);
    }

    MilaResult<ASTNode1> parse(MilaParser& parser) const override {
        assertToken(TokenType1::While);

        auto cond = TRY(parser.parseExpression());

        auto body = TRY(parser.parseStatement());

        return makeStuff2<While>(std::move(cond), std::move(body));
    }
};

struct ForParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "ForParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Ahead;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPeekType(TokenType1::For);
    }

    MilaResult<ASTNode1> parse(MilaParser& parser) const override {
        assertToken(TokenType1::For);

        assertToken(TokenType1::ORB);

        optional<ASTStm> start;
        if (not parser.isPeekTypeConsume(TokenType1::Semicolon)) {
            start = TRY(parser.parseStatement());
        }

        optional<ASTExpr> cond;
        if (not parser.isPeekTypeConsume(TokenType1::Semicolon)) {
            cond = TRY(parser.parseExpression());
        }

        optional<ASTStm> update;
        if (not parser.isPeekTypeConsume(TokenType1::CRB)) {
            update = TRY(parser.parseStatement());
            assertToken(TokenType1::CRB);
        }

        vector<ASTStm> body;
        body.push_back(TRY(parser.parseStatement()));
        if (update.has_value()) {
            body.push_back(std::move(*update));
        }

        auto whajl = makeStuff2<While>(std::move(cond), makeStuff2<Block>(std::move(body)));

        auto block = makeStuff2<Block>();
        if (start.has_value()) {
            block->data.statements.push_back(std::move(*start));
        }
        block->data.statements.push_back(std::move(whajl));

        return block;
    }
};

struct AssignParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "AssignParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Behind;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPrevExp() && parser.isPeekType(TokenType1::Assign);
    }

    MilaResult<ASTNode1> parse(MilaParser& parser) const override {
        auto tgt = TRY(parser.popExpr());

        assertToken(TokenType1::Assign);

        auto value = TRY(parser.parseExpression());

        return makeStuff2<Assign>(std::move(tgt), std::move(value));
    }
};

struct VariableDeclarationParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "VariableDeclarationParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Ahead;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPeekType(TokenType1::Var);
    }

    MilaResult<ASTNode1> parse(MilaParser& parser) const override {
        assertToken(TokenType1::Var);

        auto name = TRY(parser.assertIdent());

        optional<ASTExpr> value;
        if (parser.isPeekTypeConsume(TokenType1::Assign)) {
            value = TRY(parser.parseExpression());
        }

        return makeStuff2<VariableDeclaration>(std::move(name), std::move(value));
    }
};

optional<pair<BinaryType, int>> toType(TokenType1 tok) {
    switch (tok) {
        case TokenType1::Div: return optional{pair{BinaryType::DIV, 3}};
        case TokenType1::Mul: return optional{pair{BinaryType::MUL, 3}};
        case TokenType1::Modulo: return optional{pair{BinaryType::MOD, 3}};

        case TokenType1::Plus: return optional{pair{BinaryType::ADD, 2}};
        case TokenType1::Minus: return optional{pair{BinaryType::SUB, 3}};

        case TokenType1::OPB: return optional{pair{BinaryType::LESS, 1}};
        case TokenType1::CPB: return optional{pair{BinaryType::GT, 1}};
        case TokenType1::Equals: return optional{pair{BinaryType::EQ, 1}};
        case TokenType1::NotEquals: return optional{pair{BinaryType::NEQ, 1}};
        case TokenType1::LEQ: return optional{pair{BinaryType::LEQ, 1}};
        case TokenType1::GEQ: return optional{pair{BinaryType::GEQ, 1}};

        case TokenType1::And: return optional{pair{BinaryType::AND, 0}};
        case TokenType1::Or: return optional{pair{BinaryType::OR, 0}};
        default:
            return {};
    }
}

struct ThisSuperParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "ThisSuperParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Ahead;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPeekTypeOneOf({TokenType1::Super, TokenType1::This});
    }

    MilaResult<ASTNode1> parse(MilaParser& parser) const override {
        if (parser.isPeekTypeConsume(TokenType1::This)) {
            return makeStuff2<This>();
        } else {
            assertToken(TokenType1::Super);
            return makeStuff2<Super>();
        }
    }
};

struct IdentParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "IdentParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Ahead;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPeekType(TokenType1::Idntifier);
    }

    MilaResult<ASTNode1> parse(MilaParser& parser) const override {
        auto ident = TRY(parser.assertIdent());

        return makeStuff2<Identifier>(std::move(ident));
    }
};

struct StringLiteralParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "StringLiteralParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Ahead;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPeekType(TokenType1::String);
    }

    MilaResult<ASTNode1> parse(MilaParser& parser) const override {
        auto ident = string(assertToken(TokenType1::String).content);

        return makeStuff2<StringLiteral1>(std::move(ident));
    }
};

struct BoolLiteralParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "BoolLiteralParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Ahead;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPeekType(TokenType1::True) || parser.isPeekType(TokenType1::False);
    }

    MilaResult<ASTNode1> parse(MilaParser& parser) const override {
        bool isTrue = true;
        if (parser.isPeekTypeConsume(TokenType1::True)) {

        } else {
            isTrue = false;
            assertToken(TokenType1::False);
        }

        return makeStuff2<BoolLiteral1>(isTrue);
    }
};

struct PrefixMinusParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "PrefixMinusParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Ahead;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPeekType(TokenType1::Minus);
    }

    MilaResult<ASTNode1> parse(MilaParser& parser) const override {
        assertToken(TokenType1::Minus);

        TRY(parser.parseBinaryArm());

        auto expr = TRY(parser.popExpr());

        if (auto v = dynamic_cast<IntLiteral*>(expr/*.get()*/); v) {
            return makeStuff2<IntLiteral>(-v->data.value);
        }

        return makeStuff2<Binary>(BinaryType::SUB, makeStuff2<IntLiteral>(0.0), std::move(expr));
    }
};

struct NilLiteralParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "NilLiteralParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Ahead;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPeekType(TokenType1::Nil);
    }

    MilaResult<ASTNode1> parse(MilaParser& parser) const override {
        assertToken(TokenType1::Nil);

        return makeStuff2<NilLiteral>();
    }
};

struct NumericParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "NumericParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Ahead;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPeekType(TokenType1::NumberLiteral);
    }

    MilaResult<ASTNode1> parse(MilaParser& parser) const override {
        auto ident = TRY(parser.getAssert(TokenType1::NumberLiteral)).content;

        // FIXME mby handle error?
        double idk = std::strtod(ident.begin(), nullptr);

        return makeStuff2<IntLiteral>(idk);
    }
};

struct ClassParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "ClassParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Ahead;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPeekType(TokenType1::Class);
    }

    MilaResult<ASTNode1> parse(MilaParser& parser) const override {
        assertToken(TokenType1::Class);

        auto className = TRY(parser.assertIdent());

        optional<string> superName;
        if (parser.isPeekTypeConsume(TokenType1::OPB)) {
            superName = TRY(parser.assertIdent());
        }

        assertToken(TokenType1::OCB);

        vector</*unique_ptr<*/Function*/*>*/> methods;

        while (!parser.isPeekTypeConsume(TokenType1::CCB)) {
            auto methodName = TRY(parser.assertIdent());

            assertToken(TokenType1::ORB);
            auto params = TRY(parser.parseManyWithSeparatorUntil3(TokenType1::Comma, TokenType1::CRB, [&] {
               return parser.assertIdent();
            }));

            assertToken(TokenType1::CRB);

            auto body = TRY(parser.parseBody());

            auto func = makeStuff2<Function>(std::move(methodName), std::move(params), std::move(body));
            func->isMethod = true;
            methods.push_back(std::move(func));
        }

        return makeStuff2<Class>(std::move(className), std::move(superName), std::move(methods));
    }
};

struct BracketsParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "BracketsParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Ahead;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPeekType(TokenType1::ORB);
    }

    MilaResult<ASTNode1> parse(MilaParser& parser) const override {
        assertToken(TokenType1::ORB);

        auto inner = TRY(parser.parseExpression());

        assertToken(TokenType1::CRB);

        return inner;
    }
};

struct ReturnParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "ReturnParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Ahead;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPeekType(TokenType1::Return);
    }

    MilaResult<ASTNode1> parse(MilaParser& parser) const override {
        assertToken(TokenType1::Return);

        optional<ASTExpr> inner;
        if (parser.isPeekType(TokenType1::Semicolon)) {
        } else {
            inner = TRY(parser.parseExpression());
        }

        return makeStuff2<Return>(std::move(inner));
    }
};

struct PrintParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "PrintParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Ahead;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPeekType(TokenType1::Print);
    }

    MilaResult<ASTNode1> parse(MilaParser& parser) const override {
        assertToken(TokenType1::Print);

        auto inner = TRY(parser.parseExpression());

        return makeStuff2<Print>(std::move(inner));
    }
};

struct BlockParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "BlockParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Ahead;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPeekType(TokenType1::OCB);
    }

    MilaResult<ASTNode1> parse(MilaParser& parser) const override {
        auto block = TRY(parser.parseBody());

        return makeStuff2<Block>(std::move(block));
    }
};

struct BinaryParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "BinaryParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Around;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPrevExp() && parser.isPeek([&](auto& tok) { return toType(tok.type).has_value(); });
    }

    MilaResult<ASTNode1> parse(MilaParser& parser) const override {
        auto lhs = TRY(parser.popExpr());

        auto tok = toType(TRY(parser.consumeToken()).type);
        assert(tok.has_value());

        TRY(parser.parseBinaryArm());
        auto rhs = TRY(parser.popExpr());

        // is there any other binary?
        if (not parser.hasToken() || not toType(parser.getToken()->type).has_value()) {
            return makeStuff2<Binary>(tok->first, std::move(lhs), std::move(rhs));
        }
        // yes

        auto typ1 = toType(parser.getToken()->type);

        if (typ1->second > tok->second) {
            parser.pushExp(std::move(rhs));
            TRY(parser.parseOne(LookDirection::Around));
            auto newRhs = TRY(parser.popExpr());

            return makeStuff2<Binary>(tok->first, std::move(lhs), std::move(newRhs));
        } else {
            parser.pushExp(makeStuff2<Binary>(tok->first, std::move(lhs), std::move(rhs)));
            TRY(parser.parseOne(LookDirection::Around));
            return TRY(parser.popExpr());
        }
    }
};

struct FieldParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "FieldParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Behind;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPrevExp() && parser.isPeekType(TokenType1::Dot);
    }

    MilaResult<ASTNode1> parse(MilaParser& parser) const override {
        auto subject = TRY(parser.popExpr());

        assertToken(TokenType1::Dot);

        auto fieldName = TRY(parser.assertIdent());

        return makeStuff2<FieldAccess>(std::move(subject), std::move(fieldName));
    }
};

struct NegateParsingUnit: MilaParsingUnit {
    const std::string_view className() const override {
        return "NegateParsingUnit";
    }

    LookDirection lookDirection() const override {
        return LookDirection::Ahead;
    }

    bool canParse(const MilaParser& parser) const override {
        return parser.isPeekType(TokenType1::Negate);
    }

    MilaResult<ASTNode1> parse(MilaParser& parser) const override {
        assertToken(TokenType1::Negate);

        auto inner = TRY(parser.parseExpression());

        return makeStuff2<Negate>(std::move(inner));
    }
};

vector<unique_ptr<MilaParsingUnit>> getMilaParsingUnits() {
    vector<unique_ptr<MilaParsingUnit>> units;

    units.push_back(make_unique<FunctionCallParsingUnit>());
    units.push_back(make_unique<FunctionParsingUnit>());

    units.push_back(make_unique<IFParsingUnit>());
    units.push_back(make_unique<IdentParsingUnit>());
    units.push_back(make_unique<BinaryParsingUnit>());
    units.push_back(make_unique<NumericParsingUnit>());
    units.push_back(make_unique<AssignParsingUnit>());
    units.push_back(make_unique<VariableDeclarationParsingUnit>());
    units.push_back(make_unique<BracketsParsingUnit>());
    units.push_back(make_unique<ReturnParsingUnit>());
    units.push_back(make_unique<BlockParsingUnit>());
    units.push_back(make_unique<PrintParsingUnit>());
    units.push_back(make_unique<StringLiteralParsingUnit>());
    units.push_back(make_unique<BoolLiteralParsingUnit>());
    units.push_back(make_unique<WhileParsingUnit>());
    units.push_back(make_unique<ForParsingUnit>());
    units.push_back(make_unique<ClassParsingUnit>());
    units.push_back(make_unique<FieldParsingUnit>());
    units.push_back(make_unique<NilLiteralParsingUnit>());
    units.push_back(make_unique<NegateParsingUnit>());
    units.push_back(make_unique<PrefixMinusParsingUnit>());
    units.push_back(make_unique<ThisSuperParsingUnit>());

    return units;
}

struct StackFrame;
struct LoxValue;
struct ObjectRef;

struct FunctionRef {
    FunctionRef* parent;
    Function* func;
    size_t argCount;
    void* fPtr;
    LoxValue* captures[];

    LoxValue read(size_t id);

    void write(size_t id, LoxValue val);

    void writeConst(size_t id, LoxValue value);

    LoxValue readConst(size_t id);

    void setThis(ObjectRef* self);

    size_t calculateSize();
};

// closed variables are allocated separately on heap (boxed)
// if closed variables are arguments, we move them into boxes
// we hook variable accesses to assign/read to boxes
// we will only read upvalus of our parent function
// we need to propagate captured values

constexpr size_t CONSTRUCTOR_ID = 0;

struct __attribute__ ((packed)) EntryPair {
    size_t first;
    size_t second;
};

struct  LoxMapBucket {
    size_t size = 0;
    EntryPair items[];
};

struct LoxMapBucketArray {
    size_t size;
    LoxMapBucket* buckets[];
};

struct LoxMap {
    static constexpr size_t BUCKET_SIZE = 2;
    static constexpr size_t INIT_SIZE = 4;
    static constexpr size_t INVALID_VALUE = -1;
    static constexpr size_t BBUCKET_SIZE = sizeof(LoxMapBucket)+LoxMap::BUCKET_SIZE*sizeof(EntryPair);
    static constexpr size_t BUCKET_ARRAY_BASE_SIZE = sizeof(LoxMapBucketArray)+INIT_SIZE*sizeof(LoxMapBucket*);

    LoxMapBucketArray* bucks = nullptr;
};

template<typename FN>
void forEachBucketArray(LoxMapBucketArray* map, FN&& fn) {
    if (map == nullptr) return;

    for (auto i = 0ul; i < map->size; i++) {
        auto bucket = map->buckets[i];
        if (bucket == nullptr) continue;

        for (auto j = 0ul; j < bucket->size; j++) {
            fn(bucket->items[j].second);
        }
    }
}

template<typename FN>
void forEachMap(LoxMap* map, FN&& fn) {
    if (map->bucks == nullptr) return;
    forEachBucketArray(map->bucks, fn);
}

template<typename FN>
void forEachBucket(LoxMapBucket* bucket, FN&& fn) {
    for (auto j = 0ul; j < bucket->size; j++) {
        fn(bucket->items[j].second);
    }
}

void dump(LoxMap* map) {
    for (auto i = 0ul; i < map->bucks->size; i++) {
        println("== BUCKET {}", i);
        LoxMapBucket* b = map->bucks->buckets[i];
        if (b == nullptr) continue;
        for (auto j = 0ul; j < b->size; j++) {
            println("== key: {}, value: {}", (size_t)b->items[j].first, (size_t)b->items[j].second);
        }
    }
}

LoxMapBucket* allocBucket() {
    auto bucket = allocateTyped<LoxMapBucket>(LoxMap::BBUCKET_SIZE, AllocType::HASH_MAP_BUCKET);
    bucket->size = 0;

    return bucket;
}

void resize(LoxMap* map) {
    // std::cout << "RESIZE " << map->size << " " << map->size*4 << std::endl;
    auto oldSize = map->bucks == nullptr ? 0 : map->bucks->size;

    auto newSizeBytes = std::max((sizeof(LoxMapBucketArray)+(oldSize*sizeof(LoxMapBucket*)))*2, LoxMap::BUCKET_ARRAY_BASE_SIZE);
    auto newItemCount = (newSizeBytes - sizeof(LoxMapBucketArray))/(sizeof(LoxMapBucket*));

    auto* newBukcets = allocateTyped<LoxMapBucketArray>(newSizeBytes, AllocType::HASH_MAP_BUCKET_ARRAY);
    std::memset(newBukcets, 0, newSizeBytes);
    newBukcets->size = newItemCount;

    for (auto i = 0ul; i < oldSize; i++) {
        auto oldBucket = map->bucks->buckets[i];
        if (oldBucket  == nullptr) continue;
        for (auto j = 0ul; j < oldBucket->size; j++) {
            auto entry = oldBucket->items[j];
            auto newBucketId = entry.first % newItemCount;
            auto& newBucket = newBukcets->buckets[newBucketId];
            if (newBucket == nullptr) {
                newBukcets->buckets[newBucketId] = allocBucket();
                newBucket = newBukcets->buckets[newBucketId];
            }
            newBucket->items[newBucket->size++] = entry;
        }
    }

    map->bucks = newBukcets;
}

size_t readMap(LoxMap* map, size_t id) {
    if (map->bucks == nullptr)
        return LoxMap::INVALID_VALUE;

    auto bucket = map->bucks->buckets[id % map->bucks->size];

    if (bucket == nullptr)
        return LoxMap::INVALID_VALUE;

    for (auto i = 0ul; i < bucket->size; i++) {
        auto entry = bucket->items[i];
        if (entry.first == id) return entry.second;
    }

    return LoxMap::INVALID_VALUE;
}

void writeMap(LoxMap* map, size_t id, size_t value) {
    assert(map != nullptr);
    // println("MAP ADDR IZ {}", map);
    assert(not IS_POISONED(map));
    if (map->bucks == nullptr) {
        resize(map);
    }
    // println("MAP BUCKZ {}", map->bucks);
    assertIsValidPtr(map->bucks);
    auto bucket = map->bucks->buckets[id % map->bucks->size];

    if (bucket == nullptr) {
        bucket = allocBucket();
        // println("MAP ADDR IZZZZZZZZZZZZZZ {}", map);
        assertIsValidPtr(map->bucks);
        assert(map->bucks != nullptr);
        map->bucks->buckets[id % map->bucks->size] = bucket;
    } else if (bucket->size == LoxMap::BUCKET_SIZE) {
        resize(map);

        bucket = map->bucks->buckets[id % map->bucks->size];

        if (bucket == nullptr) {
            bucket = allocBucket();
            map->bucks->buckets[id % map->bucks->size] = bucket;
        }
    }

    for (auto i = 0ul; i < bucket->size; i++) {
        if (bucket->items[i].first == id) {
            bucket->items[i].second = value;
            return;
        }
    }
    assert(bucket->size < LoxMap::BUCKET_SIZE);

    bucket->items[bucket->size++] = {(u32)id, value};
}

struct ClassRef {
    Class* clazz;
    ClassRef* super;
    FunctionRef* parent;
    LoxMap methods;

    Function* getConstructor() {
        auto res = clazz->getMethod(CONSTRUCTOR_ID);

        if (res != nullptr)  return res;

        if (super != nullptr) return super->getConstructor();;

        return nullptr;
    }
};

struct ObjectRef;

struct LoxStr {
    size_t size;
    char cString[];

    size_t calculateSize() {
        return align(sizeof(LoxStr)+(size+1), 16);
    }
};

struct LoxValue {
    static constexpr uint64_t NAN_MASK  = 0x7FFC000000000000; // 13 bits
    static constexpr uint64_t TAG_MASK  = 0x8003000000000000; // 3 bits
    static constexpr uint64_t DATA_MASK = 0x0000ffffffffffff; // 48 bits
    static constexpr uint64_t INV_DATA_MASK = ~0x0000ffffffffffff;

#if 1
    enum ValueType2: uint64_t {
        FLOAT =        0x0000000000000000, // 0
        FUNCTION_REF = 0x0001000000000000, // 1
        NIL =          0x0002000000000000, // 2
        BOOL =         0x0003000000000000, // 3
        STRING =       0x8000000000000000, // 4
        CLASS =        0x8001000000000000, // 5
        INSTANCE =     0x8002000000000000, // 6
        BOOL_FALSE =   0x8003000000000000  // 7
    };

    static bool isNan(uint64_t value) {
        return (value & NAN_MASK) == NAN_MASK;
    }

    static ValueType2 toType(uint64_t v) {
        if (!isNan(v)) { // not nan must be number i guess?
            return ValueType2::FLOAT;
        }
        auto tagBits = (v & TAG_MASK);

        return bit_cast<ValueType2>(tagBits);
    }

    static bool decodeBool(uint64_t v) {
        return toType(v) == ValueType2::BOOL;
    }

    static uint64_t encodeBool(bool v) {
        return NAN_MASK | (v ? bit_cast<uint64_t>(ValueType2::BOOL) : bit_cast<uint64_t>(ValueType2::BOOL_FALSE));
    }

    static void* decodePointer(uint64_t v) {
        return bit_cast<void*>((v & DATA_MASK)/* << 2*/);
    }

    static uint64_t encodePointer(uint64_t tag, void* ptr1) {
        auto ptr = bit_cast<uint64_t>(ptr1);
        // assert((ptr & 3) == 0);
        // assert(((ptr >> 2) & INV_DATA_MASK) == 0);
        // assert(((ptr >> 2) & DATA_MASK) == (ptr >> 2));
        // assert((DATA_MASK & NAN_MASK & tag) == 0);
        return tag | NAN_MASK | (ptr/* >> 2*/);
    }

    static uint64_t encodeNil() {
        return NAN_MASK | bit_cast<uint64_t>(ValueType2::NIL);
    }

    uint64_t internal;

    ValueType2 getType() const {
        return toType(internal);
    }

    double asNumber() const {
        return std::bit_cast<double>(internal);
    }

    string_view asString() const {
        auto s = (LoxStr*)decodePointer(internal);
        return string_view{s->cString, s->size};
    }

    LoxStr* asLoxStr() const {
        return (LoxStr*)decodePointer(internal);
    }

    FunctionRef* asFunction() const {
        assert(isFunction());
        return (FunctionRef*)decodePointer(internal);
    }

    bool asBool() const {
        return decodeBool(internal);
    }

    ObjectRef* asObject() const {
        return (ObjectRef*)decodePointer(internal);
    }

    ClassRef* asClass() const {
        return (ClassRef*)decodePointer(internal);
    }

    static LoxValue Bool(bool v) {
        return {encodeBool(v)};
    }

    static LoxValue Number(double v) {
        return {bit_cast<uint64_t>(v)};
    }

    static LoxValue Function(FunctionRef* v) {
        return {encodePointer((uint64_t)ValueType2::FUNCTION_REF, v)};
    }

    static LoxValue Nil() {
        return {encodeNil()};
    }

    static LoxValue BitZero() {
        return LoxValue{0};
    }

    static LoxValue String(LoxStr* s) {
        return{encodePointer((uint64_t)ValueType2::STRING, s)};
    }

    static LoxValue Class(ClassRef* ref) {
        return {encodePointer((uint64_t)ValueType2::CLASS, ref)};
    }

    static LoxValue Object(ObjectRef* ref) {
        return {encodePointer((uint64_t)ValueType2::INSTANCE, ref)};
    }

#else
    enum ValueType1 {
    FLOAT,
    FUNCTION_REF,
    NIL,
    BOOL,
    STRING,
    CLASS,
    INSTANCE,
    BOOL_FALSE // unused
};

    ValueType1 v;
    union {
        double flot;
        FunctionRef* ref;
        ClassRef* classRef;
        ObjectRef* objectRef;
        bool buul;
        string* str;
    };

    ValueType1 getType() const {
        return v;
    }

    double asNumber() const {
        return flot;
    }

    string_view asString() const {
        return string_view{*str};
    }

    FunctionRef* asFunction() const {
        return ref;
    }

    bool asBool() const {
        return buul;
    }

    ObjectRef* asObject() const {
        return objectRef;
    }

    ClassRef* asClass() const {
        return classRef;
    }

    static LoxValue Bool(bool v) {
        return LoxValue{.v=BOOL, .buul=v};
    }

    static LoxValue Number(double v) {
        return LoxValue{.v=FLOAT, .flot=v};
    }

    static LoxValue Function(FunctionRef* v) {
        return LoxValue{.v=FUNCTION_REF, .ref=v};
    }

    static LoxValue Nil() {
        return LoxValue{.v=NIL};
    }

    static LoxValue String(string_view s) {
        return LoxValue{.v=STRING, .str=new string(s)};
    }

    static LoxValue Class(ClassRef* ref) {
        return LoxValue{.v=CLASS, .classRef=ref};
    }

    static LoxValue Object(ObjectRef* ref) {
        return LoxValue{.v=INSTANCE, .objectRef=ref};
    }
#endif

    static LoxValue True() {
        return LoxValue::Bool(true);
    }

    static LoxValue False() {
        return LoxValue::Bool(false);
    }

    bool matchesType(LoxValue other) const {
        return this->getType() == other.getType();
    }

    bool isNumber() const {
        return getType() == FLOAT;
    }

    bool isString() const {
        return getType() == STRING;
    }

    bool isNil() const {
        return getType() == NIL;
    }

    bool isBool() const {
        return getType() == BOOL || getType() == BOOL_FALSE;
    }

    bool isClass() const {
        return getType() == CLASS;
    }

    bool isObject() const {
        return getType() == INSTANCE;
    }

    bool isFunction() const {
        return getType() == FUNCTION_REF;
    }

    string toString() const;

    bool toBool() const {
        switch (getType()) {
            case FLOAT:
            case CLASS:
            case INSTANCE:
            case FUNCTION_REF:
            case STRING:
            case BOOL:
                return true;
            case NIL:
            case BOOL_FALSE:
                return false;
        }
        PANIC()
    }
};

LoxValue FunctionRef::read(size_t id) {
    auto v = captures[id];
    if (v == nullptr) return LoxValue::Nil();

    return *v;
}

void FunctionRef::write(size_t id, LoxValue val) {
    auto v = captures[id];
    if (v == nullptr) PANIC();
    *v = val;
}

struct ASTExecutor;

FunctionRef* createMethod(Function* f1, FunctionRef* parent, ObjectRef* self);

struct ObjectRef {
    ClassRef* clazz;
    ObjectRef* proto;
    FunctionRef* construcor;
    LoxValue proto1;
    LoxMap* fields;

    FunctionRef* getRawMethod(u32 m, bool doCrimes) {
        ObjectRef* me = this;
        while (me != nullptr) {
            auto res = readMap(&me->clazz->methods, m);
            if (res != LoxMap::INVALID_VALUE) {
                auto r = std::bit_cast<FunctionRef*>(res);
                if (doCrimes) r->setThis(me);
                return r;
            }
            me = me->proto;
        }
        return nullptr;
    }

    LoxValue read(u32 name) {
        auto r = readMap(fields, name);
        if (r != LoxMap::INVALID_VALUE) return std::bit_cast<LoxValue>(r);
        return getMethod2(name);
    }

    void write(u32 name, LoxValue v) {
        writeMap(fields, name, std::bit_cast<size_t>(v));
    }

    LoxValue getMethod2(u32 name) {
        auto m = clazz->clazz->getMethod(name);
        if (m == nullptr && proto != nullptr) {
            return proto->getMethod2(name);
        }
        if (m == nullptr) PANIC();

        return LoxValue::Function(createMethod(m, clazz->parent, this));
    }
};

size_t toUpvalId(const vector<bool>& locals, size_t id) {
    size_t acu = 0;
    for (auto [i, isUp] : locals | views::enumerate) {
        if ((size_t)i == id) {
            assert(isUp);
            return acu;
        }
        if (isUp) acu += 1;
    }
    UNREACHABLE();
}

size_t calcUpValCount(const vector<bool>& locals) {
    size_t acu = 0;
    for (auto [i, isUp] : locals | views::enumerate) {
        if (isUp) acu += 1;
    }
    return acu;
}

size_t toLocalId(const vector<bool>& locals, size_t id) {
    size_t acu = 0;
    for (auto [i, isUp] : locals | views::enumerate) {
        if ((size_t)i == id) {
            assert(not isUp);
            return acu;
        }
        if (not isUp) acu += 1;
    }
    UNREACHABLE();
}

auto FIELD_LOOKUP = new std::unordered_map<std::string_view, u32>();

std::string_view idToName(u32 id) {
    for (auto& xd : *FIELD_LOOKUP) {
        if (xd.second == id) return xd.first;
    }
    PANIC();
}

// GOALS:
// determine the capture chain of functions
// translate local names to index in parameter table
// - deshadow locals
// - determine if local is captured
// assign integer value to unique field names
struct Linerizer: ASTVisitor {
    enum class IdentOp {
        READ,
        WRITE,
        DECLARE
    };

    std::unordered_map<std::string_view, u32>* fieldIdLookup;

    struct HookData {
        SpecTarget* toPatch;
        Function* source;
        size_t relFrameId;
        size_t relLocalId;
        IdentOp isDecl;
        Function* sourceUpFunction;
        std::string name;

        bool isEscaped() {
            return source->locals[relLocalId];
        }

        bool isModified() {
            assert(relLocalId < source->isModified.size());
            return source->isModified[relLocalId];
        }
    };
    vector<HookData> hookList;

    struct LexScope {
        vector<pair<string, size_t>> vars;

        optional<size_t> getLocal(const string& s) {
            for (const auto& var : vars) {
                if (var.first == s) return var.second;
            }
            return {};
        }
    };

    struct StackScope {
    private:
        vector<bool> isUpVal;
        vector<bool> isModified;
    public:
        void init(Function* f) {
            assert(f->locals.empty());
            assert(f->isModified.empty());
            f->locals = isUpVal;
            f->isModified = isModified;
        }

        size_t rawRegisterLocal() {
            isUpVal.push_back(false);
            isModified.push_back(false);

            return isUpVal.size()-1;
        }

        Function* function;
        vector<LexScope> lexicals;
        bool isGlobal = false;

        optional<size_t> getLocal(const string& s) {
            for (auto& scope : lexicals | views::reverse) {
                auto l = scope.getLocal(s);
                if (l.has_value()) return l;
            }
            return {};
        }

        size_t putLocal(const string& s) {
            assert(not lexicals.empty());
            auto cur = lexicals.back().getLocal(s);
            if (isGlobal && cur.has_value()) return *cur;
            if (cur.has_value()) PANIC("local {} already defiend in curren scope", s);

            auto id = rawRegisterLocal();
            lexicals.back().vars.emplace_back(s, id);

            return id;
        }

        size_t putRootLocal(const string& s) {
            assert(not lexicals.empty());
            auto cur = lexicals[0].getLocal(s);
            if (cur.has_value()) PANIC("local {} already defiend in curren scope", s);

            auto id = rawRegisterLocal();
            lexicals[0].vars.emplace_back(s, id);

            return id;
        }

        void enterScope() {
            lexicals.emplace_back();
        }

        void exitScope() {
            assert(not lexicals.empty());
            lexicals.pop_back();
        }

        void markUpVal(size_t id) {
            isUpVal[id] = true;
        }

        void markModified(size_t id) {
            isModified[id] = true;
        }

        size_t toUpvalId(size_t id) {
            size_t acu = 0;
            for (auto [i, isUp] : isUpVal | views::enumerate) {
                if ((size_t)i == id) {
                    assert(isUp);
                    return acu;
                }
                if (isUp) acu += 1;
            }
            UNREACHABLE();
        }

        size_t toLocalId(size_t id) {
            size_t acu = 0;
            for (auto [i, isUp] : isUpVal | views::enumerate) {
                if ((size_t)i == id) {
                    assert(not isUp);
                    return acu;
                }
                if (not isUp) acu += 1;
            }
            UNREACHABLE();
        }

        size_t upValCount() {
            size_t acu = 0;
            for (auto [i, isUp] : isUpVal | views::enumerate) {
                if (not isUp) acu += 1;
            }
            return acu;
        }

        size_t localCount() {
            return isUpVal.size()-upValCount();
        }
    };

    vector<StackScope> stack;
    StackScope globals;

    void realFix() {
        for (auto f : hookList) {
            if (f.relFrameId == 0) { // self
                auto self = f.source;
                auto isEscaped = f.isEscaped();
                if (isEscaped && f.isDecl == IdentOp::DECLARE) { // declaration of escaping upVal (redeclare it/alocate new one)
                    if (f.isModified()) {
                        *f.toPatch = SpecializedVariable(HookedVariableType::ALLOC_UPVAL, f.source->getUpValId(f.relLocalId), 0);
                    } else {
                        *f.toPatch = SpecializedVariable(HookedVariableType::ALLOC_CONST_UPVAL, f.source->getUpValId(f.relLocalId), 0);
                    }
                } else if (isEscaped) { // access to localy defined upval
                    if (f.isModified()) {
                        *f.toPatch = SpecializedVariable(HookedVariableType::UPVAL, self->getUpValId(f.relLocalId), 0);
                    } else {
                        *f.toPatch = SpecializedVariable(HookedVariableType::CONST_UPVAL, self->getUpValId(f.relLocalId), 0);
                    }
                } else { // access to ordinary local
                    *f.toPatch = SpecializedVariable(HookedVariableType::LOCAL, self->getLocalId(f.relLocalId), 0);
                }
            } else if (f.relFrameId == 1) { // capturing parents local
                auto self = f.sourceUpFunction;
                assert(f.isDecl != IdentOp::DECLARE);

                if (f.isModified()) {
                    auto localId = self->putCapture(f.relLocalId);

                    *f.toPatch = SpecializedVariable(HookedVariableType::UPVAL, self->getUpValId(localId), 0); // offset 0, we copied upval to our frame
                } else {
                    assert(f.isDecl != IdentOp::WRITE);
                    auto localId = self->putCapture(f.relLocalId);

                    *f.toPatch = SpecializedVariable(HookedVariableType::CONST_UPVAL, self->getUpValId(localId), 0); // offset 0, we copied upval to our frame
                }
            } else { // capturing nested local
                auto closestChild = f.sourceUpFunction;
                assert(f.isDecl != IdentOp::DECLARE);

                if (f.isModified()) {
                    auto someParentsId = closestChild->putCapture(f.relLocalId);

                    *f.toPatch = SpecializedVariable(HookedVariableType::UPVAL, closestChild->getUpValId(someParentsId), f.relFrameId-1); // relFrameId-1, we access version stored in nodes child
                } else {
                    // FIXME
                    assert(f.isDecl != IdentOp::WRITE);
                    auto someParentsId = closestChild->putCapture(f.relLocalId);

                    *f.toPatch = SpecializedVariable(HookedVariableType::CONST_UPVAL, closestChild->getUpValId(someParentsId), f.relFrameId-1); // relFrameId-1, we access version stored in nodes child
                }
            }
            if (DEBUG_JIT) {
                switch (f.isDecl) {
                    case IdentOp::READ: std::cout << "READING "; break;
                    case IdentOp::WRITE: std::cout << "WRITING "; break;
                    case IdentOp::DECLARE: std::cout << "DECLARING "; break;
                }
                std::cout << f.name << " TO " << f.toPatch->toString() << std::endl;
            }
        }
    }

    void invoke(Return& it) override {
        if (it.data.value.has_value()) {
            hook(*it.data.value);
        }
    }

    void invoke(Print& it) override {
        hook(it.data.value);
    }

    void invoke(Call& it) override {
        hook(it.fName);
        for(auto& arg : it.args) {
            hook(arg);
        }
    }

    void invoke(NilLiteral& it) override {}

    void invoke(IntLiteral& it) override {}

    void invoke(Binary& it) override {
        hook(it.data.lhs);
        hook(it.data.rhs);
    }

    void invoke(Super& it) override {
        hookIdent("__this", &it.hookedTarget, IdentOp::READ);
    }

    void invoke(This& it) override {
        hookIdent("__this", &it.hookedTarget, IdentOp::READ);
    }

    u32 getFieldId(std::string_view fieldName) {
        if (fieldIdLookup->contains(fieldName)) {
            return fieldIdLookup->at(fieldName);
        } else {
            auto id = fieldIdLookup->size();
            fieldIdLookup->emplace(fieldName, id);
            return id;
        }
    }

    void invoke(FieldAccess &it) override {
        it.fieldId = getFieldId(it.data.fieldName);
        hook(it.data.subject);
    }

    bool isInRealGlobal() {
        return stack.empty() or (stack.size() == 1 and stack.back().lexicals.size() == 1);
    }

    void putIdent(string_view name1) {
        if (isInRealGlobal()) {
            globals.putLocal(string(name1));
        } else {
            stack.back().putLocal(string(name1));
        }
    }

    void invoke(Class& it) override {
        putIdent(it.data.name);

        hookIdent(it.data.name, &it.hookedDst, IdentOp::DECLARE);

        if (it.data.superClass.has_value()) {
            hookIdent(*it.data.superClass, &it.hookedSuper, IdentOp::READ);
        }

        for (auto& method : it.data.methods) {
            it.methodIds[getFieldId(method->data.name)] = method;
            stack.emplace_back();
            stack.back().lexicals.emplace_back();
            stack.back().function = method/*.get()*/;

            stack.back().markUpVal(stack.back().putLocal("__this"));

            for (auto a : method->data.argz) {
                stack.back().putLocal(a);
            }

            for (auto& s : method->data.body) {
                hook(s);
            }

            stack.back().init(method);

            stack.pop_back();
        }
    }

    void invoke(StringLiteral1& it) override {}

    void invoke(Negate& it) override {
        hook(it.data.inner);
    }

    optional<size_t> getGlobal(const string& s) {
        return globals.getLocal(s);
    }

    map<VariableDeclaration*, ASTStm> declarationsToPatch;

    void hookIdent(const string& name, SpecTarget* hookedTarget, IdentOp isDecl) {
        if (VERBOSE) println("VISITING identifier {}", name);
        if (isInRealGlobal()) {
            auto g = getGlobal(name);
            if (not g.has_value()) {
                g = globals.putRootLocal(name);
            }

            *hookedTarget = SpecializedVariable(HookedVariableType::GLOBAL, *g, 0);

            return;
        }

        for (auto [i, scope] : stack | views::reverse | views::enumerate) {
            auto up = scope.getLocal(name);
            if (up.has_value()) {
                if (i != 0) scope.markUpVal(*up);
                if (isDecl == IdentOp::WRITE) {
                    scope.markModified(*up);
                }

                putPatch(hookedTarget, scope.function, i, *up, isDecl, (i == 0) ? nullptr : stack[stack.size()-i].function, name);
                return;
            }
        }

        auto g = globals.getLocal(name);
        if (g.has_value()) {
            // generate global assign wrapper
            *hookedTarget = SpecializedVariable(HookedVariableType::GLOBAL, *g, 0);
            return;
        }

        // global var hasent been declared yet i guess
        *hookedTarget = SpecializedVariable(HookedVariableType::GLOBAL, globals.putRootLocal(name), 0);
        // PANIC("variable {} never defined", it.data.value);
    }

    void invoke(Identifier& it) override {
        hookIdent(it.data.value, &it.data.hookedTarget, IdentOp::READ);
    }

    void hook(ASTExpr& tgt) {
        tgt->visit(*this);
    }

    void hook(ASTStm& tgt) {
        tgt->visit(*this);
    }

    void putPatch(SpecTarget* toPatch1, Function* source, size_t relFrameId, size_t relLocalId, IdentOp isDecl, Function* up, string_view name) {
        hookList.emplace_back(toPatch1, source, relFrameId, relLocalId, isDecl, up, string(name));
    }

    void invoke(VariableDeclaration& it) override {
        if (VERBOSE) println("VISITING VariableDeclaratio {}", it.data.dst);

        if (it.data.value.has_value()) {
            hook(*it.data.value);
        }

        putIdent(it.data.dst);
        hookIdent(it.data.dst, &it.data.hookedTarget, IdentOp::DECLARE);
    }

    void invoke(Block& it) override {
        stack.back().enterScope();

        for (auto& s : it.data.statements) {
            hook(s);
        }

        stack.back().exitScope();
    }

    void invoke(IF& it) override {
        stack.back().enterScope();

        hook(it.data.cond);

        stack.back().enterScope();
        hook(it.data.ifBody);
        stack.back().exitScope();

        if (it.data.elsBody.has_value()) {
            stack.back().enterScope();
            hook(*it.data.elsBody);
            stack.back().exitScope();
        }

        stack.back().exitScope();
    }

    void invoke(While& it) override {
        it.data.body->visit(*this);

        if (it.data.cond.has_value()) hook(*it.data.cond);
    }

    void invoke(Function& it) override {
        putIdent(it.data.name);
        hookIdent(it.data.name, &it.hookedTarget, IdentOp::DECLARE);

        stack.emplace_back();
        stack.back().lexicals.emplace_back();
        stack.back().function = &it;

        for (auto& n : it.data.argz) {
            stack.back().putLocal(n);
        }

        for (auto& idk : it.data.body) {
            hook(idk);
        }

        stack.back().init(&it);
        stack.pop_back();
    }

    void invoke(Assign& it) override {
        hook(it.data.value);
        auto dst = it.data.dst/*.get()*/;

        if (dynamic_cast<Identifier*>(dst) != nullptr) {
            auto& ident = dynamic_cast<Identifier*>(dst)->data;
            auto name = ident.value;
            if (VERBOSE) println("VISITING assing {}", name);
            hookIdent(name, &ident.hookedTarget, IdentOp::WRITE);
        } else if (dynamic_cast<FieldAccess*>(dst) != nullptr) {
            hook(it.data.dst);
        } else {
            PANIC("CANT ASSING TO STUFF");
        }
    }

    void invoke(BoolLiteral1& it) override {

    }


    void invoke(ASTNode& it) override {
        TODO();
    }
};

LoxValue GLOBALS_TABLE[512];

LoxStr* allocateEmptyLoxString(size_t size) {
    auto idk = allocateTyped<LoxStr>(sizeof(LoxStr)+size+1, AllocType::STRING);
    idk->size = size;
    idk->cString[size] = 0;

    return idk;
}

LoxStr* allocateLoxString(std::string_view s) {
    auto idk = allocateEmptyLoxString(s.size());
    std::memcpy(idk->cString, s.data(), s.size());

    return idk;
}

// FIXME could be replace with jited countrapart to get rid of call overhead
namespace builtin {
    void writeClosed(FunctionRef* v, size_t fId, size_t lId, LoxValue o) {
        if (DEBUG_JIT) println("[writeClosed] {} {}:{} {}", v->func->data.name, fId, lId, o.toString());
        auto c = v;
        for (size_t i = 0; i < fId; i++) {
            c = c->parent;
        }
        (*c->captures)[lId] = o;
    }

    LoxValue readClosed(FunctionRef* v, size_t fId, size_t lId, bool isConst) {
        if (DEBUG_JIT) println("[readClosed] {} {}:{}", v->func->data.name, fId, lId);
        auto c = v;
        for (size_t i = 0; i < fId; i++) {
            c = c->parent;
        }

        return *c->captures[lId];
    }

    void copyClosed(LoxValue dst, FunctionRef* src, size_t dstId, size_t srcId) {
        assert(dst.isFunction());
        if (DEBUG_JIT) println("[copyClosed] {}@{} <- {}@{}", dst.asFunction()->func->data.name, dstId, src->func->data.name, srcId);
        dst.asFunction()->captures[dstId] = src->captures[srcId];
    }

    void allocateClosed(FunctionRef* v, size_t fId, size_t lId, LoxValue o) {
        auto c = v;
        if (DEBUG_JIT) println("[allocateClosed] {} {}:{} {}", v->func->data.name, fId, lId, o.toString());
        for (size_t i = 0; i < fId; i++) {
            c = c->parent;
        }

        auto v1 = allocateTypedSimple<LoxValue>(AllocType::LOX_VALUE);
        *v1 = o;
        c->captures[lId] = v1;
    }

    LoxValue readField(LoxValue subj, u32 id) {
        assert(subj.isObject());
        return subj.asObject()->read(id);
    }

    void writeField(LoxValue obj, LoxValue value, u32 id) {
        assert(obj.isObject());
        obj.asObject()->write(id, value);
    }

    void loxPrint(LoxValue value) {
        std::cout << value.toString() << std::endl;
    }

    size_t toBool(LoxValue value) {
        // std::cout << value.toBool() << std::endl;
        return value.toBool();
    }

    LoxValue* allocateLoxValue() {
        return allocateTypedSimple<LoxValue>(AllocType::LOX_VALUE);
    }

    LoxValue doSimpleBin(BinaryType type, LoxValue lhs, LoxValue rhs) {
    // if (lhs.asNumber() < 0.0) PANIC();
    // if (lhs.asNumber() > 1'000'000.0) PANIC();
    // std::cout << lhs.toString() << " " << binarToString(type) << " " << rhs.toString() << std::endl;
    LoxValue res;
    switch (type) {
        case BinaryType::ADD:
            if (rhs.isString()) {
                assert(lhs.isString());
                auto newString = allocateEmptyLoxString(lhs.asString().size() + rhs.asString().size());
                std::memcpy(newString->cString, lhs.asString().data(), lhs.asString().size());
                std::memcpy(newString->cString+lhs.asString().size(), rhs.asString().data(), rhs.asString().size());
                res = LoxValue::String(newString);
            } else {
                assert(rhs.isNumber());
                assert(lhs.isNumber());
                res = LoxValue::Number(lhs.asNumber() + rhs.asNumber());
            }
            break;
        case BinaryType::SUB:
            assert(rhs.isNumber());
            assert(lhs.isNumber());
            res = LoxValue::Number(lhs.asNumber() - rhs.asNumber());
            break;
        case BinaryType::DIV:
            assert(rhs.isNumber());
            assert(lhs.isNumber());
            res = LoxValue::Number(lhs.asNumber() / rhs.asNumber());
            break;
        case BinaryType::MOD:
            assert(rhs.isNumber());
            assert(lhs.isNumber());
            res = LoxValue::Number((double) ((long) lhs.asNumber() % (long) rhs.asNumber()));
            break;
        case BinaryType::MUL:
            assert(rhs.isNumber());
            assert(lhs.isNumber());
            res = LoxValue::Number(lhs.asNumber() * rhs.asNumber());
            break;
        case BinaryType::GT:
            assert(rhs.isNumber());
            assert(lhs.isNumber());
            res = LoxValue::Bool(lhs.asNumber() > rhs.asNumber());
            break;
        case BinaryType::LESS:
            assert(rhs.isNumber());
            assert(lhs.isNumber());
            res = LoxValue::Bool(lhs.asNumber() < rhs.asNumber());
            break;
        case BinaryType::EQ:
        case BinaryType::NEQ: {
            if (rhs.isNumber()) { // needed for nan equality
                res = LoxValue::Bool(rhs.asNumber() == lhs.asNumber());
            } else if (rhs.isString() && lhs.isString()) { // "structural equality"
                res = LoxValue::Bool(rhs.asString() == lhs.asString());
            } else { // referential equality
                res = LoxValue::Bool(rhs.internal == lhs.internal);
            }

            if (type == BinaryType::NEQ) {
                res = LoxValue::Bool(!res.asBool());
            }
            break;
        }
        case BinaryType::GEQ:
            assert(rhs.isNumber());
            assert(lhs.isNumber());
            res = LoxValue::Bool(lhs.asNumber() >= rhs.asNumber());
            break;
        case BinaryType::LEQ:
            assert(rhs.isNumber());
            assert(lhs.isNumber());
            res = LoxValue::Bool(lhs.asNumber() <= rhs.asNumber());
            break;
        case BinaryType::AND:
            break;
        case BinaryType::OR:
            break;
    }

    return res;
}

    ObjectRef* rawInstant(ClassRef* clazz, LoxMap* data) {
        ObjectRef* proto = nullptr;
        if (clazz->super != nullptr) {
            proto = rawInstant(clazz->super, data);
        }
        auto me = allocateTypedSimple<ObjectRef>(AllocType::OBJECT_REF);
        me->clazz = clazz;
        me->proto = proto;
        me->proto1 = proto == nullptr ? LoxValue::Nil() : LoxValue::Object(proto);
        me->fields = data;
        me->construcor = nullptr;
        if (clazz->getConstructor() != nullptr) me->construcor = me->getRawMethod(CONSTRUCTOR_ID, false);
        return me;
    }

    LoxValue instantiate(LoxValue clazz) {
        // println("instantiate {}", clazz);
        auto map = allocateTypedSimple<LoxMap>(AllocType::HASH_MAP);
        map->bucks = nullptr;
        auto* res = rawInstant(clazz.asClass(), map);
        // println("after");

        return LoxValue::Object(res);
    }

    FunctionRef* getMethod(LoxValue obj, u32 id, size_t argCount) {
        // println("getMethod {} {}@{} {}", obj.toString(), idToName(id), id, argCount);
        assert(obj.isObject());
        auto self = obj.asObject();
        auto r = readMap(self->fields, id);
        if (r != LoxMap::INVALID_VALUE) {
            auto m = std::bit_cast<LoxValue>(r);
            assert(m.isFunction());
            return m.asFunction();
        }
        auto m = self->getRawMethod(id, true);
        assert(m != nullptr);
        assert(m->argCount == argCount);

        return m;
    }

    LoxValue getConstructor(LoxValue clazz) {
        // println("getConstructor {}", clazz.toString());
        if (clazz.asObject()->clazz->getConstructor() != nullptr) {
            return clazz.asObject()->getMethod2(CONSTRUCTOR_ID);
        } else {
            return LoxValue::BitZero();
        }
    }

    bool hasConstructor(LoxValue clazz) {
        return clazz.asClass()->getConstructor() != nullptr;
    }

    // FIXME in order to make this work we need to JIT constructor specialy
    // NOTE: even classes without constructor will get synthetic constructor that will do the folowing:
    // 1. it will manually allocate object
    FunctionRef* getCallPtr(LoxValue callable, size_t argsCount) {
        // std::cout << GLOBALS_TABLE[1].toString() << std::endl;
        // std::cout << "CALLING: " << callable.toString() << std::endl;
        assert(callable.isFunction() or callable.isClass());

        if (callable.isFunction()) {
            assert(callable.asFunction()->func->data.argz.size() == argsCount);
            return callable.asFunction();
        }

        if (callable.isClass()) {
            assert(false);
            auto cons = callable.asClass()->getConstructor();
            if (cons == nullptr) {
                assert(argsCount == 0);
                return nullptr;
            }
            assert(cons->data.argz.size() == argsCount);

            return nullptr;
        }

        PANIC();
    }

    LoxValue allocateClosure(Function* f, FunctionRef* closure) {
        auto c = allocateTyped<FunctionRef>(sizeof(FunctionRef)+f->totalUpValCount()*sizeof(LoxValue*), AllocType::FUNCTION_REF);

        std::memset(c->captures, 0x0, f->totalUpValCount()*sizeof(LoxValue*));

        c->parent = closure;
        c->func = f;
        c->argCount = f->data.argz.size();
        c->fPtr = f->runtimeData;

        return LoxValue::Function(c);
    }

    LoxValue allocateClass(Class* clazz, LoxValue super, FunctionRef* frame) {
        // println("allocateClass {} - {} - {}", clazz, super, frame);
        ClassRef* sup = nullptr;
        if (super.isClass()) sup = super.asClass();
        auto claz = allocateTypedSimple<ClassRef>(AllocType::CLASS_REF);
        claz->clazz = clazz;
        claz->super = sup;
        claz->parent = frame;
        claz->methods.bucks = nullptr;
        for (auto [m, mId] : clazz->methodIds) {
            writeMap(&claz->methods, m, std::bit_cast<size_t>(createMethod(mId, frame, nullptr)));
            // std::cout << "PUTTING TO MAP " << m << " / " << mId->data.name << std::endl;
            // dump(&claz->methods);

            // FIXME methods can depend on captured value of class, which isn't yet set, try to patch them
            for (auto i = 0ul; i < mId->captures.size(); i++) {
                auto capture = mId->captures[i];

                if (capture == clazz->hookedDst.id) {
                    if (clazz->hookedDst.type == HookedVariableType::ALLOC_CONST_UPVAL) {
                        ((FunctionRef*)readMap(&claz->methods, m))->writeConst(mId->upValCount()+i, LoxValue::Class(claz));
                    } else {
                        TODO();
                    }
                }
            }
        }

        // println("allocateClass out {}", claz);

        return LoxValue::Class(claz);
    }

    LoxValue loxNegate(LoxValue v) {
        return LoxValue::Bool(!v.toBool());
    }
}

struct MilaAssembler: virtual Assembler {
    vector<string> stuff;

    struct Label {
        size_t id;

        string toString() {
            return stringify("new-label-{}", id);
        }
    };

    Label makeLabel1() {
        return Label{this->allocateJmpLabel()};
    }

    virtual void callMethod(size_t tgt1, size_t subj1, u32 fieldId, span<size_t> argz, std::optional<size_t> methodFrame) = 0;

    void bind(Label l) {
        this->createLabel(l.id);
    }

    size_t getId(string lol) {
        for (auto i = 0u; i < stuff.size(); i++) {
            if (stuff[i] == lol) return i;
        }

        stuff.push_back(lol);
        return stuff.size()-1;
    }

    // FIXME TODO THIS JUST WORKS FOR NAN-BOX
    void getPtr(size_t dst, size_t value) {
        movInt(dst, LoxValue::DATA_MASK);
        andInt(dst, dst, value);
    }

    void cJmp(Label l, JumpCondType t, size_t lhs, size_t rhs) {
        jmpCond(l.id, t, lhs, rhs);
    }

    void cJmp(Label l) {
        this->jmp(l.id);
    }

    void getLoxTag(size_t dst, size_t val) {
        auto done = makeLabel1();
        auto extractTag = makeLabel1();
        auto mask = movImmValueToReg(LoxValue::NAN_MASK);

        andInt(dst, val, mask);
        cJmp(extractTag, JumpCondType::EQUALS, dst, mask);
        { // its not nan we are float
            movInt(dst, LoxValue::ValueType2::FLOAT);
            cJmp(done);
        }
        { // extract tag
            bind(extractTag);
            movInt(mask, LoxValue::TAG_MASK);
            andInt(dst, val, mask);
        }

        bind(done);

        freeRegister(mask);
    }

    virtual void doBin(BinaryType type, size_t dst, size_t lhs, size_t rhs) = 0;

    virtual void negate(size_t dst, size_t src) = 0;

    virtual void readField(size_t tgt, size_t self, u32 id) = 0;

    virtual void writeField(size_t self, u32 id, size_t value) = 0;

    virtual void copyClosed(size_t dst, size_t src, size_t dstId, size_t srcId) = 0;

    virtual void readClosed(size_t dst, size_t ref, size_t frameId, size_t localId, bool isConst) = 0;

    virtual void writeClosed(size_t ref, size_t frameId, size_t localId, size_t value, bool isConst) = 0;

    virtual void allocateClosed(size_t ref, size_t frameId, size_t localId, size_t value, bool isConst) = 0;

    virtual void allocateClosure(size_t tgt, Function* f, size_t parent) = 0;

    virtual void allocateClass(size_t tgt, Class* clazz, size_t frame, std::optional<size_t> super) = 0;

    virtual void dynamicCall(size_t tgt, size_t subj, span<size_t> argz) = 0;

    virtual void print1(size_t arg) = 0;

    virtual void toBool(size_t tgt, size_t src) = 0;

    virtual void readGlobal(size_t tgt, size_t id) {
        movInt(tgt, std::bit_cast<size_t>(&GLOBALS_TABLE));
        readMem(tgt, tgt, id*sizeof(LoxValue), sizeof(LoxValue));
    }

    virtual void writeGlobal(size_t id, size_t value) {
        // FIXME this can be direct read without adend?
        auto gReg = movImmPtrToReg(&GLOBALS_TABLE);
        writeMem(gReg, value, id*sizeof(LoxValue), sizeof(LoxValue));

        freeRegister(gReg);
    }
};

struct X86MilaAssembler: virtual MilaAssembler, X86Assembler {
    using X86Assembler::X86Assembler;

    std::map<size_t, std::string> hints;

    size_t HINT_ID = 0;

    X86MilaAssembler(span<size_t> args, size_t ret): X86Assembler(args, ret) {
        HINT_ID = this->allocateLabelType();
    }

    void bindHint(std::string_view h) override {
        auto id = allocateLabel();
        this->bindRawLabel(id, HINT_ID);
        hints[id] = std::string(h);
    }

    void dumpHints(std::string_view s) override {
        std::ofstream idk1{std::string(s)};

        idk1 << hints.size() << std::endl;
        for (auto& hint : hints) {
            idk1 << getBoundLabelById(hint.first).offset << std::endl;
            idk1 << escape(hint.second) << std::endl;
        }

        idk1.close();
    }

    void readField(size_t tgt, size_t self, u32 id) override {
        array<Arg, 3> argz{handleToArg(self), Arg::Imm(id)};
        chadCall(Arg::ImmPtr((void*)&builtin::readField), argz, handleToArg(tgt));
    }

    void writeField(size_t self, u32 id, size_t value) override {
        array<Arg, 4> argz{handleToArg(self), handleToArg(value), Arg::Imm(id)};
        chadCall(Arg::ImmPtr((void*)&builtin::writeField), argz, {});
    }

    void cJmp1(Label l, JumpCondType t) {
        this->writeJmp(toCmpType(t), l.id);
    }

    void cJmp2(Label l, JumpCondType t) {
        this->writeJmp(toCmpType2(t), l.id);
    }

    void print1(size_t arg) override {
        array<Arg, 1> argz{handleToArg(arg)};
        // trap();
        chadCall(Arg::ImmPtr((void*)&builtin::loxPrint), argz, {});
    }

    void numberGuard(RegAllocCtx& alloc, size_t subjReg, size_t tmp1, size_t tmp2, Label doneLabel, Label crashLabel) {
        movInt(tmp1, LoxValue::NAN_MASK);
        movReg(tmp2, subjReg);
        mc.doNot(alloc.REG(tmp2));
        mc.writeRegInst(X64Instruction::Test, alloc.REG(tmp1), alloc.REG(tmp2));
        cJmp1(doneLabel, JumpCondType::NOT_EQUALS);

        movInt(tmp1, LoxValue::TAG_MASK);
        mc.writeRegInst(X64Instruction::Test, alloc.REG(subjReg), alloc.REG(tmp1));
        cJmp1(doneLabel, JumpCondType::NOT_EQUALS);
        cJmp(crashLabel);
    }

    void fastArith(size_t dst, size_t lhs, size_t rhs, ArithmeticOp op) {
        auto ctx = getAllocCtx();
        auto lReg = ctx.ensureReg(lhs);
        auto rReg = ctx.ensureReg(rhs);
        auto dstReg = ctx.ensureRegWriteback(dst);
        auto tmpReg = (dstReg == lReg or dstReg == rReg) ? ctx.allocReg() : dstReg;
        auto tmpReg1 = ctx.allocReg();

        auto lhsNumberLabel = makeLabel1();
        auto rhsNumberLabel = makeLabel1();
        auto crashLabel = makeLabel1();

        numberGuard(ctx, lReg, tmpReg, tmpReg1, lhsNumberLabel, crashLabel);

        bind(lhsNumberLabel);
        numberGuard(ctx, rReg, tmpReg, tmpReg1, rhsNumberLabel, crashLabel);

        bind(crashLabel);
        // trap();
        mc.hlt();

        bind(rhsNumberLabel);
        this->arithmeticFloat(op, FloatingPointType::Double, dstReg, lReg, rReg);

        ctx.restore();
    }

    void fastAdd(size_t dst, size_t lhs, size_t rhs, Label crashLabel, Label doneLabel) {
        auto ctx = getAllocCtx();
        auto lReg = ctx.ensureReg(lhs);
        auto rReg = ctx.ensureReg(rhs);
        auto dstReg = ctx.ensureRegWriteback(dst);
        auto tmpReg = (dstReg == lReg or dstReg == rReg) ? ctx.allocReg() : dstReg;
        auto tmpReg1 = ctx.allocReg();

        auto lhsNumberLabel = makeLabel1();
        auto rhsNumberLabel = makeLabel1();

        numberGuard(ctx, lReg, tmpReg, tmpReg1, lhsNumberLabel, crashLabel);

        bind(lhsNumberLabel);
        numberGuard(ctx, rReg, tmpReg, tmpReg1, rhsNumberLabel, crashLabel);

        bind(rhsNumberLabel);
        addDouble(dstReg, lReg, rReg);
        cJmp(doneLabel);

        ctx.restore();
    }

    void fastCmp(size_t dst, size_t lhs, size_t rhs, JumpCondType type) {
        auto ctx = getAllocCtx();
        auto lReg = ctx.ensureReg(lhs);
        auto rReg = ctx.ensureReg(rhs);
        auto dstReg = ctx.ensureRegWriteback(dst);
        auto tmpReg = (dstReg == lReg or dstReg == rReg) ? ctx.allocReg() : dstReg;
        auto tmpReg1 = ctx.allocReg();

        auto lhsNumberLabel = makeLabel1();
        auto rhsNumberLabel = makeLabel1();
        auto crashLabel = makeLabel1();
        auto isTrueLabel = makeLabel1();
        auto isDoneLabel = makeLabel1();

        numberGuard(ctx, lReg, tmpReg, tmpReg1, lhsNumberLabel, crashLabel);

        bind(lhsNumberLabel);
        numberGuard(ctx, rReg, tmpReg, tmpReg1, rhsNumberLabel, crashLabel);

        bind(crashLabel);
        // trap();
        mc.hlt();

        bind(rhsNumberLabel);
        // this->trap();
        mc.movq(0, allocator.getReg(lReg), true);
        mc.movq(1, allocator.getReg(rReg), true);
        mc.comisd(0, 1, true);
        // this->trap();
        cJmp2(isTrueLabel, type);
        movInt(dstReg, std::bit_cast<u64>(LoxValue::False()));
        cJmp(isDoneLabel);

        bind(isTrueLabel);
        movInt(dstReg, std::bit_cast<u64>(LoxValue::True()));

        bind(isDoneLabel);

        ctx.restore();
    }

    // FIXME broken
    void fastEq(size_t dst, size_t lhs, size_t rhs, size_t trueValue, size_t falseValue) {
        auto ctx = getAllocCtx();
        auto lReg = ctx.ensureReg(lhs);
        auto rReg = ctx.ensureReg(rhs);
        auto dstReg = ctx.ensureRegWriteback(dst);
        auto tmpReg = (dstReg == lReg or dstReg == rReg) ? ctx.allocReg() : dstReg;
        auto tmpReg1 = ctx.allocReg();

        auto lhsNumberLabel = makeLabel1();
        auto rhsNumberLabel = makeLabel1();
        auto crashLabel = makeLabel1();
        auto isTrueLabel = makeLabel1();
        auto isDoneLabel = makeLabel1();
        auto doRawCmp = makeLabel1();
        auto isTrue = makeLabel1();
        auto trulyDone = makeLabel1();

        auto checkNotNumber = makeLabel1();

        getLoxTag(tmpReg, lReg);
        movInt(tmpReg1, LoxValue::ValueType2::FLOAT);
        cJmp(checkNotNumber, JumpCondType::NOT_EQUALS, tmpReg, tmpReg1);
        mc.movq(0, allocator.getReg(lReg), true);
        mc.movq(1, allocator.getReg(rReg), true);
        mc.comisd(0, 1, true);
        cJmp(isDoneLabel);

        bind(checkNotNumber);
        movInt(tmpReg1, LoxValue::ValueType2::STRING);
        cJmp(doRawCmp, JumpCondType::NOT_EQUALS, tmpReg, tmpReg1);

        getLoxTag(tmpReg, rReg);
        cJmp(doRawCmp, JumpCondType::NOT_EQUALS, tmpReg, tmpReg1);

        mc.hlt(); // TODO
        cJmp(isDoneLabel);

        bind(doRawCmp);
        mc.writeRegInst(X64Instruction::cmp, ctx.REG(lReg), ctx.REG(rReg));
        // cJmp(isDoneLabel);

        bind(isDoneLabel);
        cJmp1(isTrue, JumpCondType::EQUALS);
        movInt(dstReg, falseValue);
        cJmp(trulyDone);

        bind(isTrue);
        movInt(dstReg, trueValue);

        bind(trulyDone);

        ctx.restore();
    }

    void doBin(BinaryType type, size_t dst, size_t lhs, size_t rhs) override {
        if (type == BinaryType::SUB) {
            fastArith(dst, lhs, rhs, ArithmeticOp::SUB);
            return;
        }
        if (type == BinaryType::MUL) {
            fastArith(dst, lhs, rhs, ArithmeticOp::MUL);
            return;
        }
        if (type == BinaryType::DIV) {
            fastArith(dst, lhs, rhs, ArithmeticOp::DIV);
            return;
        }
        if (type == BinaryType::GT) {
            fastCmp(dst, lhs, rhs, JumpCondType::GREATER);
            return;
        }
        if (type == BinaryType::LESS) {
            fastCmp(dst, lhs, rhs, JumpCondType::LESS);
            return;
        }
        if (type == BinaryType::GEQ) {
            fastCmp(dst, lhs, rhs, JumpCondType::GREATER_OR_EQUAL);
            return;
        }
        if (type == BinaryType::LEQ) {
            fastCmp(dst, lhs, rhs, JumpCondType::LESS_OR_EQUAL);
            return;
        }
        /*if (type == BinaryType::EQ) {
            fastEq(dst, lhs, rhs, std::bit_cast<size_t>(LoxValue::True()), std::bit_cast<size_t>(LoxValue::False()));
            return;
        }
        if (type == BinaryType::NEQ) {
            fastEq(dst, lhs, rhs, std::bit_cast<size_t>(LoxValue::False()), std::bit_cast<size_t>(LoxValue::True()));
            return;
        }*/
        auto crashLabel = makeLabel1();
        auto doneLabel = makeLabel1();
/*        if (type == BinaryType::ADD) {
            fastAdd(dst, lhs, rhs, crashLabel, doneLabel);
        }*/

        bind(crashLabel);
        array<Arg, 3> argz{Arg::Imm((size_t)type), handleToArg(lhs), handleToArg(rhs)};
        chadCall(Arg::ImmPtr((void*)&builtin::doSimpleBin), argz, handleToArg(dst));

        bind(doneLabel);
    }

    void fastToBool(size_t tgt, size_t src, size_t tmp, size_t trueValue, size_t falseValue) {
        this->getLoxTag(tgt, src);

        auto doneLabel = makeLabel1();
        auto falseLabel = makeLabel1();
        auto trueLabel = makeLabel1();

        // trap();
        movInt(tmp, LoxValue::ValueType2::NIL);
        cJmp(falseLabel, JumpCondType::EQUALS, tgt, tmp);

        movInt(tmp, LoxValue::ValueType2::BOOL_FALSE);
        cJmp(trueLabel, JumpCondType::NOT_EQUALS, tgt, tmp);

        bind(falseLabel);
        movInt(tgt, falseValue);
        cJmp(doneLabel);

        bind(trueLabel);
        movInt(tgt, trueValue);

        bind(doneLabel);
    }

    void toBool(size_t tgt, size_t src) override {
        auto tmp = allocateRegister(8);
        fastToBool(tgt, src, tmp, 1, 0);
        freeRegister(tmp);
        // array<Arg, 1> argz{handleToArg(src)};
        // chadCall(Arg::ImmPtr((void*)&builtin::toBool), argz, handleToArg(tgt));
    }

    void allocateClosed(size_t ref, size_t frameId, size_t localId, size_t value, bool isConst) override {
        if (isConst) {
            auto tmp = allocateRegister(sizeof(LoxValue*));

            movReg(tmp, ref);

            vector<int> derefs;
            for (size_t i = 0; i < frameId; i++) {
                derefs.push_back(offsetof(FunctionRef, parent));
            }
            derefChain(tmp, derefs);
            writeMem(tmp, value, offsetof(FunctionRef, captures)+(sizeof(LoxValue*) * localId), sizeof(LoxValue));

            freeRegister(tmp);
            return;
        }

        array<Arg, 4> argz{handleToArg(ref), Arg::Imm(frameId), Arg::Imm(localId), handleToArg(value)};
        chadCall(Arg::ImmPtr((void*)builtin::allocateClosed), argz, nullopt);
        return;
        vector<int> derefs;
        for (size_t i = 0; i < frameId; i++) {
            derefs.push_back(offsetof(FunctionRef, parent));
        }
        derefs.push_back(offsetof(FunctionRef, captures)+(sizeof(LoxValue*) * localId));

        auto tmp = allocateRegister(sizeof(LoxValue*));
        auto tmp2 = allocateRegister(sizeof(LoxValue*));

        movReg(tmp, ref);
        derefChain(tmp, derefs);

        chadCall(Arg::ImmPtr((void*)&builtin::allocateLoxValue), {}, handleToArg(tmp2));
        writeMem(tmp2, value, 0, sizeof(LoxValue));

        writeMem(tmp, tmp2, 0, sizeof(LoxValue*));

        freeRegister(tmp);
        freeRegister(tmp2);
    }

    void cmpImm(size_t tgt, i32 imm) {
        if (allocator.isStack(tgt)) {
            mc.cmpImm(X64Register::Rsp, allocator.getStackOffset(tgt), imm);
        } else {
            mc.cmpImm(allocator.getReg(tgt), imm);
        }
    }

    void fasterCall(size_t tgt, size_t subj, span<size_t> argz) {
        bindHint("LOX - fasterCall");
        getPtr(tgt, subj); // FunctionRef* in tgt

        auto crashLabel = makeLabel1();
        auto doneLabel = makeLabel1();

        assert(not allocator.isStack(tgt));
        mc.cmpImm(allocator.getReg(tgt), offsetof(FunctionRef, argCount), argz.size());
        cJmp1(crashLabel, JumpCondType::NOT_EQUALS);

        vector<size_t> argz2;
        argz2.push_back(tgt);
        for (auto arg : argz) {
            argz2.push_back(arg);
        }

        assert(not allocator.isStack(tgt));
        callC(Arg::MemoryValue(allocator.getReg(tgt), offsetof(FunctionRef, fPtr), 8), argz2, tgt);

        cJmp(doneLabel);

        bind(crashLabel);
        mc.hlt();
        bind(doneLabel);
        bindHint("LOX - fasterCall END");
    }

    void dynCall(size_t tgt1, size_t subj1, span<size_t> argz) {
        auto ctx = this->getAllocCtx();
        auto tgt = ctx.ensureRegWriteback(tgt1);
        auto subj = ctx.ensureReg(subj1);
        auto tmp = ctx.allocReg();
        auto tmp1 = ctx.allocReg();

        auto handleNotFunctionLabel = makeLabel1();
        auto doneLabel = makeLabel1();
        auto crashLabel = makeLabel1();
        auto okLabel = makeLabel1();

        auto t = ctx.originalTransform(argz);

        getLoxTag(tmp, subj);

        movInt(tgt, LoxValue::ValueType2::FUNCTION_REF);
        cJmp(handleNotFunctionLabel, JumpCondType::NOT_EQUALS, tmp, tgt);

        fasterCall(tgt, subj, t);
        cJmp(doneLabel);

        bind(handleNotFunctionLabel);

        movInt(tgt, LoxValue::ValueType2::CLASS);
        // trap();
        cJmp(crashLabel, JumpCondType::NOT_EQUALS, tmp, tgt);
        cJmp(okLabel);

        bind(crashLabel);
        mc.hlt();
        bind(okLabel);

        std::array<Arg,1>instArgz{handleToArg(subj)};
        chadCall(Arg::ImmPtr((void*)builtin::instantiate), instArgz, handleToArg(tgt));

        getPtr(tmp, tgt);

        readMem(tmp, tmp, offsetof(ObjectRef, construcor), sizeof(FunctionRef*));
        // TODO FIXME if class doenst have constuctor, and user passes arguments we wont crash

        // std::array<Arg,1>checkArgz{handleToArg(tgt)};
        // chadCall(Arg::ImmPtr((void*)builtin::getConstructor), checkArgz, handleToArg(tmp));

        auto zeroImm = movImmValueToReg(0);
        cJmp(doneLabel, JumpCondType::EQUALS, tmp, zeroImm);
        freeRegister(zeroImm);

        readMem(tmp1, tmp, offsetof(FunctionRef, captures), sizeof(LoxValue));
        writeMem(tmp, tgt, offsetof(FunctionRef, captures), sizeof(LoxValue));

        fasterCall(tgt, tmp, t);

        writeMem(tmp, tmp1, offsetof(FunctionRef, captures), sizeof(LoxValue));

        cJmp(doneLabel);

        bind(doneLabel);
        ctx.restore();
    }

/*    void dinnerCall(size_t tgt1, size_t subj1, span<size_t> argz) {
        auto ctx = this->getAllocCtx();
        auto tgt = ctx.ensureRegWriteback(tgt1);
        auto subj = ctx.ensureReg(subj1);
        auto tmp = ctx.allocReg();
        auto tmp1 = ctx.allocReg();

        auto handleNotFunctionLabel = makeLabel1();
        auto doneLabel = makeLabel1();
        auto crashLabel = makeLabel1();
        auto okLabel = makeLabel1();

        auto t = ctx.originalTransform(argz);

        getLoxTag(tmp, subj);

        movInt(tgt, LoxValue::ValueType2::FUNCTION_REF);
        cJmp(handleNotFunctionLabel, JumpCondType::NOT_EQUALS, tmp, tgt);

        fasterCall(tgt, subj, t);
        cJmp(doneLabel);

        bind(handleNotFunctionLabel);

        movInt(tgt, LoxValue::ValueType2::CLASS);
        // trap();
        cJmp(crashLabel, JumpCondType::NOT_EQUALS, tmp, tgt);
        cJmp(okLabel);

        bind(crashLabel);
        mc.hlt();
        bind(okLabel);

        std::array<Arg,1>instArgz{handleToArg(subj)};
        chadCall(Arg::ImmPtr((void*)builtin::instantiate), instArgz, handleToArg(tgt));

        getPtr(tmp, tgt);

        readMem(tmp, tmp, offsetof(ObjectRef, construcor), sizeof(FunctionRef*));
        // TODO FIXME if class doenst have constuctor, and user passes arguments we wont crash

        // std::array<Arg,1>checkArgz{handleToArg(tgt)};
        // chadCall(Arg::ImmPtr((void*)builtin::getConstructor), checkArgz, handleToArg(tmp));

        auto zeroImm = movImmValueToReg(0);
        cJmp(doneLabel, JumpCondType::EQUALS, tmp, zeroImm);
        freeRegister(zeroImm);

        readMem(tmp1, tmp, offsetof(FunctionRef, captures), sizeof(LoxValue));
        writeMem(tmp, tgt, offsetof(FunctionRef, captures), sizeof(LoxValue));

        fasterCall(tgt, tmp, t);

        writeMem(tmp, tmp1, offsetof(FunctionRef, captures), sizeof(LoxValue));

        cJmp(doneLabel);

        bind(doneLabel);
        ctx.restore();
    }*/


    void callMethod(size_t tgt1, size_t subj1, u32 fieldId, span<size_t> argz, std::optional<size_t> methodFrame) {
        auto ctx = this->getAllocCtx();
        auto tgt = ctx.ensureRegWriteback(tgt1);
        auto subj = ctx.ensureReg(subj1);
        auto tmp = ctx.allocReg();
        auto tmp1 = methodFrame.has_value() ? ctx.allocReg() : 0;

        auto handleNotFunctionLabel = makeLabel1();
        auto doneLabel = makeLabel1();

        auto t = ctx.originalTransform(argz);

        getLoxTag(tmp, subj);

        movInt(tgt, LoxValue::ValueType2::INSTANCE);
        cJmp(handleNotFunctionLabel, JumpCondType::NOT_EQUALS, tmp, tgt);

        // preserve `this`
        if (methodFrame.has_value()) {
            readMem(tmp1, *methodFrame, offsetof(FunctionRef, captures), sizeof(LoxValue));
        }

        std::array<Arg,3>instArgz{handleToArg(subj), Arg::Imm(fieldId), Arg::Imm(argz.size())};
        chadCall(Arg::ImmPtr((void*)builtin::getMethod), instArgz, handleToArg(tmp));

        fasterCall(tgt, tmp, t);

        // restore `this`
        if (methodFrame.has_value()) {
            writeMem(*methodFrame, tmp1, offsetof(FunctionRef, captures), sizeof(LoxValue));
        }

        cJmp(doneLabel);

        bind(handleNotFunctionLabel);
        mc.hlt();

        bind(doneLabel);
        ctx.restore();
    }

    void dynamicCall(size_t tgt, size_t subj, span<size_t> argz) override {
        dynCall(tgt, subj, argz);
        // VALIDATE CALL
        /*array<Arg, 2> argz1{handleToArg(subj), Arg::Imm(argz.size())};
        chadCall(Arg::ImmPtr((void*)&builtin::getCallPtr), argz1, handleToArgAssume8(tgt));

        auto tmp = allocateRegister(8);
        movReg(tmp, tgt);

        derefChainI(tmp, {offsetof(FunctionRef, func), offsetof(Function, runtimeData), 0});

        vector<size_t> argz2;
        argz2.push_back(tgt);
        for (auto arg : argz) {
            argz2.push_back(arg);
        }

        callC(handleToArgAssume8(tmp), argz2, tgt);

        freeRegister(tmp);*/
    }

    void allocateClosure(size_t tgt, Function *f, size_t parent) override {
        array<Arg, 2> argz{Arg::ImmPtr(f), handleToArg(parent)};
        chadCall(Arg::ImmPtr((void*)builtin::allocateClosure), argz, handleToArg(tgt));
    }

    void allocateClass(size_t tgt, Class* clazz, size_t frame, std::optional<size_t> super) override {
        array<Arg, 3> argz{Arg::ImmPtr(clazz), Arg::Imm(LoxValue::Nil().internal), handleToArg(frame)};
        if (super.has_value()) argz[1] = handleToArg(*super);
        chadCall(Arg::ImmPtr((void*)builtin::allocateClass), argz, handleToArg(tgt));
    }

    void readClosed(size_t dst, size_t ref, size_t frameId, size_t localId, bool isConst) override {
        movReg(dst, ref);

        vector<int> derefs;
        for (size_t i = 0; i < frameId; i++) {
            derefs.push_back(offsetof(FunctionRef, parent));
        }
        derefs.push_back(offsetof(FunctionRef, captures)+(sizeof(LoxValue*) * localId));
        if (not isConst) derefs.push_back(0);
        derefs.push_back(0);

        derefChain(dst, derefs);
        // trap();
    }

    void writeClosed(size_t ref, size_t frameId, size_t localId, size_t value, bool isConst) override {
        // array<Arg, 4> argz{handleToArg(ref), Arg::Imm(frameId), Arg::Imm(localId), handleToArg(value)};
        // chadCall(Arg::ImmPtr((void*)builtin::writeClosed), argz, nullopt);
        // return;
        vector<int> derefs;
        for (size_t i = 0; i < frameId; i++) {
            derefs.push_back(offsetof(FunctionRef, parent));
        }
        if (not isConst) derefs.push_back(offsetof(FunctionRef, captures)+(sizeof(LoxValue*) * localId));
        derefs.push_back(0);

        auto tmp = this->allocateRegister(sizeof(LoxValue*));

        movReg(tmp, ref);
        derefChain(tmp, derefs);
        if (not isConst) {
            writeMem(tmp, value, 0, sizeof(LoxValue));
        } else {
            writeMem(tmp, value, offsetof(FunctionRef, captures)+(sizeof(LoxValue*) * localId), sizeof(LoxValue));
        }

        freeRegister(tmp);
    }

    void negate(size_t dst, size_t src) override {
        auto tmp = allocateRegister(8);
        fastToBool(dst, src, tmp, std::bit_cast<size_t>(LoxValue::False()), std::bit_cast<size_t>(LoxValue::True()));
        freeRegister(tmp);
        // array<Arg, 1> argz{handleToArg(src)};
        // chadCall(Arg::ImmPtr((void*)builtin::loxNegate), argz, handleToArg(dst));
    }

    // FIXME raw code
    void copyClosed(size_t dst, size_t src, size_t dstId, size_t srcId) override {
        auto tmp = allocateRegister(8);
        auto tmp1 = allocateRegister(8);

        readMem(tmp1, src, offsetof(FunctionRef, captures)+(sizeof(LoxValue*)*srcId), sizeof(LoxValue*));
        getPtr(tmp, dst);
        writeMem(tmp, tmp1, offsetof(FunctionRef, captures)+(sizeof(LoxValue*)*dstId), sizeof(LoxValue*));

        freeRegister(tmp);
        freeRegister(tmp1);

        // array<Arg, 4> argz{handleToArg(dst), handleToArg(src), Arg::Imm(dstId), Arg::Imm(srcId)};
        // chadCall(Arg::ImmPtr((void*)builtin::copyClosed), argz, nullopt);
    }
};

struct MilaReg;
struct MilaIrGen;
struct MilaCodeGen;

struct MilaDataType {
    size_t size = 8;
};

struct MilaGenCtx {
    using REG = MilaReg;
    using IRGEN = MilaIrGen;
    using GEN = MilaCodeGen;
    using ASSEMBLER = MilaAssembler;
};

struct MilaCodeGen: CodeGen<MilaGenCtx> {
    using CodeGen::CodeGen;
};

struct LoxBool: public NamedIrInstruction<"lox_bool", MilaGenCtx> {
    PUB_VIRTUAL_COPY(LoxBool)
    bool v;

    LoxBool(SSARegisterHandle target, bool v) : NamedIrInstruction(target), v(v) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {}

    void print(MilaIrGen&, std::ostream& steam) override {
        basePrint(steam, "{}", v);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.movInt(gen.getReg(target), std::bit_cast<uint64_t>(LoxValue::Bool(v)));
    }
};

struct LoxCallMethod: public NamedIrInstruction<"lox_call", MilaGenCtx> {
    PUB_VIRTUAL_COPY(LoxCallMethod)
    SSARegisterHandle subj;
    u32 methodId;
    std::string methodName;
    std::vector<SSARegisterHandle> argz;
    std::optional<SSARegisterHandle> methodFrame;

    LoxCallMethod(SSARegisterHandle target, SSARegisterHandle subj, u32 methodId, std::string_view methodName, std::vector<SSARegisterHandle> argz, std::optional<SSARegisterHandle> methodFrame) : NamedIrInstruction(target), subj(subj), methodId(methodId), methodName(std::string(methodName)), argz(argz), methodFrame(methodFrame) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        for (auto& a : argz) fn(a);
        fn(subj);
        if (methodFrame.has_value()) fn(*methodFrame);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{}#{}@{} {} is? {}", subj, methodName, methodId, argz, methodFrame.has_value() ? "in-method" : "in-function");
    }

    void generate(MilaCodeGen& gen) override {
        auto args = gen.getRegs(argz);
        gen.assembler.callMethod(gen.getReg(target), gen.getReg(subj), methodId, args, gen.getReg(methodFrame));
    }
};

void FunctionRef::setThis(ObjectRef *self) {
    writeConst(0, LoxValue::Object(self));
}

void FunctionRef::writeConst(size_t id, LoxValue value) {
    captures[id] = std::bit_cast<LoxValue*>(value);
}

LoxValue FunctionRef::readConst(size_t id) {
    return std::bit_cast<LoxValue>(captures[id]);
}

size_t FunctionRef::calculateSize() {
    return sizeof(FunctionRef)+func->upValCount()*sizeof(LoxValue);
}


struct LoxNil: public NamedIrInstruction<"lox_nil", MilaGenCtx> {
    PUB_VIRTUAL_COPY(LoxNil)

    LoxNil(SSARegisterHandle target) : NamedIrInstruction(target) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {}

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "");
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.movInt(gen.getReg(target), std::bit_cast<uint64_t>(LoxValue::Nil()));
    }
};

struct LoxNumber: public NamedIrInstruction<"lox_number", MilaGenCtx> {
    PUB_VIRTUAL_COPY(LoxNumber)
    double v;

    LoxNumber(SSARegisterHandle target, double v) : NamedIrInstruction(target), v(v) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {}

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{}", v);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.movInt(gen.getReg(target), std::bit_cast<uint64_t>(LoxValue::Number(v)));
    }
};

std::unordered_set<void*> STATIC_ROOTS;

struct LoxString: public NamedIrInstruction<"lox_string", MilaGenCtx> {
    PUB_VIRTUAL_COPY(LoxString)
    string v;

    LoxString(SSARegisterHandle target, string v) : NamedIrInstruction(target), v(v) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {}

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "\"{}\"", escape(v));
    }

    void generate(MilaCodeGen& gen) override {
        auto alloc = allocateLoxString(string_view(v.data()+1, v.size()-2));
        STATIC_ROOTS.insert(alloc);
        gen.assembler.movInt(gen.getReg(target), std::bit_cast<uint64_t>(LoxValue::String(alloc)));
    }
};

struct LoxNeg: public NamedIrInstruction<"lox_neg", MilaGenCtx> {
    PUB_VIRTUAL_COPY(LoxNeg)
    SSARegisterHandle v;

    LoxNeg(SSARegisterHandle target, SSARegisterHandle v) : NamedIrInstruction(target), v(v) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(v);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{}", v);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.negate(gen.getReg(target), gen.getReg(v));
    }
};


struct LoxBin: public NamedIrInstruction<"bin", MilaGenCtx> {
    PUB_VIRTUAL_COPY(LoxBin)
    BinaryType type;
    SSARegisterHandle lhs;
    SSARegisterHandle rhs;

    LoxBin(SSARegisterHandle target, BinaryType type, SSARegisterHandle lhs, SSARegisterHandle rhs) : NamedIrInstruction(target), type(type), lhs(lhs), rhs(rhs) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(rhs);
        fn(lhs);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{} {} {}", lhs, binarToString(type), rhs);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.doBin(type, gen.getReg(target), gen.getReg(lhs), gen.getReg(rhs));
    }
};

struct LoxReadField: public NamedIrInstruction<"read_field", MilaGenCtx> {
    PUB_VIRTUAL_COPY(LoxReadField)
    SSARegisterHandle subj;
    string fieldName;
    u32 fieldId;

    LoxReadField(SSARegisterHandle target, SSARegisterHandle subj, string fieldName, u32 fieldId) : NamedIrInstruction(target), subj(subj), fieldName(fieldName), fieldId(fieldId) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(subj);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{}#{}@{}", subj, fieldName, fieldId);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.readField(gen.getReg(target), gen.getReg(subj), fieldId);
    }
};

struct LoxWriteField: public NamedIrInstruction<"write_field", MilaGenCtx> {
    PUB_VIRTUAL_COPY(LoxWriteField)
    SSARegisterHandle subj;
    string fieldName;
    SSARegisterHandle v;
    u32 fieldId;

    LoxWriteField(SSARegisterHandle subj, string fieldName, SSARegisterHandle v, u32 fieldId) : NamedIrInstruction(SSARegisterHandle::invalid()), subj(subj), fieldName(fieldName), v(v), fieldId(fieldId) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(subj);
        fn(v);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{}#{}@{} <- {}", subj, fieldName, fieldId, v);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.writeField(gen.getReg(subj), fieldId, gen.getReg(v));
    }
};


struct DynamicCall: public NamedIrInstruction<"dynamic_call", MilaGenCtx> {
    PUB_VIRTUAL_COPY(DynamicCall)
    SSARegisterHandle self;
    vector<SSARegisterHandle> argz;

    DynamicCall(SSARegisterHandle target, SSARegisterHandle self, vector<SSARegisterHandle> argz) : NamedIrInstruction(target), self(self), argz(argz) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(self);
        for (auto arg : argz) fn(arg);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{} {}", self, argz);
    }

    void generate(MilaCodeGen& gen) override {
        auto args = gen.getRegs(argz);
        gen.assembler.dynamicCall(gen.getReg(target), gen.getReg(self), args);
    }
};

struct BuiltinPrint: public NamedIrInstruction<"print", MilaGenCtx> {
    PUB_VIRTUAL_COPY(BuiltinPrint)
    SSARegisterHandle arg;

    BuiltinPrint(SSARegisterHandle arg) : NamedIrInstruction(SSARegisterHandle::invalid()), arg(arg) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(arg);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{}", arg);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.print1(gen.getReg(arg));
    }
};

struct LoxBooling: public NamedIrInstruction<"to_bool", MilaGenCtx> {
    PUB_VIRTUAL_COPY(LoxBooling)
    SSARegisterHandle v;

    LoxBooling(SSARegisterHandle target, SSARegisterHandle v) : NamedIrInstruction(target), v(v) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(v);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{}", v);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.toBool(gen.getReg(target), gen.getReg(v));
    }
};

struct LoxAllocateClass: public NamedIrInstruction<"allocate_class", MilaGenCtx> {
    PUB_VIRTUAL_COPY(LoxAllocateClass)
    Class* clazz;
    std::optional<SSARegisterHandle> super;
    SSARegisterHandle frame;

    LoxAllocateClass(SSARegisterHandle target, Class* clazz, std::optional<SSARegisterHandle> super, SSARegisterHandle frame) : NamedIrInstruction(target), clazz(clazz), super(super), frame(frame) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(frame);
        if (super.has_value()) fn(*super);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{} - {}", clazz->data.name, super.has_value() ? super->toString() : "");
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.allocateClass(gen.getReg(target), clazz, gen.getReg(frame), gen.getReg(super));
    }
};

struct LoxReadGlobal: public NamedIrInstruction<"read_global", MilaGenCtx> {
    PUB_VIRTUAL_COPY(LoxReadGlobal)
    size_t id;

    LoxReadGlobal(SSARegisterHandle target, size_t id) : NamedIrInstruction(target), id(id) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {}

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{}", id);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.readGlobal(gen.getReg(target), id);
    }
};

struct LoxWriteGlobal: public NamedIrInstruction<"write_global", MilaGenCtx> {
    PUB_VIRTUAL_COPY(LoxWriteGlobal)
    size_t id;
    SSARegisterHandle v;

    LoxWriteGlobal(size_t id, SSARegisterHandle v) : NamedIrInstruction(SSARegisterHandle::invalid()), id(id), v(v) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(v);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{} <- {}", id, v);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.writeGlobal(id, gen.getReg(v));
    }
};

struct LoxReadCaptured: public NamedIrInstruction<"read_captured", MilaGenCtx> {
    PUB_VIRTUAL_COPY(LoxReadCaptured)
    SSARegisterHandle closure;
    size_t fId = -1;
    size_t locId = -1;
    bool isConst;

    LoxReadCaptured(SSARegisterHandle target, SSARegisterHandle closure, size_t fId, size_t locId, bool isConst) : NamedIrInstruction(target), closure(closure), fId(fId), locId(locId), isConst(isConst) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(closure);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{} {}#{}/{}", closure, fId, locId, isConst);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.readClosed(gen.getReg(target), gen.getReg(closure), fId, locId, isConst);
    }
};

struct LoxWriteCaptured: public NamedIrInstruction<"write_captured", MilaGenCtx> {
    PUB_VIRTUAL_COPY(LoxWriteCaptured)
    SSARegisterHandle closure;
    size_t fId = -1;
    size_t locId = -1;
    SSARegisterHandle v;
    bool isConst;

    LoxWriteCaptured(SSARegisterHandle closure, size_t fId, size_t locId, SSARegisterHandle v, bool isConst) : NamedIrInstruction(SSARegisterHandle::invalid()), closure(closure), fId(fId), locId(locId), v(v), isConst(isConst) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(closure);
        fn(v);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{} % {}#{}/{} <- {}", closure, fId, locId, isConst, v);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.writeClosed(gen.getReg(closure), fId, locId, gen.getReg(v), isConst);
    }
};

struct LoxAllocCaptured: public NamedIrInstruction<"alloc_captured", MilaGenCtx> {
    PUB_VIRTUAL_COPY(LoxAllocCaptured)
    SSARegisterHandle closure;
    size_t fId = -1;
    size_t locId = -1;
    SSARegisterHandle v;
    bool isConst;

    LoxAllocCaptured(SSARegisterHandle closure, size_t fId, size_t locId, SSARegisterHandle v, bool isConst) : NamedIrInstruction(SSARegisterHandle::invalid()), closure(closure), fId(fId), locId(locId), v(v), isConst(isConst) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(v);
        fn(closure);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{} % {}#{}/{} <- {}", closure, fId, locId, isConst, v);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.allocateClosed(gen.getReg(closure), fId, locId, gen.getReg(v), isConst);
    }
};

struct LoxReadLocal: public NamedIrInstruction<"read_local", MilaGenCtx> {
    PUB_VIRTUAL_COPY(LoxReadLocal)
    size_t localId;
    string name;
    SSARegisterHandle frame;

    LoxReadLocal(SSARegisterHandle target, size_t localId, string name, SSARegisterHandle frame) : NamedIrInstruction(target), localId(localId), name(name), frame(frame) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(frame);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{} % {}@{}", frame, name, localId);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.movReg(gen.getReg(target), gen.getReg(frame), 0, localId*sizeof(LoxValue), sizeof(LoxValue));
    }
};

struct LoxWriteLocal: public NamedIrInstruction<"write_local", MilaGenCtx> {
    PUB_VIRTUAL_COPY(LoxWriteLocal)
    size_t localId;
    string name;
    SSARegisterHandle v;
    SSARegisterHandle frame;

    LoxWriteLocal(size_t localId, string name, SSARegisterHandle v, SSARegisterHandle frame) : NamedIrInstruction(SSARegisterHandle::invalid()), localId(localId), name(name), v(v), frame(frame) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(v);
        fn(frame);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{} - {}@{} <- {}", frame, name, localId, v);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.movReg(gen.getReg(frame), gen.getReg(v), localId*sizeof(LoxValue), 0, sizeof(LoxValue));
    }
};

struct LoxAllocateClosure: public NamedIrInstruction<"allocate_closure", MilaGenCtx> {
    PUB_VIRTUAL_COPY(LoxAllocateClosure)
    Function* func;
    SSARegisterHandle parent;

    LoxAllocateClosure(SSARegisterHandle target, Function* func, SSARegisterHandle parent) : NamedIrInstruction(target), func(func), parent(parent) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(parent);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{} {}", func->data.name, parent);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.allocateClosure(gen.getReg(target), func, gen.getReg(parent));
    }
};

struct LoxCopyCapture: public NamedIrInstruction<"copy_capture", MilaGenCtx> {
    PUB_VIRTUAL_COPY(LoxCopyCapture)
    SSARegisterHandle tgt;
    size_t tgtId;
    SSARegisterHandle src;
    size_t srcId;

    LoxCopyCapture(SSARegisterHandle tgt, size_t tgtId, SSARegisterHandle src, size_t srcId) : NamedIrInstruction(SSARegisterHandle::invalid()), tgt(tgt), tgtId(tgtId), src(src), srcId(srcId) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(src);
        fn(tgt);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{}#{} <- {}#{}", tgt, tgtId, src, srcId);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.copyClosed(gen.getReg(tgt), gen.getReg(src), tgtId, srcId);
        return;
        // gen.assembler.movReg(gen.getReg(tgt), gen.getReg(src), offsetof(FunctionRef, captures)+(sizeof(LoxValue*)*tgtId), offsetof(FunctionRef, captures)+(sizeof(LoxValue*)*srcId), sizeof(LoxValue*));

        // gen.assembler.trap();
        auto tmp = gen.allocateTemp(sizeof(LoxValue*));
        auto tmp1 = gen.allocateTemp(sizeof(LoxValue*));
        gen.assembler.readMem(tmp, gen.getReg(src), offsetof(FunctionRef, captures)+(sizeof(LoxValue*)*srcId), sizeof(LoxValue*));
        gen.assembler.getPtr(tmp1, gen.getReg(tgt));
        gen.assembler.writeMem(tmp1, tmp, offsetof(FunctionRef, captures)+(sizeof(LoxValue*)*tgtId), sizeof(LoxValue*));
        gen.freeTemp(tmp);
        gen.freeTemp(tmp1);
        // gen.assembler.trap();
    }
};

struct LoxGetSuper: public NamedIrInstruction<"get_super", MilaGenCtx> {
    PUB_VIRTUAL_COPY(LoxGetSuper)
    SSARegisterHandle self;

    LoxGetSuper(SSARegisterHandle target, SSARegisterHandle self) : NamedIrInstruction(target), self(self) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(self);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{}", self);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.getPtr(gen.getReg(target), gen.getReg(self));
        gen.assembler.readMem(gen.getReg(target), gen.getReg(target), offsetof(ObjectRef, proto1), sizeof(LoxValue));
    }
};

struct MilaReg: SSARegister {
    MilaReg(size_t blockId, string name, MilaDataType type1, Type type): SSARegister(blockId, name, type), dataType(type1) {

    }

    MilaDataType dataType;

    size_t sizeBytes() const {
        return dataType.size; // dataType.getHandle().sizeBytes();
    }

    MilaReg copy() const {
        return *this;
    }
};

struct MilaIrGen: IRGen<MilaGenCtx> {
    using IRGen::IRGen;
};

using MilaBB = CodeBlock<MilaGenCtx>*;
using MilaGenRet = Result<pair<SSARegisterHandle, MilaBB>>;

struct MilaIrGenCtx: IRGenCtx<MilaIrGenCtx, MilaGenCtx> {
    string functionName;

    using IRGenCtx::IRGenCtx;

    SSARegisterHandle makeTmp(MilaDataType type) {
        return pushRegister("_tmp", type, {}, SSARegister::Type::TMP);
    }

    template<template<typename> typename T, typename... Args>
    SSARegisterHandle push(MilaDataType type, Args&&... argz) {
        auto tmp = makeTmp(type);
        pushInstruction<T<MilaGenCtx>>(tmp, std::forward<Args>(argz)...);

        return tmp;
    }

    template<typename T, typename... Args>
    SSARegisterHandle push(MilaDataType type, Args&&... argz) {
        auto tmp = makeTmp(type);
        pushInstruction<T>(tmp, std::forward<Args>(argz)...);

        return tmp;
    }

    SSARegisterHandle pushRegister(string name, MilaDataType type, optional<SSARegisterHandle> prev, SSARegister::Type type1) {
        return current().pushRegister(make_unique<MilaReg>(0, std::move(name), type, type1));
    }

    SSARegisterHandle generateNewVersion(SSARegisterHandle previousHandle) {
        assert(previousHandle.isValid());

        auto& previous = gen.getRecord(previousHandle);

        return pushRegister(previous.name, previous.dataType, previousHandle, SSARegister::Type::VAR);
    }
};

struct Compiler: ASTVisitor {
    vector<unique_ptr<MilaIrGen>> irGens;
    vector<Function*> funks;
    vector<unique_ptr<ControlFlowGraph<MilaGenCtx>>> graphs;

    std::vector<MilaIrGenCtx> stuff;
    std::optional<SSARegisterHandle> curRet;
    vector<std::pair<Function*, std::pair<bool, bool>>> functionStack;

    static constexpr std::string CURRENT_CLOSSURE = "__self";
    static constexpr std::string CURRENT_LOCALS = "__frame";

    MilaIrGenCtx getCtx() {
        assert(not stuff.empty());
        return stuff.back();
    }

    SSARegisterHandle genExp(ASTExpr expr) {
        assert(not curRet.has_value());
        expr->visit(*this);
        assert(curRet.has_value());
        auto v = *curRet;
        curRet = {};

        return v;
    }

    void genStm(ASTStm stm) {
        assert(not curRet.has_value());
        stm->visit(*this);
        if (curRet.has_value()) curRet = {}; // TODO FIXME
        assert(not curRet.has_value());
    }

    SSARegisterHandle genRead(SpecTarget s, string name) {
        switch (s.type) {
            case HookedVariableType::LOCAL:
                return getCtx().push<LoxReadLocal>(MilaDataType{}, s.id, name, *getCtx().lookupLocal(CURRENT_LOCALS));
                break;
            case HookedVariableType::UPVAL:
                return getCtx().push<LoxReadCaptured>(MilaDataType{}, getCurrentClosure(), s.frameId, s.id, false);
                break;
            case HookedVariableType::GLOBAL:
                return getCtx().push<LoxReadGlobal>(MilaDataType{}, s.id);
                break;
            case HookedVariableType::CONST_UPVAL:
                return getCtx().push<LoxReadCaptured>(MilaDataType{}, getCurrentClosure(), s.frameId, s.id, true);
                break;
            case HookedVariableType::ALLOC_CONST_UPVAL:
            case HookedVariableType::ALLOC_UPVAL:
                PANIC();
        }
        PANIC();
    }

    void genWrite(SpecTarget s, SSARegisterHandle v, string name) {
        switch (s.type) {
            case HookedVariableType::LOCAL:
                getCtx().pushInstruction<LoxWriteLocal>(s.id, name, v, *getCtx().lookupLocal(CURRENT_LOCALS));
                break;
            case HookedVariableType::UPVAL:
                getCtx().pushInstruction<LoxWriteCaptured>(getCurrentClosure(), s.frameId, s.id, v, false);
                break;
            case HookedVariableType::GLOBAL:
                getCtx().pushInstruction<LoxWriteGlobal>(s.id, v);
                break;
            case HookedVariableType::ALLOC_UPVAL:
                getCtx().pushInstruction<LoxAllocCaptured>(getCurrentClosure(), s.frameId, s.id, v, false);
                break;
            case HookedVariableType::ALLOC_CONST_UPVAL:
                getCtx().pushInstruction<LoxAllocCaptured>(getCurrentClosure(), s.frameId, s.id, v, true);
                break;
            case HookedVariableType::CONST_UPVAL:
                getCtx().pushInstruction<LoxWriteCaptured>(getCurrentClosure(), s.frameId, s.id, v, true);
                break;
            default:
                PANIC();
        }
    }

    /*void doCompile(span<ASTNode1> ast, Function* funk) {
        auto& CFG = graphs.emplace_back(make_unique<ControlFlowGraph<MilaGenCtx>>());
        auto& IRGEN = irGens.emplace_back(make_unique<MilaIrGen>(*CFG.get()));
        funks.push_back(funk);
        functionStack.push_back(funk);

        auto& bb = IRGEN->createBlock("main");
        MilaIrGenCtx IR_GEN_CTX(*IRGEN, &bb, nullptr, {}, {});
        IR_GEN_CTX.functionName = funk->data.name;
        IR_GEN_CTX.pushRegister(CURRENT_CLOSSURE, MilaDataType{}, {}, SSARegister::Type::ARG);
        auto localsCount = funk->locals.size()-funk->upValCount();
        auto frameReg = IR_GEN_CTX.pushRegister(CURRENT_LOCALS, MilaDataType{localsCount*sizeof(LoxValue)}, {}, SSARegister::Type::VAR);

        pushCtx(IR_GEN_CTX);

        IR_GEN_CTX.pushInstruction<instructions::Alloca>(frameReg, localsCount*sizeof(LoxValue));

        for (auto& node : ast) {
            if (getCtx().current().isTerminated()) break;
            genStm(dynamic_cast<ASTStm>(node));
        }

        if (!getCtx().currentBlock->isTerminated()) {
            auto v = getCtx().push<LoxNil>(MilaDataType{});
            getCtx().pushInstruction<instructions::Return>(SSARegisterHandle::invalid(), v);
        }

        popCtx();
    }*/

    void begin(Function* gf) {
        compFunk(*gf);
    }

    SSARegisterHandle readThis() {
        return getCtx().push<LoxReadCaptured>(MilaDataType{}, getCurrentClosure(), 0, 0, true);
    }

    void invoke(Return& it) override {
        if (it.data.value.has_value()) {
            auto idk = genExp(*it.data.value);
            // FIXME
            getCtx().pushInstruction<instructions::Return>(idk);
        } else {
            auto v = isCurrentFunctionConstructor() ? readThis() : getCtx().push<LoxNil>(MilaDataType{});
            getCtx().pushInstruction<instructions::Return>(v);
        }
    }

    void invoke(Print& it) override {
        auto v = genExp(it.data.value);
        getCtx().pushInstruction<BuiltinPrint>(v);
    }

    void invoke(Call& it) override {
        vector<SSARegisterHandle> argz;
        for (auto arg : it.args) {
            argz.push_back(genExp(arg));
        }

        if (auto field = dynamic_cast<FieldAccess*>(it.fName); field != nullptr) {
            auto v = genExp(field->data.subject);
            curRet = getCtx().push<LoxCallMethod>(MilaDataType{}, v, field->fieldId, field->data.fieldName, argz, isCurrentFunctionMethod() ? std::optional<SSARegisterHandle>(getCurrentClosure()) : std::nullopt);
        } else {
            auto v = genExp(it.fName);
            curRet = getCtx().push<DynamicCall>(MilaDataType{}, v, argz);
        }
    }

    void invoke(NilLiteral& it) override {
        curRet = getCtx().push<LoxNil>(MilaDataType{});
    }

    void invoke(IntLiteral& it) override {
        curRet = getCtx().push<LoxNumber>(MilaDataType{}, it.data.value);
    }

    void invoke(Binary& it) override {
        if (it.data.type == BinaryType::AND) {
            array<pair<SSARegisterHandle, size_t>, 2> phis;
            auto nextBlock = getCtx().makeIf(
                [&](MilaIrGenCtx ctx) -> Result<pair<SSARegisterHandle, MilaBB>> {
                    pushCtx(ctx);

                    auto v = genExp(it.data.lhs);
                    phis[0] = {v, getCtx().current().id()};
                    auto actBuul = getCtx().push<LoxBooling>(MilaDataType{}, v);

                    auto ctx1 = popCtx();

                    return pair{actBuul, ctx1.currentBlock};
                },
                [&](MilaIrGenCtx ctx) -> Result<MilaBB> {
                    pushCtx(ctx);

                    auto v = genExp(it.data.rhs);
                    phis[1] = {v, getCtx().current().id()};

                    auto ctx1 = popCtx();

                    return ctx1.currentBlock;
                }
            );
            pushCtx(popCtx().withBlock(*nextBlock));

            curRet = getCtx().makePhi(phis).first;

            return;
        }

        if (it.data.type == BinaryType::OR) {
            array<pair<SSARegisterHandle, size_t>, 2> phis;
            auto nextBlock = getCtx().makeIf(
                    [&](MilaIrGenCtx ctx) -> Result<pair<SSARegisterHandle, MilaBB>> {
                        pushCtx(ctx);

                        auto v = genExp(it.data.lhs);
                        phis[0] = {v, getCtx().current().id()};
                        auto actBuul = getCtx().push<LoxBooling>(MilaDataType{}, v);
                        auto notActBuul = getCtx().push<instructions::BoolNot>(MilaDataType{}, actBuul);

                        auto ctx1 = popCtx();

                        return pair{notActBuul, ctx1.currentBlock};
                    },
                    [&](MilaIrGenCtx ctx) -> Result<MilaBB> {
                        pushCtx(ctx);

                        auto v = genExp(it.data.rhs);
                        phis[1] = {v, getCtx().current().id()};

                        auto ctx1 = popCtx();

                        return ctx1.currentBlock;
                    }
            );
            pushCtx(popCtx().withBlock(*nextBlock));

            curRet = getCtx().makePhi(phis).first;

            return;
        }

        auto lhs = genExp(it.data.lhs);
        auto rhs = genExp(it.data.rhs);

        curRet = getCtx().push<LoxBin>(MilaDataType{}, it.data.type, lhs, rhs);
    }

    void invoke(Super& it) override {
        auto self = genRead(it.hookedTarget, "_this");

        curRet = getCtx().push<LoxGetSuper>(MilaDataType{}, self);
    }

    void invoke(This& it) override {
        curRet = genRead(it.hookedTarget, "_this");
    }

    void invoke(FieldAccess &it) override {
        auto v = genExp(it.data.subject);
        curRet = getCtx().push<LoxReadField>(MilaDataType{}, v, it.data.fieldName, it.fieldId);
    }

    void invoke(Class& it) override {
        // FIXME constructors are special snoflakes :))))))))))))))))))
        for (auto& methdod : it.data.methods) {
            compFunk(*methdod, true, methdod->data.name == "init");
        }

        std::optional<SSARegisterHandle> super;
        if (it.data.superClass.has_value()) {
            super = genRead(it.hookedSuper, *it.data.superClass);
        }
        auto res = getCtx().push<LoxAllocateClass>(MilaDataType{}, &it, super, getCurrentClosure());
        genWrite(it.hookedDst, res, it.data.name);
    }

    void invoke(StringLiteral1& it) override {
        curRet = getCtx().push<LoxString>(MilaDataType{}, it.data.value);
    }

    void invoke(Negate& it) override {
        auto v = genExp(it.data.inner);
        curRet = getCtx().push<LoxNeg>(MilaDataType{}, v);
    }

    void invoke(Identifier& it) override {
        curRet = genRead(it.data.hookedTarget, it.data.value);
    }

    void hook(ASTExpr& tgt) {
        tgt->visit(*this);
    }

    void hook(ASTStm& tgt) {
        tgt->visit(*this);
    }

    void invoke(VariableDeclaration& it) override {
        auto rex = it.data.value.has_value() ? genExp(*it.data.value) : getCtx().push<LoxNil>(MilaDataType{});
        genWrite(it.data.hookedTarget, rex, it.data.dst);
    }

    void invoke(Block& it) override {
        for (auto& s : it.data.statements) {
            if (getCtx().current().isTerminated()) break;
            genStm(s);
        }
    }

    void pushCtx(MilaIrGenCtx ctx) {
        stuff.push_back(ctx);
    }

    MilaIrGenCtx popCtx() {
        auto v = stuff.back();

        stuff.pop_back();

        return v;
    }

    void invoke(IF& it) override {
        if (not it.data.elsBody.has_value()) {
            auto nextBlock = *getCtx().makeIf([&](MilaIrGenCtx ctx1) -> Result<pair<SSARegisterHandle, MilaBB>> {
                pushCtx(ctx1);

                auto fakeBool = genExp(it.data.cond);

                auto actualBool = getCtx().template push<LoxBooling>(MilaDataType{}, fakeBool);

                auto v = popCtx();

                return pair{actualBool, v.currentBlock};
            }, [&](MilaIrGenCtx ctx1) -> Result<MilaBB> {
                pushCtx(ctx1);

                genStm(it.data.ifBody);

                return popCtx().currentBlock;
            });
            pushCtx(popCtx().withBlock(nextBlock));
        } else {
            auto nextBlock = *getCtx().makeIf([&](MilaIrGenCtx ctx1) -> Result<pair<SSARegisterHandle, MilaBB>> {
                pushCtx(ctx1);

                auto fakeBool = genExp(it.data.cond);

                auto actualBool = getCtx().template push<LoxBooling>(MilaDataType{}, fakeBool);

                auto v = popCtx();

                return pair{actualBool, v.currentBlock};
            }, [&](MilaIrGenCtx ctx1) -> Result<MilaBB> {
                pushCtx(ctx1);

                genStm(it.data.ifBody);

                return popCtx().currentBlock;
            }, [&](MilaIrGenCtx ctx1) -> Result<MilaBB> {
                pushCtx(ctx1);

                genStm(*it.data.elsBody);

                return popCtx().currentBlock;
            });
            pushCtx(popCtx().withBlock(nextBlock));
        }
    }

    void invoke(While& it) override {
        auto nextBlock = *getCtx().makeWhile(
            "asdad"sv,
            [&](MilaIrGenCtx ctx1) -> Result<pair<SSARegisterHandle, MilaBB>> {
                pushCtx(ctx1);

                SSARegisterHandle v3;
                if (not it.data.cond.has_value()) {
                    v3 = getCtx().push<instructions::IntLiteral>(MilaDataType{}, 1);
                } else {
                    auto ss = genExp(*it.data.cond);
                    v3 = getCtx().push<LoxBooling>(MilaDataType{}, ss);
                }

                auto c1 = popCtx();

                return pair{v3, c1.currentBlock};
            },
            [&](MilaIrGenCtx ctx1) -> Result<MilaBB> {
                pushCtx(ctx1);

                genStm(it.data.body);

                auto c1 = popCtx();

                return c1.currentBlock;
            }
        );

        pushCtx(popCtx().withBlock(nextBlock));
    }

    void compFunk(Function& it, bool isMethod = false, bool isConstructor = false) {
        auto& CFG = graphs.emplace_back(make_unique<ControlFlowGraph<MilaGenCtx>>());
        auto& IRGEN = irGens.emplace_back(make_unique<MilaIrGen>(*CFG.get()));
        funks.push_back(&it);

        auto& bb = IRGEN->createBlock("main");
        MilaIrGenCtx IR_GEN_CTX(*IRGEN, &bb, nullptr, {}, {});
        IR_GEN_CTX.functionName = it.data.name;
        IR_GEN_CTX.pushRegister(CURRENT_CLOSSURE, MilaDataType{}, {}, SSARegister::Type::ARG);
        auto localsCount = it.locals.size()-it.upValCount();
        auto frameReg = IR_GEN_CTX.pushRegister(CURRENT_LOCALS, MilaDataType{localsCount*sizeof(LoxValue)}, {}, SSARegister::Type::VAR);

        pushCtx(IR_GEN_CTX);
        functionStack.push_back({&it, {isConstructor, isMethod}});

        IR_GEN_CTX.pushInstruction<instructions::Alloca>(frameReg, localsCount*sizeof(LoxValue));

        size_t UP_VAL_OFFSET = isMethod ? 1 : 0;

        size_t upValId = UP_VAL_OFFSET;
        size_t localId = 0;

        for (auto i = 0u; i < it.data.argz.size(); i++) {
            auto arg = it.data.argz[i];
            auto poop = IR_GEN_CTX.pushRegister(arg, MilaDataType{}, {}, SSARegister::Type::ARG);

            auto realI = UP_VAL_OFFSET+i;

            if (it.locals[realI]) {
                if (it.isModified[realI]) {
                    genWrite(SpecializedVariable::AllocateCaptured(upValId), poop, it.data.argz[i]);
                } else {
                    genWrite(SpecializedVariable::AllocateConstCaptured(upValId), poop, it.data.argz[i]);
                }
                upValId += 1;
            } else {
                genWrite(SpecializedVariable::Local(localId), poop, it.data.argz[i]);
                localId += 1;
            }
        }

        for (auto& node : it.data.body) {
            if (getCtx().current().isTerminated()) break;
            genStm(node);
        }

        if (!getCtx().currentBlock->isTerminated()) {
            auto v = isConstructor ? readThis() : getCtx().push<LoxNil>(MilaDataType{});
            getCtx().pushInstruction<instructions::Return>(SSARegisterHandle::invalid(), v);
        }

        functionStack.pop_back();
        popCtx();
    }

    void invoke(Function& it) override {
        compFunk(it);
        auto v = getCtx().push<LoxAllocateClosure>(MilaDataType{}, &it, getCurrentClosure());
        genWrite(it.hookedTarget, v, it.data.name);

        for (auto i = 0UL; i < it.captures.size(); i++) {
            if (DEBUG_JIT) println("WE {} CAPTURING {} - {} - {}", it.data.name, it.captures.size(), i, it.captures[i]);
            getCtx().pushInstruction<LoxCopyCapture>(v, it.upValCount()+i, getCurrentClosure(), currentFunction()->getUpValId(it.captures[i]));
        }
    }

    Function* currentFunction() {
        return functionStack.back().first;
    }

    bool isCurrentFunctionConstructor() {
        return functionStack.back().second.first;
    }

    bool isCurrentFunctionMethod() {
        return functionStack.back().second.second;
    }

    SSARegisterHandle getCurrentClosure() {
        return UNWRAP(getCtx().lookupLocal(CURRENT_CLOSSURE));
    }

    void invoke(Assign& it) override {
        auto v = genExp(it.data.value);

        if (auto v1 = dynamic_cast<Identifier*>(it.data.dst); v1) {
            genWrite(v1->data.hookedTarget, v, v1->data.value);
            curRet = v;
        } else if (auto v2 = dynamic_cast<FieldAccess*>(it.data.dst); v2) {
            auto subj = genExp(v2->data.subject);
            getCtx().pushInstruction<LoxWriteField>(subj, v2->data.fieldName, v, v2->fieldId);

            // do we need this????
            curRet = v;
        } else {
            std::cout << it.data.dst->className() << std::endl;
            TODO();
        }
    }

    void invoke(BoolLiteral1& it) override {
        curRet = getCtx().push<LoxBool>(MilaDataType{}, it.data.value);
    }


    void invoke(ASTNode& it) override {
        TODO();
    }
};

struct ASTExecutor: ASTVisitor {
    vector<LoxValue> valueStack;
    // vector<StackFrame*> frames;
    bool shouldReturn = false;
    LoxValue globals[4096];
    LoxValue callStack[4096];
    LoxValue* stackBase = callStack+4096;

    LoxValue pop() {
        assert(not valueStack.empty());

        auto v = valueStack.back(); valueStack.pop_back();

        return v;
    }

    void push(LoxValue val) {
        // println("PUSHING!!! {}", val.toString());
        valueStack.push_back(val);
    }

    void invoke(IntLiteral& it) override {
        push(LoxValue::Number(it.data.value));
    }

    FunctionRef* currentFrame;

    FunctionRef* allocateFunctionRef(Function& f) {
        auto idk = allocateTyped<FunctionRef>(sizeof(FunctionRef)+(f.totalUpValCount()*sizeof(LoxValue*)), AllocType::FUNCTION_REF);
        idk->func = &f;

        // std::memset(idk->captures, 0, f.totalUpValCount()*sizeof(LoxValue*));

        return idk;
    }

    LoxValue* getUpVal(size_t localId) {
        assert(currentFrame != nullptr);
        return currentFrame->captures[currentFrame->func->getUpValId(localId)];
    }

    void invoke(This &it) override {
        push(getSpecVar(it.hookedTarget));
    }

    void invoke(Super &it) override {
        push(getSpecVar(it.hookedTarget));
    }

    void invoke(Function& it) override {
        // assert(it.hookedTarget != nullptr);
        auto specVar = it.hookedTarget;
        auto* fRef = allocateFunctionRef(it);
        fRef->func = &it;
        fRef->parent = currentFrame;

        setSpecVar(specVar, LoxValue::Function(fRef));

        for (auto i = 0UL; i < it.captures.size(); i++) {
            fRef->captures[it.upValCount()+i] = getUpVal(it.captures[i]);
        }
    }

    void invoke(Class& it) override {
        ClassRef* super = nullptr;
        if (it.data.superClass.has_value()) {
            auto v = getSpecVar(it.hookedSuper);
            assert(v.isClass());
            super = v.asClass();
        }
        auto clazz = new ClassRef{&it, super, currentFrame};
        auto loxClass = LoxValue::Class(clazz);

        setSpecVar(it.hookedDst, loxClass);
    }

    void setSpecVar(SpecializedVariable var, LoxValue value) {
        switch (var.type) {
            case HookedVariableType::LOCAL:
                assert(stackBase < callStack+4096);
                if (VERBOSE) println("STORING LOCAL {}", value);
                stackBase[var.id] = value;
                break;
            case HookedVariableType::UPVAL: {
                auto* frame = currentFrame;
                for (auto i = 0u; i < var.frameId; i++) {
                    assert(frame != nullptr);
                    frame = currentFrame->parent;
                }
                assert(frame != nullptr);
                if(VERBOSE)println("STORING UP_VAL {}", value);
                frame->write(var.id, value);
                break;
            }
            case HookedVariableType::GLOBAL:
                if(VERBOSE)println("STORING GLOBAL {}", value);
                this->globals[var.id] = value;
                break;
            case HookedVariableType::ALLOC_UPVAL:
                currentFrame->captures[var.id] = new LoxValue(value);
                break;
            case HookedVariableType::ALLOC_CONST_UPVAL:
                TODO();
                break;
            case HookedVariableType::CONST_UPVAL:
                TODO();
                break;
        }
    }

    LoxValue getSpecVar(SpecializedVariable var) {
        switch (var.type) {
            case HookedVariableType::LOCAL:
                if (VERBOSE) println("LOADING LOCAL {} - {}", var.id, stackBase[var.id].toString());
                return stackBase[var.id];
            case HookedVariableType::UPVAL: {
                auto* frame = currentFrame;
                for (auto i = 0u; i < var.frameId; i++) {
                    assert(frame != nullptr);
                    frame = frame->parent;
                }
                assert(frame != nullptr);
                if(VERBOSE)println("LOADING UP_VAR {}", frame->read(var.id).toString());
                return frame->read(var.id);
            }
            case HookedVariableType::GLOBAL: {
                auto v = this->globals[var.id];
                if(VERBOSE)println("LOADING GLOBAL {}", v.toString());

                return v;
            }
            case HookedVariableType::CONST_UPVAL:
                TODO();
                break;
            case HookedVariableType::ALLOC_CONST_UPVAL:
                TODO();
                break;
            case HookedVariableType::ALLOC_UPVAL:
                PANIC();
                break;
        }
        UNREACHABLE();
    }

    void invoke(While& it) override {
        while (true) {
            if (it.data.cond.has_value()) {
                (*it.data.cond)->visit(*this);

                if (not pop().toBool()) break;
            }

            it.data.body->visit(*this);
            if (shouldReturn) return;
        }
    }

    void instantiate(ClassRef* clazz, span<ASTExpr> argz) {
        auto constructor = clazz->getConstructor();
        if (constructor == nullptr && not argz.empty()) PANIC();
        if (constructor != nullptr && constructor->data.argz.size() != argz.size()) PANIC();

        auto* res = builtin::rawInstant(clazz, new LoxMap);

        if (constructor != nullptr) {
            auto f = res->getMethod2(CONSTRUCTOR_ID);
            call(*f.asFunction(), argz);
            pop();
        }

        push(LoxValue::Object(res));
    }

    void call(FunctionRef& f, span<ASTExpr> argz) {
        auto& idk = f;

        if (idk.func->data.argz.size() != argz.size()) PANIC("invalid number of args");

        auto localLocalCount = idk.func->locals.size()-idk.func->upValCount();

        assert(idk.func->locals.size() >= idk.func->upValCount());

        auto oldFrame = currentFrame;
        auto* newFrame = &f;

        auto oldStackBase = stackBase;
        auto newStackBase = stackBase - localLocalCount;

        size_t upValId = 0;
        size_t localId = 0;
        for (auto i = 0u; i < argz.size(); i++) {
            argz[i]->visit(*this);
            auto poop = pop();
            if(VERBOSE) println("CALLING WITH: {}", poop.toString());
            if (idk.func->locals[i]) {
                newFrame->captures[upValId] = new LoxValue(poop);
                upValId += 1;
            } else {
                newStackBase[localId] = poop;
                localId += 1;
            }
        }

        stackBase = newStackBase;
        currentFrame = newFrame;

        if (idk.func->native) {
            idk.func->native(*this);
            return;
        }

        auto ip = 0UL;
        while (!shouldReturn && ip < idk.func->data.body.size()) {
            idk.func->data.body[ip]->visit(*this);
            ip += 1;
        }

        // constructor invocation must return `this`
        if (f.func->isMethod && f.func->data.name == "init") {
            if (shouldReturn) {
                shouldReturn = false;
                pop();
                push(*f.captures[f.func->getParamUpValOffset()]); // this
            } else {
                push(*f.captures[f.func->getParamUpValOffset()]); // this
            }
        } else {
            if (shouldReturn) {
                shouldReturn = false;
            } else {
                push(LoxValue::Nil());
            }
        }

        currentFrame = oldFrame;
        stackBase = oldStackBase;
    }

    void invoke(Call& it) override {
        it.fName->visit(*this);

        auto value = pop();

        if (value.isClass()) {
            instantiate(value.asClass(), it.args);
            return;
        }

        if (not value.isFunction()) println("AAAAAAAAAAAAAAAAAA {}", value.toString());
        assert(value.isFunction());
        call(*value.asFunction(), it.args);
    }

    void invoke(NilLiteral& it) override {
        push(LoxValue::Nil());
    }

    void invoke(FieldAccess &it) override {
        it.data.subject->visit(*this);
        auto subj = pop();

        push(builtin::readField(subj, it.fieldId));
    }

    void invoke(Identifier& it) override {
        // TODO();
        // assert(it.data.hookedTarget != nullptr);
        push(getSpecVar(it.data.hookedTarget));
        // push(getFrame()->getVarVal(it.data.value));
    }

    void invoke(Print& it) override {
        it.data.value->visit(*this);

        auto value = pop();

        cout << value.toString() << endl;
    }

    void invoke(VariableDeclaration& it) override {
        auto value = LoxValue::Nil();
        if (it.data.value.has_value()) {
            (*it.data.value)->visit(*this);
            value = pop();
        }
        // assert(it.data.hookedTarget.get() != nullptr);
        setSpecVar(it.data.hookedTarget, value);
        // getFrame()->putVar(it.data.dst, value);
    }

    void invoke(Return& it) override {
        if (it.data.value.has_value()) {
            (*it.data.value)->visit(*this);
        } else {
            push(LoxValue::Nil());
        }
        shouldReturn = true;
    }

    void invoke(Binary& it) override {
        if (it.data.type == BinaryType::AND) {
            it.data.lhs->visit(*this);
            auto lhs = pop();
            if (lhs.toBool()) {
                it.data.rhs->visit(*this);
            } else {
                push(lhs);
            }
            return;
        }
        if (it.data.type == BinaryType::OR) {
            it.data.lhs->visit(*this);
            auto lhs = pop();
            if (not lhs.toBool()) {
                it.data.rhs->visit(*this);
            } else {
                push(lhs);
            }
            return;
        }

        it.data.lhs->visit(*this);
        auto lhs = pop();
        it.data.rhs->visit(*this);
        auto rhs = pop();

        push(builtin::doSimpleBin(it.data.type, lhs, rhs));
    }

    void invoke(Block& it) override {
        auto ip = 0UL;
        // auto stckSize = valueStack.size();
        while (!shouldReturn && ip < it.data.statements.size()) {
            it.data.statements[ip]->visit(*this);
            ip += 1;
        }
        // assert(stckSize == valueStack.size());
    }

    void invoke(Assign& it) override {
        it.data.value->visit(*this);

        auto* dst = it.data.dst/*.get()*/;

        if (dynamic_cast<Identifier*>(dst) != nullptr) {
            auto v = pop();
            setSpecVar(dynamic_cast<Identifier*>(dst)->data.hookedTarget, v);
            push(v); // FIXME this is retarded
        } else if (dynamic_cast<FieldAccess*>(dst) != nullptr) {
            dynamic_cast<FieldAccess*>(dst)->data.subject->visit(*this);
            auto obj = pop();
            auto v = pop();
            auto fName = dynamic_cast<FieldAccess*>(dst);
            builtin::writeField(obj, v, fName->fieldId);
            push(v); // FIXME this is retarded
        } else {
            TODO();
        }
    }

    void invoke(StringLiteral1& it) override {
        TODO();
        // string_view v(it.data.value.data()+1, it.data.value.size()-2);
        // push(LoxValue::String(v));
    }

    void invoke(IF& it) override {
        it.data.cond->visit(*this);
        auto cond = pop();

        if (cond.toBool()) {
            it.data.ifBody->visit(*this);
        } else if (it.data.elsBody.has_value()) {
            (*it.data.elsBody)->visit(*this);
        }
    }

    void invoke(BoolLiteral1& it) override {
        push(LoxValue::Bool(it.data.value));
    }

    void invoke(Negate& it) override {
        it.data.inner->visit(*this);
        auto v = pop();
        push(LoxValue::Bool(!v.toBool()));
    }
};


typedef LoxValue(*GlobalFunck)(FunctionRef*);


FunctionRef* createMethod(Function* f1, FunctionRef* parent, ObjectRef* self) {
    auto f = builtin::allocateClosure(f1, parent).asFunction();
    for (auto i = 0UL; i < f1->captures.size(); i++) {
        f->captures[f1->upValCount()+i] = parent->captures[f1->captures[i]];
    }
    if (self != nullptr) f->setThis(self);

    return f;
}


std::chrono::time_point CLOCK_START = std::chrono::high_resolution_clock::now();

LoxValue loxClock(FunctionRef* self) {
    return LoxValue::Number(duration_cast<std::chrono::microseconds>((std::chrono::high_resolution_clock::now()-CLOCK_START)).count()/1'000'000.0);
}

constexpr std::string_view CONSTRUCTOR_NAME = "init";

string LoxValue::toString() const {
    switch (getType()) {
        case FLOAT: {
            char pepa[16];
            snprintf(pepa, 16, "%G", asNumber());
            return {pepa};
        }
        case FUNCTION_REF:
            return (asFunction()->func->native) ? "<native fn>" : stringify("<fn {}>", asFunction()->func->data.name);
        case NIL:
            return "nil";
        case BOOL:
        case BOOL_FALSE:
            return asBool() ? "true" : "false";
        case STRING:
            return string(asString());
        case CLASS:
            return asClass()->clazz->data.name;
            break;
        case INSTANCE:
            return stringify("{} instance", asObject()->clazz->clazz->data.name);
            break;
    }
    UNREACHABLE();
}

// TODO FIXME!! print() is also VALID
// GLOBALS of the same name reference the same slot, undefined identifier defaults to global
// global block destroys this mechanism
// global slot can be in undefined state
// thats how global functions work
// what about overiding method by assigning function, yes you can it will "shadow" the method
// methods are closures that capture this + super
// constructors are just "init" method, call it on instantiation
// canot use return in init / can only return this, must return this
// vipl is structuraly typed?
// the whole inheritace thingy is weird
// std "clock" returns time since start in seconds
// raylib binding for lox??????????????
// ARRAY, BREAK, CONTINUE, CONST keyword, IF isType..., REPL, DEBUGER?
// only repo requirement, RUN TESTS
// merge requests for checking stuff...
// FIXME THIS IS FUCKED!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
// cache maps in cache
// maps have creation order
// mby just cache orders? statically?
// map creation depends on order
// how to handle adding new proepry? we need to realocate the object?
// js spec "species"
// around 8 objects inline
// some fancy magic
// IF WE KNOW THAT METHOD is not compared we can use differen calling convention
// FIXME this is REAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAALY BAD implementation of JIT

// interpret loop has been replaced with builtin call overhead
// register allocation is netured with __locals shajze
// evrything around objects is slow - lookup, methods, EVERYTHING

struct BitsetView {
    char* data;
    size_t bitSize;

    size_t count() {
        size_t acu = 0;
        for (auto i = 0ul; i < this->bitSize; i++) {
            if (get(i)) acu += 1;
        }

        return acu;
    }

    void set(size_t index, bool value) {
        assert(index < bitSize);
        auto v = data[index / 8];

        // println("MRDA? {} == {}", (int)v, (int)data[index/8]);
        if (value) {
            data[index / 8] = v;
            data[index / 8] = v | (1 << (index % 8));
        } else {
            data[index / 8] = ~(~v | (1 << (index % 8)));
        }
    }

    bool get(size_t index) {
        assert(index < bitSize);
        return (data[index / 8] & (1 << (index % 8))) != 0;
    }

    void clear() {
        for (auto i = 0ul; i < this->bitSize; i++) {
            set(i, false);
        }
    }
};


/// TODO modulo classe for FunctionRef
/// TODO HASH MAP BUCKETS ARE ALSO SIMPLE BCS THEIR SIZE GROWS EXPONENTIONALLY ... no wasted bytes
struct Heap {
#define GC_LOG(stuff, ...) if (debugGc) println(stuff __VA_OPT__(,) __VA_ARGS__);
    size_t gcCount = 0;
    size_t blockReclaimCount = 0;
    void* stackStart;

    char* start;
    size_t heapSize;

    char* firstFreeBigBlock;
    size_t freeBigBlocks = 0;

    void setupPages() {
        heapSize = 48ul*1024ul*1024ul;
        start = (char*)mmap(nullptr, heapSize+BIG_BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        GC_LOG("[heap] setup {}", (void*)start);
        auto oldStart = start;
        start = (char*)((uintptr_t)start & ~(BIG_BLOCK_SIZE-1));
        if (start != oldStart) start += BIG_BLOCK_SIZE;
        heapSize = (heapSize/BIG_BLOCK_SIZE)*BIG_BLOCK_SIZE;
        GC_LOG("[heap] setup aligned {}", (void*)start);

        firstFreeBigBlock = nullptr;

        // ((BigBlock*)firstFreeBigBlock)->next = nullptr;

        for (auto i = 0ul; i < heapSize; i += BIG_BLOCK_SIZE) {
            freeBigBlock(start+i);
        }
    }

    void freeBigBlock(char* other) {
        GC_LOG("[heap] freeBigBlock {}", (void*)other);
        freeBigBlocks += 1;

        ASAN_UNPOISON_MEMORY_REGION(other, BIG_BLOCK_SIZE);
        // std::memset(other, 0, BIG_BLOCK_SIZE);
        ((BigBlock*)other)->setIsAlive(false);

        auto cpy = firstFreeBigBlock;
        firstFreeBigBlock = other;
        ((BigBlock*)(other))->next = cpy;

        GC_LOG("[poison] poisoning block free {} - {}", (void*)other, bigBlockToId((BigBlock*)other));

        ASAN_POISON_MEMORY_REGION(other, BIG_BLOCK_SIZE);
    }

    char* allocBigBlock() {
        if (firstFreeBigBlock == nullptr) {
            GC_LOG("[heap] allocBigBlock null");
            return nullptr;
        }
        freeBigBlocks -= 1;

        auto bb = (BigBlock *) firstFreeBigBlock;
        ASAN_UNPOISON_MEMORY_REGION(bb, BIG_BLOCK_SIZE);
        firstFreeBigBlock = bb->next;

        // std::memset(bb, 0, BIG_BLOCK_SIZE);

        GC_LOG("[poison] poisoning allocation range {} - {}", bb, bigBlockToId(bb));

        ASAN_POISON_MEMORY_REGION(((char*)bb)+sizeof(BigBlock), BIG_BLOCK_SIZE-sizeof(BigBlock));

        GC_LOG("[heap] allocBigBlock {} - {}", bb, bigBlockToId(bb));

        return (char*)bb;
    }

    bool isManagedPtr(void* ptr) {
        auto bigBlock = ptrToBigBlock((uintptr_t)ptr);

        if (not isInsideHeap((uintptr_t)ptr)) {
            GC_LOG("-  [gc] ignored ptr bcs its outside heap {}", bigBlock);
            return false;
        }

        if (IS_POISONED(bigBlock) or not bigBlock->isAlive()) {
            GC_LOG("-  [gc] ignored ptr bcs it points into free block {}", bigBlock);
            return false;
        }

        GC_LOG("- [gc] big block {} - {} - {} - {}", bigBlock, allocToString(bigBlock->type), bigBlock->granularity, ((uintptr_t)bigBlock-(uintptr_t)start)/BIG_BLOCK_SIZE);

        auto ptrValue = (uintptr_t)ptr % bigBlock->granularity;

        if ((uintptr_t)ptr < bigBlock->calculateBaseAddress()) {
            GC_LOG("- [gc] ignoring ptr, it points into block meta {}", (void*)ptr);
            return false;
        }

        if ((uintptr_t)ptr >= (size_t)bigBlock->getBitsetAddr()) {
            GC_LOG("- [gc] ignoring ptr, points into mark bits {}", (void*)ptr);
            return false;
        }

        if (ptrValue != 0) {
            GC_LOG("- [gc] ignoring ptr granularity does not match {} expected {}", (void*)ptr, bigBlock->granularity);
            return false;
        }

        if (not bigBlock->isAllocated(ptr)) {
            GC_LOG("- [gc] ignoring ptr, is not allocated {}", (void*)ptr);
            return false;
        }

        return true;
    }

    void markPtr(char* ptr) {
        assert(isManagedPtr(ptr));

        auto bigBlock = ptrToBigBlock((uintptr_t)ptr);

        GC_LOG("[gc] marking {} - {}", (void*)ptr, allocToString(bigBlock->type));

        auto a = bigBlock->calculateMarkedCount();
        bigBlock->setIsMarkedIndex(bigBlock->ptrToIndex(ptr), true);
        auto b = bigBlock->calculateMarkedCount();
        (void)a;
        (void)b;
        assert(a+1 == b);
    }

    bool isMarkedPtr(char* ptr) {
        if (((uintptr_t)ptr & LoxValue::NAN_MASK) == LoxValue::NAN_MASK) {
            GC_LOG("[gc] pointer looks like LoxValue {}", (void*)ptr);
            PANIC();
        }
        if (not isInsideHeap((uintptr_t)ptr)) return true;
        assert(isInsideHeap((uintptr_t)ptr));

        auto* bigBlock = ptrToBigBlock((uintptr_t)ptr);

        return bigBlock->isMarkedIndex(bigBlock->ptrToIndex(ptr));
    }

    bool cmpMark(char* ptr) {
        if (isMarkedPtr(ptr)) return false;

        markPtr(ptr);

        return true;
    }

    // inspired by
    // https://webkit.org/blog/12967/understanding-gc-in-jsc-from-scratch/

    static constexpr size_t BIG_BLOCK_SIZE = 64*1024;

    enum class BigBlockType {
        FREE_LIST,
        BUMP
    };

    static constexpr size_t allocTypeToConstantSize(AllocType type) {
        switch (type) {
            case AllocType::LOX_VALUE: return sizeof(LoxValue);
            case AllocType::HASH_MAP: return sizeof(LoxMap);
            case AllocType::CLASS_REF: return sizeof(ClassRef);
            case AllocType::OBJECT_REF: return sizeof(ObjectRef);
            case AllocType::HASH_MAP_BUCKET: return LoxMap::BBUCKET_SIZE;
            case AllocType::STRING: return 0;
            case AllocType::FUNCTION_REF: return 0;
            case AllocType::HASH_MAP_BUCKET_ARRAY: return 0;
        }
        PANIC()
    }


    struct BigBlock {
        AllocType type;
        BigBlockType blockType;
        size_t flags;
        size_t granularity;
        char* next;
        char* prev;
        char* base;
        char* end;
        size_t granulaeCount;
        bool isEvenMarked;
        bool isPinned;

        char data[];

        void setIsBeignAllocated(bool isAllocated) {
            flags |= (isAllocated ? 1 : 0) << 1;
        }

        bool isBeignAllocated() {
            return (flags & (1 << 1)) != 0;
        }

        void setIsAlive(bool isAlive) {
            flags |= (isAlive ? 1 : 0) << 0;
        }

        bool isAlive() {
            return flags & 1;
        }

        size_t calculateMarkedCount() {
            size_t acu = 0;

            for (auto i = 0ul; i < constantItemCount(); i++) {
                if (isMarkedIndex(i)) acu += 1;
            }

            return acu;
        }

        size_t markedCount() {
            if (not isEvenMarked) {
                return 0;
            }

            return calculateMarkedCount();
        }

        size_t calculateAllocatedCount() {
            size_t acu = 0;

            for (auto i = 0ul; i < constantItemCount(); i++) {
                if (isAllocatedIndex(i)) acu += 1;
            }

            return acu;
        }

        size_t getObjectSize(size_t index) {
            switch (type) {
                case AllocType::STRING:
                    return ((LoxStr*)base+(granularity*index))->calculateSize();
                case AllocType::FUNCTION_REF:
                    return ((FunctionRef*)base+(granularity*index))->calculateSize();
                case AllocType::HASH_MAP_BUCKET_ARRAY:
                TODO();
                default:
                    return allocTypeToConstantSize(type);
            }
        }

        uintptr_t calculateBaseAddress() {
            return align((uintptr_t)this+sizeof(BigBlock), granularity);
        }

        size_t getOrder(void* ptr) {
            switch (type) {
                case AllocType::LOX_VALUE: return sizeof(LoxValue);
                case AllocType::HASH_MAP: return sizeof(LoxMap);
                case AllocType::CLASS_REF: return sizeof(ClassRef);
                case AllocType::OBJECT_REF: return sizeof(ObjectRef);
                case AllocType::STRING: TODO();
                case AllocType::HASH_MAP_BUCKET: return LoxMap::BBUCKET_SIZE;
                case AllocType::FUNCTION_REF: ((FunctionRef*)(ptr))->func->totalUpValCount();
                case AllocType::HASH_MAP_BUCKET_ARRAY: TODO();
            }
        }

        static constexpr size_t PER_GRANULE_BITS = 2;

        size_t callculateConstantItemCount() {
            auto available = BIG_BLOCK_SIZE-(calculateBaseAddress()-(uintptr_t)this);

            auto avialableBits = available*8;
            auto itemBitSize = granularity*8 + PER_GRANULE_BITS;

            auto itemCount = avialableBits / itemBitSize;

            return itemCount;
        }

        size_t constantItemCount() {
            assert(granulaeCount != 0);
            return granulaeCount;
        }

        size_t bitsetSizeBytes() {
            auto itemz = constantItemCount()*PER_GRANULE_BITS;
            auto bytes = itemz/8;
            if (itemz % 8 != 0) bytes += 1;
            return bytes;
        }

        char* getBitsetAddr() {
            auto nItems = constantItemCount();
            auto startPtr = calculateBaseAddress()+(nItems*granularity);

            return (char*)startPtr;
        }

        bool isFull() {
            if (blockType == BigBlockType::BUMP) {
                return base + granularity > end;
            } else {
                return base == nullptr;
            }
        }

        BitsetView getMarkBitSet() {
            return BitsetView{this->getBitsetAddr(), this->constantItemCount()*PER_GRANULE_BITS};
        }

        uintptr_t endAddress() {
            return ((uintptr_t)this)+BIG_BLOCK_SIZE;
        }

        size_t ptrToIndex(char* ptr) {
            assert((uintptr_t)ptr >= (uintptr_t )this->calculateBaseAddress());
            assert((uintptr_t)ptr < this->endAddress());

            auto cc = (uintptr_t)ptr-this->calculateBaseAddress();

            return cc/granularity;
        }

        void* allocateBump(size_t n) {
            auto self = base;

            if (self + (granularity*n) > end) {
                // GC_LOG("[heap] allocateBump OOM");
                return nullptr;
            }

            base += (granularity*n);

            return self;
        }


        void clearMarkBits(bool clearAllocatedBits) {
            isEvenMarked = false;
            for (auto i = 0ul; i < constantItemCount(); i++) {
                if (clearAllocatedBits && not isMarkedIndex(i)) setIsAllocatedIndex(i, false);
                this->setIsMarkedIndex(i, false);
            }
        }

        bool isConstantSize() {
            switch (type) {
                case AllocType::LOX_VALUE: return true;
                case AllocType::HASH_MAP: return true;
                case AllocType::CLASS_REF: return true;
                case AllocType::OBJECT_REF: return true;
                case AllocType::HASH_MAP_BUCKET: return true;
                case AllocType::STRING: return false;
                case AllocType::FUNCTION_REF: return false;
                case AllocType::HASH_MAP_BUCKET_ARRAY: return false;
            }
            PANIC();
        }

        void* allocateFreeList(size_t n) {
            assert(n == 1);

            auto self = this->base;

            if (self == nullptr)
                return nullptr;

            if (isConstantSize()) {
                this->base = *((char**)self);
            } else {
                auto parentPtr = (FreeSlot**)this->base;
                auto based = (FreeSlot*)this->base;
                auto requested = n*this->granularity;

                while (true) {
                    if (based == nullptr) {
                        return nullptr;
                    }

                    if (based->size == requested) { // we consumed the entire free space
                        *parentPtr = based->next;

                        return based;
                    } else if (based->size < requested) { // not enough space :(
                        based = based->next;
                        parentPtr = &based->next;
                    } else { // more space than we need :(
                        auto rem = based->size-requested;

                        auto sliced = (FreeSlot*)((char*)based)+requested;
                        sliced->size = rem;
                        sliced->next = based->next;

                        *parentPtr = sliced;

                        return based;
                    }
                }
            }

            return self;
        }

        void markAllocated(void* ptr) {
            auto objIdex = ptrToIndex((char*)ptr);
            setIsAllocatedIndex(objIdex, true);
        }

        void setIsAllocatedIndex(size_t index, bool value) {
            getMarkBitSet().set(constantItemCount()+index, value);
        }

        void setIsMarkedIndex(size_t index, bool value) {
            if (value) isEvenMarked = true;
            getMarkBitSet().set(index, value);
        }

        bool isAllocated(void* ptr) {
            auto objIdex = ptrToIndex((char*)ptr);
            return isAllocatedIndex(objIdex);
        }

        bool isAllocatedIndex(size_t index) {
            return getMarkBitSet().get(constantItemCount()+index);
        }

        bool isMarkedIndex(size_t index) {
            return getMarkBitSet().get(index);
        }

        void* allocate(size_t order = 1)  {
            void* res;
            if (blockType == BigBlockType::BUMP) {
                res = allocateBump(order);
            } else {
                res = allocateFreeList(order);
            }

            if (res != nullptr) {
                markAllocated(res);
                assert((uintptr_t)res % granularity == 0);

                ASAN_UNPOISON_MEMORY_REGION(res, granularity*order);
            }

            return res;
        }
    };

    size_t bigBlockToId(BigBlock* bb) {
        return ((uintptr_t)bb - (uintptr_t)start) / BIG_BLOCK_SIZE;
    }

    struct FreeSlot {
        FreeSlot* next;
        size_t size;
    };

    struct FreeBigBlock {
        void* next;
    };

    // allocation
    BigBlock* LOX_VALUE_BIG_BLOCK = nullptr;
    BigBlock* SLOW_LOX_VALUE_BIG_BLOCK = nullptr;

    BigBlock* OBJECT_BIG_BLOCK = nullptr;
    BigBlock* SLOW_OBJECT_BIG_BLOCK = nullptr;

    BigBlock* MAP_BIG_BLOCK = nullptr;
    BigBlock* SLOW_MAP_BIG_BLOCK = nullptr;

    BigBlock* CLASS_BIG_BLOCK = nullptr;
    BigBlock* SLOW_CLASS_BIG_BLOCK = nullptr;

    BigBlock* MAP_BUCKET_BIG_BLOCK = nullptr;
    BigBlock* SLOW_MAP_BUCKET_BIG_BLOCK = nullptr;

    BigBlock* STRING_BIG_BLOCK = nullptr;
    BigBlock* SLOW_STRING_BIG_BLOCK = nullptr;

    BigBlock* BUCKETS_BIG_BLOCK = nullptr;
    BigBlock* SLOW_BUCKETS_BIG_BLOCK = nullptr;

    // modulo alloc classes for FunctionRef, this prevents wasting of precious bytes, ... we will still waste mark bits
    constexpr static size_t IDK = sizeof(FunctionRef)/sizeof(LoxValue);
    BigBlock* FUNCTION_REF_BIG_BLOCK[IDK] = {};
    BigBlock* SLOW_FUNCTION_REF_BIG_BLOCK[IDK] = {};

    BigBlock* putCureentBigBlock(BigBlock*& oldBlock, BigBlock* newBlock) {
        if (oldBlock != nullptr) {
            oldBlock->setIsBeignAllocated(false);
        }
        newBlock->setIsBeignAllocated(true);

        return newBlock;
    }

    bool debugGc = false;

    size_t getFunctionRefBigBlockIndex(size_t closedCount) {
        return closedCount % IDK;
    }

    size_t getFunctionRefBigBlockIndexBySize(size_t size) {
        return getFunctionRefBigBlockIndex((size-sizeof(FunctionRef))/sizeof(LoxValue));
    }

    BigBlock* getFunctionBigBlockBySize(size_t size) {
        return FUNCTION_REF_BIG_BLOCK[((size-sizeof(FunctionRef))/sizeof(LoxValue))%IDK];
    }

    constexpr BigBlock* setupBigBlock(char* ptr, AllocType type) {
        assert(ptr != nullptr);
        return setupBigBlock(ptr, type, allocTypeToConstantSize(type));
    }

    BigBlock* setupBigBlock(char* ptr, AllocType type, size_t granularity) {
        GC_LOG("[heap] setupBigBlock {} - {} - {}", (void*)ptr, allocToString(type), granularity);
        auto bb = (BigBlock*)ptr;
        bb->type = type;
        bb->blockType = BigBlockType::BUMP;
        bb->granularity = granularity;
        bb->next = nullptr;
        bb->prev = nullptr;

        bb->granulaeCount = bb->callculateConstantItemCount();
        bb->end = (char*)(bb->calculateBaseAddress()+(bb->constantItemCount()*granularity));
        bb->base = (char*)bb->calculateBaseAddress();
        bb->isEvenMarked = false;
        bb->setIsAlive(true);

        // ASAN_UNPOISON_MEMORY_REGION(bb->end, BIG_BLOCK_SIZE-(uintptr_t)bb->end);
        auto headerSize = sizeof(BigBlock);
        auto dataSize = bb->constantItemCount()*granularity;
        auto bitsetSize = bb->bitsetSizeBytes();

        GC_LOG("[heap] BIG BOLEST {} - {} - {} - {}", headerSize, dataSize, bitsetSize, BIG_BLOCK_SIZE);
        assert((headerSize + dataSize + bitsetSize) <= BIG_BLOCK_SIZE);

        ASAN_UNPOISON_MEMORY_REGION(bb->getBitsetAddr(), bitsetSize);

        GC_LOG("[poison] unpoisoning bitset {} - {}", bb, bigBlockToId(bb));

        // FIXME shouldn be needed bcs this is called on freshly allocated block which is zeroed
        // bb->clearMarkBits();

        return bb;
    }

    constexpr BigBlock** allocTypeToCurrentAlloc(AllocType type) {
        switch (type) {
            case AllocType::LOX_VALUE: return &LOX_VALUE_BIG_BLOCK;
            case AllocType::HASH_MAP: return &MAP_BIG_BLOCK;
            case AllocType::CLASS_REF: return &CLASS_BIG_BLOCK;
            case AllocType::OBJECT_REF: return &OBJECT_BIG_BLOCK;
            case AllocType::STRING: return &STRING_BIG_BLOCK;
            case AllocType::HASH_MAP_BUCKET: return &BUCKETS_BIG_BLOCK;
            case AllocType::HASH_MAP_BUCKET_ARRAY: return &MAP_BUCKET_BIG_BLOCK;
            case AllocType::FUNCTION_REF: return nullptr;
        }
    }

    constexpr BigBlock** allocTypeToCurrentAlloc(AllocType type, size_t size) {
        switch (type) {
            case AllocType::LOX_VALUE: return &LOX_VALUE_BIG_BLOCK;
            case AllocType::HASH_MAP: return &MAP_BIG_BLOCK;
            case AllocType::CLASS_REF: return &CLASS_BIG_BLOCK;
            case AllocType::OBJECT_REF: return &OBJECT_BIG_BLOCK;
            case AllocType::STRING: return &STRING_BIG_BLOCK;
            case AllocType::HASH_MAP_BUCKET: return &BUCKETS_BIG_BLOCK;
            case AllocType::HASH_MAP_BUCKET_ARRAY: return &MAP_BUCKET_BIG_BLOCK;
            case AllocType::FUNCTION_REF: return &FUNCTION_REF_BIG_BLOCK[getFunctionRefBigBlockIndexBySize(size)];
        }
        PANIC();
    }

    constexpr BigBlock** allocTypeToSlowAlloc(AllocType type) {
        switch (type) {
            case AllocType::LOX_VALUE: return &SLOW_LOX_VALUE_BIG_BLOCK;
            case AllocType::HASH_MAP: return &SLOW_MAP_BIG_BLOCK;
            case AllocType::CLASS_REF: return &SLOW_CLASS_BIG_BLOCK;
            case AllocType::OBJECT_REF: return &SLOW_OBJECT_BIG_BLOCK;
            case AllocType::STRING: return &SLOW_STRING_BIG_BLOCK;
            case AllocType::HASH_MAP_BUCKET: return &SLOW_BUCKETS_BIG_BLOCK;
            case AllocType::HASH_MAP_BUCKET_ARRAY: return &SLOW_MAP_BUCKET_BIG_BLOCK;
            case AllocType::FUNCTION_REF: TODO();
        }
        PANIC();
    }

    constexpr BigBlock** getFastByGranularity(AllocType type, size_t granularity) {
        switch (type) {
            case AllocType::LOX_VALUE: return &LOX_VALUE_BIG_BLOCK;
            case AllocType::HASH_MAP: return &MAP_BIG_BLOCK;
            case AllocType::CLASS_REF: return &CLASS_BIG_BLOCK;
            case AllocType::OBJECT_REF: return &OBJECT_BIG_BLOCK;
            case AllocType::STRING: return &STRING_BIG_BLOCK;
            case AllocType::HASH_MAP_BUCKET: return &BUCKETS_BIG_BLOCK;
            case AllocType::HASH_MAP_BUCKET_ARRAY: return &MAP_BUCKET_BIG_BLOCK;
            case AllocType::FUNCTION_REF: return &FUNCTION_REF_BIG_BLOCK[getFunctionRefBigBlockIndexBySize(granularity)];
        }
        PANIC();
    }

    constexpr BigBlock** getSlowByGranularity(AllocType type, size_t granularity) {
        switch (type) {
            case AllocType::LOX_VALUE: return &SLOW_LOX_VALUE_BIG_BLOCK;
            case AllocType::HASH_MAP: return &SLOW_MAP_BIG_BLOCK;
            case AllocType::CLASS_REF: return &SLOW_CLASS_BIG_BLOCK;
            case AllocType::OBJECT_REF: return &SLOW_OBJECT_BIG_BLOCK;
            case AllocType::STRING: return &SLOW_STRING_BIG_BLOCK;
            case AllocType::HASH_MAP_BUCKET: return &SLOW_BUCKETS_BIG_BLOCK;
            case AllocType::HASH_MAP_BUCKET_ARRAY: return &SLOW_MAP_BUCKET_BIG_BLOCK;
            case AllocType::FUNCTION_REF: TODO();
        }
        PANIC();
    }

    constexpr BigBlock** allocTypeToSlowAlloc(AllocType type, size_t size) {
        switch (type) {
            case AllocType::LOX_VALUE: return &SLOW_LOX_VALUE_BIG_BLOCK;
            case AllocType::HASH_MAP: return &SLOW_MAP_BIG_BLOCK;
            case AllocType::CLASS_REF: return &SLOW_CLASS_BIG_BLOCK;
            case AllocType::OBJECT_REF: return &SLOW_OBJECT_BIG_BLOCK;
            case AllocType::STRING: return &SLOW_STRING_BIG_BLOCK;
            case AllocType::HASH_MAP_BUCKET: return &SLOW_BUCKETS_BIG_BLOCK;
            case AllocType::HASH_MAP_BUCKET_ARRAY: return &SLOW_MAP_BUCKET_BIG_BLOCK;
            case AllocType::FUNCTION_REF: return &SLOW_FUNCTION_REF_BIG_BLOCK[getFunctionRefBigBlockIndexBySize(size)];
        }
        PANIC();
    }

    constexpr BigBlock* allocateInitilizeBigBlock(AllocType type, size_t size) {
        auto block = allocTypeToCurrentAlloc(type, size);

        // try to get free block
        auto newBlock = allocBigBlock();
        if (newBlock == nullptr) {
            GC_LOG("[heap] doTypeAlloc OOM - {}", allocToString(type));
            doGc();
            newBlock = allocBigBlock();

            if (newBlock == nullptr) {
                auto slowBlock = allocTypeToSlowAlloc(type, size);
                if (*slowBlock == nullptr) {
                    GC_LOG("[heap] exiting not even free list is available to satisfy allocation after gc :(");
                    PANIC();
                }

                return putCureentBigBlock(*block, *slowBlock);
            } else {
                return putCureentBigBlock(*block, setupBigBlock(newBlock, type, calculateGranularity(type, size)));
            }
        } else {
            return putCureentBigBlock(*block, setupBigBlock(newBlock, type, calculateGranularity(type, size)));
        }
    }

    constexpr size_t calculateGranularity(AllocType type, size_t size) {
        switch (type) {
            case AllocType::FUNCTION_REF: return sizeof(FunctionRef)+(getFunctionRefBigBlockIndex((size-sizeof(FunctionRef))/sizeof(LoxValue))*8);
            case AllocType::HASH_MAP_BUCKET_ARRAY: return LoxMap::BUCKET_ARRAY_BASE_SIZE;
            case AllocType::STRING: return 16;
            default: return allocTypeToConstantSize(type); // constant size allocation
        }

        PANIC();
    }

    constexpr size_t calculateAllocationOrder(AllocType type, size_t size) {
        auto granularity = calculateGranularity(type, size);

        if (type != AllocType::STRING && size % granularity != 0) {
            GC_LOG("trying to allocate {} with size {} and granularity {}", allocToString(type), size, granularity);
            PANIC();
        }

        switch (type) {
            case AllocType::FUNCTION_REF: return size / granularity;
            case AllocType::HASH_MAP_BUCKET_ARRAY: return size / granularity;
            case AllocType::STRING: return align(size, 16)/16;
            default: return 1; // constant size allocation
        }
    }

    constexpr void* doAllocationGeneric(AllocType type, size_t size) {
        auto block = allocTypeToCurrentAlloc(type, size);
        auto allocationOrder = calculateAllocationOrder(type, size);

        auto blk = *block;

        if (blk == nullptr) {
            GC_LOG("[heap] blk is null WTF?");
            blk = allocateInitilizeBigBlock(type, size);
            *block = blk;
        }

        auto allocated = blk->allocate(allocationOrder);

        if (allocated == nullptr) {
            GC_LOG("[heap] allocated is null new block");
            blk = allocateInitilizeBigBlock(type, size);
            *block = blk;

            // FIXME this can still fail when allocating big value eg string that is larger than available space, OR when using free list allocator ... well we failed to get free block which means we are low on memory / fragmentation
            allocated = blk->allocate(allocationOrder);
        }

        if (allocated == nullptr) {
            GC_LOG("[heap] strange OOM - {}", allocToString(type));
            PANIC();
        }

        std::memset(allocated, 0, size);

        return allocated;
    }

    constexpr void* allocate(AllocType type, size_t size) {
        // doGc();
        return doAllocationGeneric(type, size);
    }

    bool isInsideHeap(uintptr_t ptr) {
        return ptr >= (uintptr_t)this->start && ptr < (((uintptr_t)this->start)+this->heapSize);
    }

    void visitConservativePtr(uintptr_t ptr, std::vector<uintptr_t>& workList) {
        if (!isInsideHeap(ptr)) return;

        // ptr is possible heap allocation
        GC_LOG("[gc] value points into heap {} - {}", (void*)ptr, bigBlockToId((BigBlock*)ptr));
        // try to determine to which object it points
        // 1. we could set allocation granularity and manage bit set of allocated objects, query it to find out if
        if (isManagedPtr((char*)ptr)) {
            GC_LOG("- [gc] ROOT PTR FOUND {}", (void*)ptr);
            workList.push_back((uintptr_t)ptr);
        }
    }

    void collectPossibleValue(uintptr_t value, std::vector<uintptr_t>& workList) {
        if ((value & LoxValue::NAN_MASK) == LoxValue::NAN_MASK) { // if value looks like nan try to cellect it as lox value
            GC_LOG("[gc] value {} looks like LoxValue", (void*)value);
            visitConservativePtr(value & LoxValue::DATA_MASK, workList);
            visitConservativePtr(value, workList);
        } else {
            visitConservativePtr(value, workList);
        }
    }

    template<typename T>
    void collectManagedPtr(T* self) {
        if (self == nullptr) return;
        if (this->cmpMark((char*)self))
            collect(self);
    }

    void collect(LoxValue* self) {
        collectLox(*self);
    }

    void collect(FunctionRef* self) {
        collectManagedPtr(self->parent);

        auto captures = self->func->totalUpValCount();
        for (auto i = 0ul; i < captures; i++) {
            auto v = self->captures[i];
            auto isImutable = not self->func->isModifiedCaptured(i);

            if (isImutable) {
                collectLox(std::bit_cast<LoxValue>(v));
            } else {
                if (v != nullptr) collect(v);
            }
        }
    }

    void collect(ClassRef* self) {
        collectManagedPtr(self->super);

        collectManagedPtr(self->parent);

        collect(&self->methods);
    }

    void collect(LoxStr* self) {
        // do nothing
    }

    void collect(ObjectRef* self) {
        collectManagedPtr(self->clazz);

        collectManagedPtr(self->proto);

        collectManagedPtr(self->construcor);

        collectManagedPtr(self->fields);
    }

    void collect(LoxMapBucket* self) {
        forEachBucket((LoxMapBucket*)self, [&](size_t v) {
            if (((uintptr_t)v & LoxValue::NAN_MASK) == LoxValue::NAN_MASK) {
                collectLox(std::bit_cast<LoxValue>(v));
            } else {
                collectManagedPtr(std::bit_cast<FunctionRef*>(v));
            }
        });
    }

    void collect(LoxMapBucketArray* self) {
        for (auto i = 0ul; i < self->size; i++) {
            collectManagedPtr(self->buckets[i]);
        }
    }

    void collect(LoxMap* self) {
        collectManagedPtr(self->bucks);
    }

    void collectLox(LoxValue v) {
        switch (v.getType()) {
            case LoxValue::ValueType2::FLOAT:
            case LoxValue::ValueType2::BOOL:
            case LoxValue::ValueType2::BOOL_FALSE:
            case LoxValue::ValueType2::NIL:
                break;
            case LoxValue::ValueType2::CLASS:
                collectManagedPtr(v.asClass());
                break;
            case LoxValue::ValueType2::INSTANCE:
                collectManagedPtr(v.asObject());
                break;
            case LoxValue::ValueType2::FUNCTION_REF:
                collectManagedPtr(v.asFunction());
                break;
            case LoxValue::ValueType2::STRING:
                collectManagedPtr(v.asLoxStr());
                break;
        }
    }

    void collectRegisterRoots(std::vector<uintptr_t>& roots) {
#ifdef __x86_64__
        uintptr_t regz[5];

        asm ("mov %%rbx, %0\n"
             "mov %%r12, %1\n"
             "mov %%r13, %2\n"
             "mov %%r14, %3\n"
             "mov %%r15, %4\n": "=rm" (regz[0]), "=rm" (regz[1]), "=rm" (regz[2]), "=rm" (regz[3]), "=rm" (regz[4]));

        for (auto ptr : regz) {
            collectPossibleValue(ptr, roots);
        }
#else
#error unsuported architecture TODO arm64
#endif
    }

    __attribute__((no_sanitize("address")))
    void collectStackRoots(void* start1, void* end, std::vector<uintptr_t>& workList) {
        GC_LOG("[gc] collecting stack roots {}..{} - {}B", end, start1, (uintptr_t)start1-(uintptr_t)end);


        auto s = (uintptr_t*)std::min(start1, end);
        auto e = (uintptr_t*)std::max(start1, end);

        for (auto i = s; i <= e; i++) {
            collectPossibleValue(*i, workList);
        }
    }

    BigBlock* ptrToBigBlock(uintptr_t ptr) {
        auto bigBlockBits = (size_t)std::log2(Heap::BIG_BLOCK_SIZE);
        return std::bit_cast<BigBlock*>((std::bit_cast<u64>(ptr) >> bigBlockBits) << bigBlockBits);
    }

    void freeAndCleanupBlock(BigBlock* bb) {
        if (bb->isBeignAllocated()) { // bb is either fast alloc variable, or part of freelist
            auto block  = getFastByGranularity(bb->type, bb->granularity);
            if (*block == bb) {
                *block = nullptr;
            }

            if (bb->blockType == BigBlockType::FREE_LIST) {
                if (bb->prev == nullptr) {
                    auto idk = getSlowByGranularity(bb->type, bb->granularity);
                    *idk = (BigBlock*)bb->next;
                } else {
                    bb->prev = bb->next;
                }
            }
        }

        freeBigBlock((char*)bb);
    }

    void setupFreeList(BigBlock* bb) {
        GC_LOG("[gc] block is full setup free-list {}", bigBlockToId(bb));

        assert(Heap::allocTypeToConstantSize(bb->type) != 0);
        auto isConstantBlock = bb->isConstantSize();

        ASAN_UNPOISON_MEMORY_REGION(bb, BIG_BLOCK_SIZE);

        if (isConstantBlock) {
            void* lastAddress = nullptr;

            auto baseAddress = bb->calculateBaseAddress();

            for (auto j = 0ul; j < bb->constantItemCount(); j++) {
                if (bb->isMarkedIndex(j)) continue;

                auto selfAdr = (void*)(baseAddress+j*bb->granularity);

                std::memcpy(selfAdr, &lastAddress, sizeof lastAddress);
                lastAddress = selfAdr;
            }

            bb->base = (char*)lastAddress;
        } else {
            void* lastAddress = nullptr;

            char* freeAddr = nullptr;
            size_t freeIndex = 0;

            auto baseAddress = bb->calculateBaseAddress();

            auto linkFreeList = [&](size_t last) {
                if (freeAddr != nullptr) {
                    auto freeSizeBytes = (last-freeIndex)*bb->granularity;

                    if (freeSizeBytes == 0) PANIC();

                    std::memcpy(freeAddr, &lastAddress, 8);
                    std::memcpy(freeAddr+8, &freeSizeBytes, 8);

                    freeIndex = 0;
                    freeAddr = nullptr;
                }
            };

            for (auto j = 0ul; j < bb->constantItemCount(); j++) {
                if (bb->isAllocatedIndex(j) && bb->isMarkedIndex(j)) {
                    linkFreeList(j);

                    auto objSize = bb->getObjectSize(j);
                    if (objSize % bb->granularity != 0) PANIC();
                    auto nGranule = objSize / bb->granularity;
                    if (nGranule == 0) PANIC();
                    j += nGranule - 1;
                    continue;
                }
                if (freeAddr == nullptr) {
                    freeAddr = (char*)baseAddress+(j*bb->granularity);
                    freeIndex = j;
                }
            }

            linkFreeList(bb->constantItemCount());
        }


        bb->blockType = BigBlockType::FREE_LIST;
        bb->clearMarkBits(true);
    }

    void markFromRoots(std::unordered_set<void*>& roots) {
        for (auto p : roots) {
            auto block = ptrToBigBlock((uintptr_t)p);

            switch (block->type) {
                case AllocType::LOX_VALUE:
                    collectManagedPtr((LoxValue*)p);
                    break;
                case AllocType::CLASS_REF:
                    collectManagedPtr((ClassRef*)p);
                    break;
                case AllocType::OBJECT_REF:
                    collectManagedPtr((ObjectRef*)p);
                    break;
                case AllocType::STRING:
                    collectManagedPtr((LoxStr*)p);
                    break;
                case AllocType::FUNCTION_REF:
                    collectManagedPtr((FunctionRef*)p);
                    break;
                case AllocType::HASH_MAP:
                    collectManagedPtr((LoxMap*)p);
                    break;
                case AllocType::HASH_MAP_BUCKET_ARRAY:
                    collectManagedPtr((LoxMapBucketArray*)p);
                    break;
                case AllocType::HASH_MAP_BUCKET:
                    collectManagedPtr((LoxMapBucket*)p);
                    break;
            }
        }
    }

    void sweepGarbage() {
        size_t blocksReclaimed = 0;
        size_t freeListsCreated = 0;


        for (auto i = (uintptr_t)start; i < (uintptr_t)(start+heapSize); i += BIG_BLOCK_SIZE) {
            auto bb = (BigBlock*)i;
            if (IS_POISONED(bb) or not bb->isAlive()) continue;

            // GC_LOG("[gc] sweep {} - {} - {}", bb, allocToString(bb->type), bigBlockToId(bb));

            size_t aliveCount = bb->markedCount();
            size_t allocatedCount = bb->calculateAllocatedCount();

            if (aliveCount == 0) {
                GC_LOG("[gc] whole block is free {} - {}", allocToString(bb->type), bigBlockToId(bb));
            } else {
                GC_LOG("[gc] {}/{} - {} block is used {} - {}", aliveCount, bb->constantItemCount(), allocatedCount, allocToString(bb->type), bigBlockToId(bb));
            }

            if (aliveCount == 0) { // whole block is free, release it
                freeAndCleanupBlock(bb);
                blocksReclaimed += 1;
                continue;
            }

            if (bb->isFull()) {
                // FIXME
                // setupFreeList(bb);
                // PANIC();
                // freeListsCreated += 1;
                bb->clearMarkBits(false);
            } else {
                // GC_LOG("[gc] block is partially filled {}", bigBlockToId(bb));
                // if bump allocator dead alive ratio is => 0.4 setup free list
                // GC_LOG("[gc] free data {}", bb->constantItemCount());
                bb->clearMarkBits(false);
                assert(bb->calculateMarkedCount() == 0);
            }
        }

        blockReclaimCount += blocksReclaimed;

        GC_LOG("[gc] ### STAT ### => BLOCKS_FREE={} - FREE_LISTS={} - AVIALIABLE_BLOCKS={}, RECLAIMED_TOTAL={}", blocksReclaimed, freeListsCreated, freeBigBlocks, blockReclaimCount);
    }

    void collectRoots(std::vector<uintptr_t>& roots) {
        auto stackEnd = __builtin_stack_address();

        collectRegisterRoots(roots);

        collectStackRoots(stackStart, (uintptr_t*)stackEnd, roots);

        for (auto stat : STATIC_ROOTS) {
            collectPossibleValue((uintptr_t)stat, roots);
        }

        for (auto stat : GLOBALS_TABLE) {
            collectPossibleValue(std::bit_cast<uintptr_t>(stat), roots);
        }
    }

    void doGc() {
        gcCount += 1;

        std::vector<uintptr_t> roots1;

        GC_LOG("[gc] === start ===");
        auto nBigBlock = this->heapSize / Heap::BIG_BLOCK_SIZE;
        GC_LOG("[gc] heapStart: {}, size: {}, bigBlocks: {}, last: {}", (void*)this->start, this->heapSize, nBigBlock, (void*)(this->start + ((nBigBlock-1)*BIG_BLOCK_SIZE)));

        collectRoots(roots1);

        std::unordered_set<void*> roots;
        for (auto root : roots1) roots.insert((void*)root);

        GC_LOG("[gc] roots {} - {}", roots.size(), roots);

        markFromRoots(roots);

        GC_LOG("[gc] END MARKING {} - {}", (size_t)start, (size_t)(start+heapSize));

        sweepGarbage();

        GC_LOG("[gc] === end ===");
    }

#undef GC_LOG
};

struct SimpleAllocation {
    size_t size;
    AllocType type;
    char data[];
};

struct SimpleHeap {
#define GC_LOG(stuff, ...) if (debugGc) println(stuff __VA_OPT__(,) __VA_ARGS__);
    std::unordered_set<SimpleAllocation*> allocated;
    std::unordered_set<SimpleAllocation*> visited;
    size_t allocCounter = 0;
    size_t heapSize = 0;
    void* stackStart = nullptr;
    bool debugGc = false;
    size_t gcCount = 0;

    static constexpr size_t ALLOC_TREASHOLD = 4096*4;
    static constexpr size_t HEAP_SIZE_TRESHOLD = 128*1024*1024;

    ~SimpleHeap() {
        for (auto alloc : allocated) {
            free(alloc);
        }
    }

    void setupPages() {

    }

    void* ptrToUser(SimpleAllocation* ptr) {
      return ((char*)ptr)+sizeof(SimpleAllocation);
    }

    SimpleAllocation* ptrFromUser(void* ptr) {
        if ((uintptr_t)ptr < 4096) return nullptr;
        return (SimpleAllocation*)(((char*)ptr)-sizeof(SimpleAllocation));
    }

    SimpleAllocation* ptrFromUserSafe(void* ptr) {
        auto ptr1 = ptrFromUser(ptr);
        if (not allocated.contains(ptr1)) {
            GC_LOG("invalid ptr {}", ptr1);
            assert(allocated.contains(ptr1));
        }

        return ptr1;
    }

    void* allocate(AllocType type, size_t size) {
        auto allocSize = size+sizeof(SimpleAllocation);

        if (heapSize+allocSize >= HEAP_SIZE_TRESHOLD) {
            doGc();
            if (heapSize+allocSize >= HEAP_SIZE_TRESHOLD) {
                PANIC("OOM heap is full even after GC");
            }
        }

        auto ptr = (SimpleAllocation*)malloc(allocSize);
        heapSize += allocSize;

        std::memset(ptr, 0, allocSize);

        ptr->type = type;
        ptr->size = size;

        allocated.emplace(ptr);

        return ptrToUser(ptr); // convert to user
    }

    void markPtr(SimpleAllocation* ptr) {
        assert(allocated.contains(ptr));
        visited.insert(ptr);
    }

    void markManagedPtr(void* ptr) {
        GC_LOG("[gc] * marking ptr {}", ptr);
        markPtr(ptrFromUserSafe(ptr));
    }

    bool isValidPtr(SimpleAllocation* ptr) {
        return allocated.contains(ptr);
    }

    bool isMarkedPtr(SimpleAllocation* ptr) {
        return visited.contains(ptr);
    }

    bool shoulMark(SimpleAllocation* ptr) {
        if (not isValidPtr(ptr)) return false;
        if (visited.contains(ptr)) return false;
        return true;
    }

    void sweep() {
        std::vector<SimpleAllocation*> toErase;
        for (auto alloc: allocated) {
            if (visited.contains(alloc)) continue;
            GC_LOG("[gc] freeing {} - {} - {}", alloc , allocToString(alloc->type), alloc->size);
            heapSize -= alloc->size+sizeof(SimpleAllocation);
            free(alloc);
            toErase.push_back(alloc);
        }
        for (auto v : toErase) {
            allocated.erase(v);
        }
        visited.clear();
    }

    void collectPossibleValue(size_t value, std::unordered_set<SimpleAllocation*>& roots) {
        if ((value & LoxValue::NAN_MASK) == LoxValue::NAN_MASK) {
            auto ptr = ptrFromUser((void*)(value & LoxValue::DATA_MASK));
            if (isValidPtr(ptr)) {
                roots.insert((SimpleAllocation*)ptr);
            }
        }
        if (isValidPtr((SimpleAllocation*)value)) {
            roots.insert((SimpleAllocation*)value);
        }
        auto usrPtr = ptrFromUser((void*)value);
        if (isValidPtr(usrPtr)) {
            roots.insert((SimpleAllocation*)usrPtr);
        }
    }

    void collectRegisterRoots(std::unordered_set<SimpleAllocation*>& roots) {
#ifdef __x86_64__
        uintptr_t regz[16];


        asm ("mov %%rbx, %0\n"
             "mov %%r12, %1\n"
             "mov %%r13, %2\n"
             "mov %%r14, %3\n"
             "mov %%r15, %4\n"
             "mov %%r8,  %5\n"
             "mov %%r9,  %6\n"
             "mov %%r10, %7\n"
             "mov %%r11, %8\n"
             "mov %%rdi, %9\n"
             "mov %%rsi, %10\n"
             "mov %%rcx, %11\n"
             "mov %%rdx, %12\n"
             "mov %%rax, %13\n"
             "mov %%rsp, %14\n"
             "mov %%rbp, %15\n": "=rm" (regz[0]), "=rm" (regz[1]), "=rm" (regz[2]), "=rm" (regz[3]), "=rm" (regz[4]), "=rm" (regz[5]), "=rm" (regz[6]), "=rm" (regz[7]), "=rm" (regz[8]), "=rm" (regz[9]), "=rm" (regz[10]), "=rm" (regz[11]), "=rm" (regz[12]), "=rm" (regz[13]), "=rm" (regz[14]), "=rm" (regz[15]));
        for (auto ptr : regz) {
            collectPossibleValue(ptr, roots);
        }
#else
#error unsuported architecture TODO arm64
#endif
    }

    __attribute__((no_sanitize("address")))
    void collectStackRoots(std::unordered_set<SimpleAllocation*>& ptrs, void* start, void* end) {
        auto s = (uintptr_t*)std::min(start, end)-4096;
        auto e = (uintptr_t*)std::max(start, end)+4096;

        GC_LOG("[gc] stack scan {} - {}", s, e);

        for (auto i = s; i <= e; i++) {
            collectPossibleValue(*i, ptrs);
        }
    }

    template<typename T>
    void collectManagedPtr(T* ptr) {
        if (ptr == nullptr) return;
        // FIXME not safe bcs iam lazy
        if (shoulMark(ptrFromUser(ptr))) {
            markManagedPtr(ptr);

            collect(ptr);
        }
    }

    void collect(LoxValue* self) {
        collectLox(*self);
    }

    void collect(FunctionRef* self) {
        assertSafePtr(self);
        collectManagedPtr(self->parent);

        auto captures = self->func->totalUpValCount();
        for (auto i = 0ul; i < captures; i++) {
            auto v = self->captures[i];

            if (((uintptr_t)v & LoxValue::NAN_MASK) == LoxValue::NAN_MASK) {
                collectLox(std::bit_cast<LoxValue>(v));
            } else {
                // FIXME this also is not right we should get this inforamtion preciely
                if (v != nullptr) collectManagedPtr(v);
            }
        }
    }

    void collect(ClassRef* self) {
        assertSafePtr(self);
        collectManagedPtr(self->super);

        collectManagedPtr(self->parent);

        collect(&self->methods);
    }

    void collect(LoxStr* self) {
        assertSafePtr(self);
        // do nothing
    }

    void collect(ObjectRef* self) {
        assertSafePtr(self);
        collectManagedPtr(self->clazz);

        collectManagedPtr(self->proto);

        collectManagedPtr(self->construcor);

        collectManagedPtr(self->fields);
    }

    void assertSafePtr(void* ptr) {
        ptrFromUserSafe(ptr);
    }

    void collect(LoxMapBucket* self) {
        assertSafePtr(self);
        forEachBucket((LoxMapBucket*)self, [&](size_t v) {
            if ((v & LoxValue::NAN_MASK) == LoxValue::NAN_MASK) {
                collectLox(std::bit_cast<LoxValue>(v));
            } else {
                collectManagedPtr(std::bit_cast<FunctionRef*>(v));
            }
        });
    }

    void collect(LoxMapBucketArray* self) {
        assertSafePtr(self);
        for (auto i = 0ul; i < self->size; i++) {
            collectManagedPtr(self->buckets[i]);
        }
    }

    void collect(LoxMap* self) {
        collectManagedPtr(self->bucks);
    }

    void collectLox(LoxValue v) {
        switch (v.getType()) {
            case LoxValue::ValueType2::FLOAT:
            case LoxValue::ValueType2::BOOL:
            case LoxValue::ValueType2::BOOL_FALSE:
            case LoxValue::ValueType2::NIL:
                break;
            case LoxValue::ValueType2::CLASS:
                collectManagedPtr(v.asClass());
                break;
            case LoxValue::ValueType2::INSTANCE:
                collectManagedPtr(v.asObject());
                break;
            case LoxValue::ValueType2::FUNCTION_REF:
                collectManagedPtr(v.asFunction());
                break;
            case LoxValue::ValueType2::STRING:
                collectManagedPtr(v.asLoxStr());
                break;
        }
    }

    void idkMarkPtr(SimpleAllocation* ptr) {
        switch (ptr->type) {
            case AllocType::LOX_VALUE:
                collectManagedPtr((LoxValue*)ptrToUser(ptr));
                break;
            case AllocType::HASH_MAP:
                collectManagedPtr((LoxMap*)ptrToUser(ptr));
                break;
            case AllocType::CLASS_REF:
                collectManagedPtr((ClassRef*)ptrToUser(ptr));
                break;
            case AllocType::OBJECT_REF:
                collectManagedPtr((ObjectRef*)ptrToUser(ptr));
                break;
            case AllocType::STRING:
                collectManagedPtr((LoxStr*)ptrToUser(ptr));
                break;
            case AllocType::HASH_MAP_BUCKET:
                collectManagedPtr((LoxMapBucket*) ptrToUser(ptr));
                break;
            case AllocType::FUNCTION_REF:
                collectManagedPtr((FunctionRef*) ptrToUser(ptr));
                break;
            case AllocType::HASH_MAP_BUCKET_ARRAY:
                collectManagedPtr((LoxMapBucketArray*) ptrToUser(ptr));
                break;
        }
    }

    void doGc() {
        gcCount += 1;
        void* stack_marker;
        auto stackEnd = &stack_marker;
        std::unordered_set<SimpleAllocation*> roots;

        collectRegisterRoots(roots);

        collectStackRoots(roots, stackStart, stackEnd);

        for (auto stat : STATIC_ROOTS) {
            collectPossibleValue((size_t)stat, roots);
        }

        for (auto g : GLOBALS_TABLE) {
            collectPossibleValue(std::bit_cast<size_t>(g), roots);
        }

        for (auto root : roots) {
            assert(isValidPtr(root));
            idkMarkPtr(root);
        }

        sweep();
    }
};

Heap heap;
size_t allocTime = 0;
size_t allocCount = 0;
size_t allocAmount = 0;
auto startTime = std::chrono::high_resolution_clock::now();

void* allocate(size_t size, AllocType type) {
    // println("[heap] allocate {} - {}", size, allocToString(type));
#if 0
    return HEAP_ARENA.allocate(size, 8);
#elif 0
    return malloc(size);
#else
    auto start = std::chrono::high_resolution_clock::now();
    auto v = heap.allocate(type, size);
    std::memset(v, 0, size);
    auto end = std::chrono::high_resolution_clock::now();
    allocTime += std::chrono::duration_cast<std::chrono::nanoseconds>(end-start).count();
    allocCount += 1;
    allocAmount += size;
    if (allocCount % 10'000 == 0) {
        auto curTime = std::chrono::high_resolution_clock::now();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(curTime-startTime).count();
        (void)elapsedMs;
        // println("[stats] allocs={}, avg-time={}ns, GCs={}, ALLOC_SPEED={}KB/S", allocCount, allocTime/allocCount, heap.gcCount, ((allocAmount/elapsedMs)*1000)/1024);
    }
    // println("[heap] allocated {} {} {}", v, size, allocToString(type));
    return v;
#endif
}

void assertIsValidPtr(void* ptr) {
    assert(heap.isManagedPtr(ptr));
}

int main(int argc, const char** argv) {
    heap.setupPages();
    void* stackMarker;
    heap.stackStart = &stackMarker;
    bool USE_AST = false;
    string filePath{argv[1]};

    if (argc >= 3) {
        auto argz = string_view{argv[1]};

        DEBUG_JIT = argz.contains("v");
        USE_AST = argz.contains("a");
        heap.debugGc = argz.contains("g");

        filePath = string{argv[2]};
    } else {
        DEBUG_JIT = false;
    }

    auto fajl = readFile(filePath); // prog

    auto src = SourceProvider<TokenType1>(fajl);

    auto tokens = tokenize1(src, getLexingUnits());

    std::cout.setf(ios::fixed);
    std::cout.setf(ios::showpoint);

    if (VERBOSE) {
        for (const auto& tok : *tokens) {
            println("{} {}", tok.content, (long)tok.type);
        }
    }

    MilaState state;
    auto nodes = getMilaParsingUnits();
    MilaParser pepa(*tokens, nodes, state);
    pepa.fileHint = filePath; // __FILE__;
    // pepa.rowOffset = 118;
    // pepa.colOffset = 0;
    pepa.verbose = false;

    auto res = pepa.parse(TokenType1::Semicolon);
    if (not res.has_value()) {
        println("== PERSER ERROR {} REMAINING == ", res.error().toString());
        for (auto t : pepa.remaining()) {
            print("{} ", t.content);
        }
        println("\n== END ==");
        return 3;
    } else {
    }


    auto globalFunc = makeStuff2<Function>("_global");
    std::vector<ASTStm> globalBody;
    for (auto n : pepa.buffer) {
        globalBody.push_back(dynamic_cast<ASTStm>(n));
    }
    globalFunc->data.body = globalBody;

    FIELD_LOOKUP->emplace(CONSTRUCTOR_NAME, 0);


    Linerizer linerizer;
    linerizer.fieldIdLookup = FIELD_LOOKUP;
    linerizer.stack.emplace_back();
    linerizer.stack.back().function = globalFunc/*.get()*/;
    linerizer.stack.back().lexicals.emplace_back();
    linerizer.globals.isGlobal = true;
    auto clk = "clock"s;
    linerizer.globals.enterScope();
    auto clockId = linerizer.globals.putLocal(clk);
    for (auto& node : pepa.buffer) {
        node->visit(linerizer);
    }


    linerizer.stack.back().init(globalFunc);

    std::chrono::time_point start1 = std::chrono::high_resolution_clock::now();

    linerizer.realFix();
    // return 3;

    auto clock = makeStuff2<Function>(clk, vector<string>{}, vector<ASTStm>{});
    clock->runtimeData = (void*)&loxClock; // used by jit
    clock->native = [&](ASTExecutor& ctx) { ctx.push(LoxValue::Number(duration_cast<std::chrono::microseconds>((std::chrono::high_resolution_clock::now()-start1)).count()/1'000'000.0)); }; // used by interp
    auto clkk = builtin::allocateClosure(clock, nullptr);
    GLOBALS_TABLE[0] = clkk;

    if (USE_AST) {
        ASTExecutor executor;

        executor.currentFrame = executor.allocateFunctionRef(*globalFunc);
        executor.stackBase -= globalFunc->locals.size()-globalFunc->totalUpValCount();


        executor.globals[clockId] = clkk;

        auto fRef = builtin::allocateClosure(globalFunc, nullptr);

        // executor.invoke(*globalFunc);
        executor.call(*fRef.asFunction(), {});

        /*size_t ip = 0;
        while (ip < pepa.buffer.size()) {
            pepa.buffer[ip]->visit(executor);
            ip += 1;
        }*/

        return 0;
    }

    Compiler comp;
    comp.begin(globalFunc);

    if (DEBUG_JIT) {
        for (auto& gen : comp.irGens) {
            gen->print();
        }
    }

    for (size_t i = 0; i < comp.funks.size(); i++) {
        std::vector<size_t> argSizes;
        argSizes.push_back(sizeof(FunctionRef*));
        for (size_t j = 0; j < comp.funks[i]->data.argz.size(); j++) {
            argSizes.push_back(sizeof(LoxValue));
        }

        X86MilaAssembler assm(argSizes, sizeof(LoxValue));
        MilaCodeGen ggs(assm, *comp.irGens[i], comp.funks[i]->data.name);

        if (DEBUG_JIT) {
            ggs.printLinearized = true;
            ggs.printLiveRanges = true;
            ggs.warnLeak = true;
        }

        UNWRAPV(ggs.gen());

        auto stackSize = assm.preserveCalleeRegs();

        assm.patchStackSize(align(stackSize, X86MilaAssembler::STACK_ALIGNMENT));

        // UNWRAPV(linkRelative(assm.bytes.data(), assm.spaces, assm.labels));
        assm.linkJumps();

        ggs.assembler.dumpHints(stringify("hints/{}", comp.funks[i]->data.name));

        auto codeSize = assm.bytes.size();
        auto code = cg::allocateJIT(codeSize);

        std::memcpy(code, assm.bytes.data(), codeSize);

        cg::makeRX(code, codeSize);

        comp.funks[i]->runtimeData = code;

        writeBytesToFile(stringify("bins/{}", comp.funks[i]->data.name), assm.bytes);
    }

    assert(comp.funks[0]->runtimeData != nullptr);
    auto funk = reinterpret_cast<GlobalFunck>(comp.funks[0]->runtimeData);

    auto ex1 = std::chrono::high_resolution_clock::now();

    auto funk1 = builtin::allocateClosure(globalFunc, nullptr);

    auto x = funk1.asFunction();
    funk(x);

    auto ex2 = std::chrono::high_resolution_clock::now();

    if (DEBUG_JIT)
        std::cout << "execution took: " << std::chrono::duration_cast<std::chrono::milliseconds>(ex2-ex1).count() << std::endl;
}