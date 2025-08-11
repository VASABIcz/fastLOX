#include <filesystem>
#include <cstring>
#include <cstdlib>
#include <unordered_map>
#include <sanitizer/asan_interface.h>

#include "lexing/Token.h"
#include "lexing/SourceProvider.h"
#include "lexing/lexerExceptions.h"
#include "lexing/lexingUnits.h"
#include "lexing/tokenize.h"
#include "parsing/Parser.h"
#include "codegen/SSARegister.h"
#include "codegen/IRGen.h"
#include "codegen/CodeGen.h"
#include "codegen/IRGenCtx.h"
#include "codegen/x86/X86Assembler.h"
#include "codegen/allocators/SimpleAllocator.h"
#include "codegen/allocators/NewAllocator.h"
#include "codegen/allocators/BetterAllocator.h"
#include "utils/code_gen.h"
#include "utils/BetterArray.h"
#include "utils/pdo_utils.h"

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

volatile u64 PERF_COUNTERS[128];
const char* PERF_NAMES[128];
u64 PERF_TIMES[128];
u64 PERF_START[128];

extern void startPerf(size_t id) {
    PERF_START[id] = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

extern void endPerf(size_t id) {
    PERF_TIMES[id] += (u64)(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count()-PERF_START[id]);
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
    void* nativeConstructor;
    size_t paramCount;
    size_t hasConstructor;

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

#if 0
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

bool writeMap(LoxMap* map, size_t id, size_t value) {
    bool didCreate = false;
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
        didCreate = true;
        bucket = allocBucket();
        // println("MAP ADDR IZZZZZZZZZZZZZZ {}", map);
        assertIsValidPtr(map->bucks);
        assert(map->bucks != nullptr);
        map->bucks->buckets[id % map->bucks->size] = bucket;
    } else if (bucket->size == LoxMap::BUCKET_SIZE) {
        didCreate = true;
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
            return didCreate;
        }
    }
    assert(bucket->size < LoxMap::BUCKET_SIZE);

    bucket->items[bucket->size++] = {(u32)id, value};

    return true;
}
#else

// TODO sorted + separate key/value arrays better cache locality?
struct KP {
    size_t key,value;
};

struct InlineArray {
    size_t size;
    size_t capacity;
    KP data[];
};

struct  LoxMapBucket {};

struct LoxMapBucketArray {
    size_t size;
    LoxMapBucket** buckets;
};

struct LoxMap {
    union {
        LoxMapBucketArray* bucks;
        InlineArray* inner;
    };

    static constexpr size_t INVALID_VALUE = 999999999;
    static constexpr size_t BBUCKET_SIZE = 64;
    static constexpr size_t BUCKET_ARRAY_BASE_SIZE = 16;
};

size_t readMap(LoxMap* map, size_t id, size_t* offset = nullptr) {
    auto m = map->inner;
    if (m == nullptr) return LoxMap::INVALID_VALUE;

    for (auto i = 0UL; i < m->size; i++) {
        auto v = m->data[i];
        if (v.key == id) {
            if (offset != nullptr) *offset = offsetof(InlineArray, data[i].value);
            return v.value;
        }
    }
    return LoxMap::INVALID_VALUE;
}

bool writeMap(LoxMap* map, size_t id, size_t value, bool* didInvalidate = nullptr, size_t* offset1 = nullptr) {
    InlineArray*& m = map->inner;
    if (m == nullptr) {
        m = (InlineArray*)malloc(sizeof(InlineArray)+8*sizeof(KP));
        m->size = 0;
        m->capacity = 8;
        if (didInvalidate != nullptr) *didInvalidate = true;
    }

    size_t offset;
    auto res = readMap(map, id, &offset);
    if (res != LoxMap::INVALID_VALUE) {
        *(size_t*)((char*)m+offset) = value;
        if (offset1 != nullptr) *offset1 = offset;
        return false;
    }

    if (m->size == m->capacity) {
        auto newCap = sizeof(InlineArray)+sizeof(KP)*(m->capacity*3/2);
        auto newArr = (InlineArray*)malloc(newCap);
        m->capacity = m->capacity*3/2;
        std::memcpy(newArr, m, m->size*sizeof(KP)+sizeof(InlineArray));
        free(m);
        m = newArr;
        if (didInvalidate != nullptr) *didInvalidate = true;
    }

    m->data[m->size++] = {id, value};

    return true;
}

size_t atOffset(LoxMap* map, size_t offset) {
    return map->inner->data[offset].value;
}

template<typename FN>
void forEachBucketArray(LoxMapBucketArray* map, FN&& fn) {

}

template<typename FN>
void forEachBucket(LoxMapBucket* bucket, FN&& fn) {

}

#endif

struct ClassRef;

struct Shape {
    Shape* parent;
    ClassRef* base;
    u32 modification;
    Shape* children[8]{};

    Shape* addField(u32 v) {
        for (auto& child : children) {
            if (child == nullptr) {
                child = new Shape(this, nullptr, v);
                return child;
            }
            if (child->modification == v) {
                return child;
            }
        }
        return new Shape();
    };

    static Shape* makeShape(ClassRef* base) {
        return new Shape(nullptr, base, 0);
    }
};

struct ClassRef {
    Class* clazz;
    ClassRef* super;
    FunctionRef* parent;
    Shape* baseShape;
    LoxMap methods;

    Function* getConstructor() {
        auto res = clazz->getMethod(CONSTRUCTOR_ID);

        if (res != nullptr)  return res;

        if (super != nullptr) return super->getConstructor();;

        return nullptr;
    }
};

// static_assert(offsetof(FunctionRef, argCount) == offsetof(ClassRef, argCount));
// static_assert(offsetof(FunctionRef, fPtr) == offsetof(ClassRef, fPtr));

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
        // these 2 are comparable and addable
        FLOAT =        0x0000000000000000, // 0
        STRING =       0x0001000000000000, // 1
        INSTANCE =     0x0002000000000000, // 2
        // this one differs from BOOL_FALSE in 1 bit
        BOOL =         0x0003000000000000, // 3
        // these 2 are callable
        FUNCTION_REF = 0x8000000000000000, // 4
        CLASS =        0x8001000000000000, // 5
        // these 2 types are falsy values
        NIL =          0x8002000000000000, // 6
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

auto FIELD_LOOKUP = new std::unordered_map<std::string_view, u32>();

std::string_view idToName(u32 id) {
    for (auto& xd : *FIELD_LOOKUP) {
        if (xd.second == id) return xd.first;
    }
    PANIC();
}

FunctionRef* createMethod(Function* f1, FunctionRef* parent, ObjectRef* self);

struct ObjectRef {
    LoxMap fields;
    ClassRef* clazz;
    ObjectRef* proto;
    FunctionRef* construcor;
    LoxValue proto1;
    Shape* shape;
    ObjectRef* inheritor;

    template<typename FN>
    void forEachFrien(FN&& f) {
        f(this);
        auto next = proto;
        while (next != nullptr) {
            f(next);
            next = next->proto;
        }
        next = inheritor;
        while (next != nullptr) {
            f(next);
            next = next->inheritor;
        }
    }

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

    size_t readFields(u32 name, size_t* offset = nullptr) {
        // if (fields == nullptr) return LoxMap::INVALID_VALUE;
        auto value = readMap(&fields, name, offset);
        return value;
    }

    LoxValue read(u32 name) {
        auto r = readFields(name);
        if (r != LoxMap::INVALID_VALUE) return std::bit_cast<LoxValue>(r);
        return getMethod2(name);
    }

    void shapeAdd(u32 v) {
        shape = shape->addField(v);
    }

    void shapeAddPropagate(u32 v) {
        forEachFrien([&](auto frien) {
            // println("adding shape to frien {}", frien);
            frien->shapeAdd(v);
        });
    }

    void mapPropagate(LoxMap m) {
        forEachFrien([&](auto frien) {
            frien->fields = m;
        });
    }

    void write(u32 name, LoxValue v, size_t* offset1 = nullptr) {
/*        if (fields == nullptr) {
            auto map = allocateTypedSimple<LoxMap>(AllocType::HASH_MAP);
            map->bucks = nullptr;
            fields = map;
        }*/

        bool didInvalidate = false;
        size_t offset;
        auto didCreate = writeMap(&fields, name, std::bit_cast<size_t>(v), &didInvalidate, &offset);
        assert(readMap(&fields, name) == std::bit_cast<size_t>(v));
        if (didInvalidate) mapPropagate(fields);
        if (didCreate) shapeAddPropagate(name);
        if (offset1 != nullptr) *offset1 = LoxMap::INVALID_VALUE;
        if (!didInvalidate && !didCreate && offset1 != nullptr) *offset1 = offset;
    }

    // FIXME this can be optimized a bit, we can just memcpy the cached FunctionRef? + remove recursive search
    LoxValue getMethod2(u32 name) {
        auto m = clazz->clazz->getMethod(name);
        if (m == nullptr && proto != nullptr) {
            return proto->getMethod2(name);
        }
        if (m == nullptr) {
            println("method {} not found on object {}", idToName(name), this->clazz->clazz->data.name);
            PANIC();
        }

        return LoxValue::Function(createMethod(m, clazz->parent, this));
    }
};

Shape* getSlowShape() {
    return nullptr;
}

struct MethodCallIC {
    std::pair<Shape*, FunctionRef*> methodEntry; // pointer to fRef bcs its constant with shape
    std::pair<Shape*, std::pair<u32, u32>> functionEntry; // upper - bucket id, lower - index in bucket
};

struct ReadFieldIC {
    std::pair<Shape*, size_t> fieldEntry; // offset in loxMap
    std::pair<Shape*, size_t> methodEntry;
};

struct WriteFieldIC {
    std::pair<Shape*, size_t> fieldEntry; // offset in loxMap
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

    LoxValue getMethodIC2(LoxValue obj, u32 id, ReadFieldIC* ic) {
        assert(obj.isObject());
        auto o = obj.asObject();

        if (ic->methodEntry.first == o->shape) {
            // std::bit_cast<FunctionRef*>(atOffset(&o->clazz->methods, ic->methodEntry.second));
            TODO("do copy of function here");
        }

        // TODO("modify getMethod2 to return offset");
        auto m = o->getMethod2(id);
        // ic->methodEntry = {o->shape, 33};
        return m;
    }

    LoxValue readFieldIC(LoxValue obj, u32 id, ReadFieldIC* ic) {
        assert(obj.isObject());
        auto o = obj.asObject();
        size_t offset;
        auto r = o->readFields(id, &offset);
        if (r != LoxMap::INVALID_VALUE) {
            ic->fieldEntry = {o->shape, offset};
            return std::bit_cast<LoxValue>(r);
        }
        return getMethodIC2(obj, id, ic);
    }

    void writeFieldIC(LoxValue obj, LoxValue value, u32 id, WriteFieldIC* ic) {
        assert(obj.isObject());
        size_t offset;
        obj.asObject()->write(id, value, &offset);
        if (offset != LoxMap::INVALID_VALUE) {
            ic->fieldEntry = {obj.asObject()->shape, offset};
        }
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

    LoxValue doConcat(LoxValue lhs, LoxValue rhs) {
        assert(lhs.isString());
        assert(rhs.isString());
        auto newString = allocateEmptyLoxString(lhs.asString().size() + rhs.asString().size());
        std::memcpy(newString->cString, lhs.asString().data(), lhs.asString().size());
        std::memcpy(newString->cString+lhs.asString().size(), rhs.asString().data(), rhs.asString().size());
        return LoxValue::String(newString);
    }

    LoxValue cmpString(LoxValue lhs, LoxValue rhs) {
        assert(lhs.isString());
        assert(rhs.isString());
        return LoxValue::Bool(rhs.asString() == lhs.asString());
    }

    LoxValue cmpStringN(LoxValue lhs, LoxValue rhs) {
        assert(lhs.isString());
        assert(rhs.isString());
        return LoxValue::Bool(rhs.asString() != lhs.asString());
    }

    LoxValue eqNumber(LoxValue lhs, LoxValue rhs) {
        return LoxValue::Bool(rhs.asNumber() == lhs.asNumber());
    }

    LoxValue eqRef(LoxValue lhs, LoxValue rhs) {
        return LoxValue::Bool(rhs.internal == lhs.internal);
    }

    LoxValue doAdd(LoxValue lhs, LoxValue rhs) {
        if (rhs.isString()) {
            assert(lhs.isString());
            assert(rhs.isString());
            return doConcat(lhs, rhs);
        } else {
            assert(rhs.isNumber());
            assert(lhs.isNumber());
            return LoxValue::Number(lhs.asNumber() + rhs.asNumber());
        }
    }

    LoxValue doEq(LoxValue lhs, LoxValue rhs) {
        if (lhs.isNumber() && lhs.isNumber()) { // needed for nan equality
            return eqNumber(lhs, rhs);
        } else if (rhs.isString() && lhs.isString()) { // "structural equality"
            return cmpString(lhs, rhs);
        } else { // referential equality
            return eqRef(lhs, rhs);
        }

        if (rhs.isNumber()) { // needed for nan equality
            return eqNumber(lhs, rhs);
        } else if (rhs.isString() && lhs.isString()) { // "structural equality"
            return cmpString(lhs, rhs);
        } else { // referential equality
            return eqRef(lhs, rhs);
        }
    }

    LoxValue doNeq(LoxValue lhs, LoxValue rhs) {
        return LoxValue::Bool(!doEq(lhs, rhs).toBool());
    }

    LoxValue doSimpleBin(BinaryType type, LoxValue lhs, LoxValue rhs) {
    // if (lhs.asNumber() < 0.0) PANIC();
    // if (lhs.asNumber() > 1'000'000.0) PANIC();
    // std::cout << lhs.toString() << " " << binarToString(type) << " " << rhs.toString() << std::endl;
    LoxValue res;
    switch (type) {
        case BinaryType::ADD:
            res = doAdd(lhs, rhs);
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
            res = doEq(lhs, rhs);
            break;
        case BinaryType::NEQ:
            res = doNeq(lhs, rhs);
            break;
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
        if (proto != nullptr) proto->inheritor = me;
        me->inheritor = nullptr;
        me->clazz = clazz;
        me->proto = proto;
        me->proto1 = proto == nullptr ? LoxValue::Nil() : LoxValue::Object(proto);
        me->fields = *data;
        me->construcor = nullptr;
        me->shape = clazz->baseShape;
        if (clazz->getConstructor() != nullptr) me->construcor = me->getRawMethod(CONSTRUCTOR_ID, false);
        return me;
    }

    ObjectRef* rawInstant(ClassRef* clazz) {
        LoxMap* data = nullptr;

        ObjectRef* proto = nullptr;
        if (clazz->super != nullptr) {
            data = allocateTypedSimple<LoxMap>(AllocType::HASH_MAP);
            data->bucks = nullptr;
            proto = rawInstant(clazz->super, data);
        }
        auto me = allocateTypedSimple<ObjectRef>(AllocType::OBJECT_REF);
        if (proto != nullptr) proto->inheritor = me;
        me->inheritor = nullptr;
        me->clazz = clazz;
        me->proto = proto;
        me->proto1 = proto == nullptr ? LoxValue::Nil() : LoxValue::Object(proto);
        me->fields.bucks = nullptr;
        me->construcor = nullptr;
        me->shape = clazz->baseShape;
        if (clazz->getConstructor() != nullptr) me->construcor = me->getRawMethod(CONSTRUCTOR_ID, false);
        return me;
    }

    LoxValue instantiate(LoxValue clazz) {
        auto* res = rawInstant(clazz.asClass());

        return LoxValue::Object(res);
    }

    FunctionRef* getMethod(LoxValue obj, u32 id, size_t argCount) {
        // println("getMethod {} {}@{} {}", obj.toString(), idToName(id), id, argCount);
        assert(obj.isObject());
        auto self = obj.asObject();
        auto r = self->readFields(id);
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


    FunctionRef* getMethodIC(LoxValue obj, u32 id, size_t argCount, MethodCallIC* ic) {
        assert((uintptr_t)ic % 8 == 0);
        assert(obj.isObject());
        auto self = obj.asObject();
        auto r = self->readFields(id);

        if (r != LoxMap::INVALID_VALUE) {
            auto m = std::bit_cast<LoxValue>(r);
            assert(m.isFunction());
            // permanently destroy IC fast path
            ic->methodEntry = {getSlowShape(), nullptr};
            return m.asFunction();
        }

        auto m = self->getRawMethod(id, true);
        assert(m != nullptr);
        assert(m->argCount == argCount);
        ic->methodEntry = {self->shape, m};

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

        // std::memset(c->captures, 0x0, f->totalUpValCount()*sizeof(LoxValue*));

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
        claz->baseShape = Shape::makeShape(claz);

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
    virtual void getPtr(size_t dst, size_t value) {
        movUnsigned(dst, LoxValue::DATA_MASK);
        andInt(dst, dst, value);
    }

    void cJmp(Label l, JumpCondType t, size_t lhs, size_t rhs) {
        jmpCond(l.id, t, lhs, rhs);
    }

    void cJmp(Label l) {
        this->jmp(l.id);
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

    virtual void allocateObject(size_t tgt, size_t subj, span<size_t> argz) = 0;

    virtual void allocateObjectTyped(size_t tgt, Class* subj, size_t self, span<size_t> argz) = 0;

    virtual void functionCall(size_t tgt, size_t subj, span<size_t> argz, bool doParamCheck) = 0;

    virtual void print1(size_t arg) = 0;

    virtual void toBool(size_t tgt, size_t src) = 0;

    virtual void numberGuard(size_t tgt, size_t src) = 0;

    virtual void readGlobal(size_t tgt, size_t id) {
        movPtr(tgt, &GLOBALS_TABLE);
        readMem(tgt, tgt, id*sizeof(LoxValue), sizeof(LoxValue));
    }

    virtual void writeGlobal(size_t id, size_t value) {
        // FIXME this can be direct read without adend?
        auto gReg = movImmPtrToReg(&GLOBALS_TABLE);
        writeMem(gReg, value, id*sizeof(LoxValue), sizeof(LoxValue));

        freeRegister(gReg);
    }

    virtual void loxBool(size_t branchT, size_t branchF, size_t subj, const std::function<void()>& genTrue, const std::function<void()>& genFalse) = 0;
};

// constexpr size_t START_PERF = __COUNTER__;

struct PerfEntry {
    size_t counter;
    size_t start;
    size_t time;
};

static_assert(std::bit_width(LoxValue::DATA_MASK) == 48);

struct X86MilaAssembler: virtual MilaAssembler, X86Assembler {
#if 0
#define MAKE_PERF() auto __id = __COUNTER__-START_PERF; /*putPerf(__id);*/ PERF_NAMES[__id] = __FUNCTION__;
#define MAKE_PERF1(name) auto __id = __COUNTER__-START_PERF; /*putPerf(__id);*/ PERF_NAMES[__id] = name;
#define DO_PERF(name) auto __id = __COUNTER__-START_PERF; /*putPerf(__id);*/ PERF_NAMES[__id] = name; putPerfStart2(__id); Cleanup{[this,__id]{ putPerfEnd2(__id); }};
#define PERF() DO_PERF(__FUNCTION__)
#define PERF_START() putPerfStart2(__id);
#define PERF_STOP() putPerfEnd2(__id);
#else
#define MAKE_PERF()
#define MAKE_PERF1(name)
#define DO_PERF(name)
#define PERF() DO_PERF(__FUNCTION__)
#define PERF_START()
#define PERF_STOP()
#endif
#define READ_IC(dst, id, A, B) readIC(dst, id, offsetof(A, B))

    std::map<size_t, std::string> hints;

    size_t HINT_ID = 0;
    size_t PERF_ID = 0;
    size_t PERF_LABEL_ID = 0;
    size_t IC_ID = 0;

    Label crashLabel = makeLabel1();

    X86MilaAssembler(span<size_t> args, size_t ret): X86Assembler(args, ret) {
        HINT_ID = this->allocateLabelType();
        PERF_ID = this->allocateLabelType();
        IC_ID = this->allocateLabelType();
        PERF_LABEL_ID = allocateLabel();
    }

    void putPerfStart2(size_t id) {
        mc.push(x86::Rax);
        mc.push(x86::Rdx);
        mc.push(x86::Rcx);

        // edi:eax = ticks
        mc.rdtsc();

        // rdx = edi:eax
        mc.shiftLImm(x86::Rdx, 32);
        mc.writeRegInst(X64Instruction::Or, x86::Rdx, x86::Rax);

        // set start time
        auto space = mc.writeRipRegInst(X64Instruction::mov, 0, x86::Rdx);
        requestLabelRel4(PERF_LABEL_ID, space, PERF_ID, id*sizeof(PerfEntry)+offsetof(PerfEntry, start));

        // increment counter
        space = mc.writeRegRipInst(X64Instruction::inc, x86::Zero, 0);
        requestLabelRel4(PERF_LABEL_ID, space, PERF_ID, id*sizeof(PerfEntry)+offsetof(PerfEntry, counter));

        mc.pop(x86::Rcx);
        mc.pop(x86::Rdx);
        mc.pop(x86::Rax);
    }

    size_t getPerfOffset() {
        return this->getBoundLabelById(PERF_LABEL_ID).offset;
    }

    void loxBool(size_t branchT, size_t branchF, size_t subj, const std::function<void()>& genTrue, const std::function<void()>& genFalse) override {
        auto label = makeLabel1();

        auto ctx = this->getAllocCtx();
        auto tmp = ctx.allocReg();

        this->getTagFast(tmp, subj, 1);
        checkTagFast(tmp, LoxValue::ValueType2::NIL, label, JumpCondType::EQUALS, 1);

        genTrue();
        jmp(branchT);
        bind(label);
        genFalse();
        jmp(branchF);

        ctx.restore();
    }

    void linkPerf() {
        bindRawLabel(PERF_LABEL_ID, PERF_ID);

        for (auto i = 0ul; i < 8ul-(bytes.size() % 8); i++) {
            mc.pushBack(0);
        }

        for (auto i = 0; i < 8*100; i++) {
            mc.pushBack(0);
        }

        this->simpleLink(PERF_ID);
    }

    void putPerfEnd2(size_t id) {
        mc.push(x86::Rax);
        mc.push(x86::Rdx);

        // edi:eax = ticks
        mc.rdtsc();

        // rdx = edi:eax
        mc.shiftLImm(x86::Rdx, 32);
        mc.writeRegInst(X64Instruction::Or, x86::Rdx, x86::Rax);

        // rdx -= PERF_START[id]
        auto space = mc.writeRegRipInst(X64Instruction::sub, x86::Rdx, 0);
        requestLabelRel4(PERF_LABEL_ID, space, PERF_ID, id*sizeof(PerfEntry)+offsetof(PerfEntry, start));

        // PERF_TIME[id] += rdx
        space = mc.writeRipRegInst(X64Instruction::add, 0, x86::Rdx);
        requestLabelRel4(PERF_LABEL_ID, space, PERF_ID, id*sizeof(PerfEntry)+offsetof(PerfEntry, time));

        mc.pop(x86::Rdx);
        mc.pop(x86::Rax);
    }

    // efficient 0 jmp, 0 imm64, 1 tmp guard for non Number values
    void ptrTagGuard(size_t subject, LoxValue::ValueType2 tag, Label fail) {
        mc.shiftRImm(allocator.getReg(subject), 48);
        mc.cmpImm(allocator.getReg(subject), (((uint64_t)tag|LoxValue::NAN_MASK) >> std::bit_width(LoxValue::DATA_MASK)));
        cJmp1(fail, JumpCondType::NOT_EQUALS);
    }

    void getTagFast(size_t tgt, size_t subject, int adend = 0) {
        movReg(tgt, subject, 0, 0, 8);
        mc.shiftRImm(allocator.getReg(tgt), std::bit_width(LoxValue::DATA_MASK)+adend);
    }

    void checkTagFast(size_t subject, LoxValue::ValueType2 tag, Label fail) {
        mc.cmpImm(allocator.getReg(subject), ((uint64_t)tag|LoxValue::NAN_MASK) >> std::bit_width(LoxValue::DATA_MASK));
        cJmp2(fail, JumpCondType::NOT_EQUALS);
    }

    void checkTagFast(size_t subject, LoxValue::ValueType2 tag, Label fail, JumpCondType type, int adend = 0) {
        mc.cmpImm(allocator.getReg(subject), ((uint64_t)tag|LoxValue::NAN_MASK) >> (std::bit_width(LoxValue::DATA_MASK)+adend));
        cJmp2(fail, type);
    }

    void putPerfStart(size_t id) {
        mc.push(x86::Rax);
        mc.push(x86::Rdx);
        mc.push(x86::Rcx);

        // edi:eax = ticks
        mc.rdtsc();

        // rdx = edi:eax
        mc.shiftLImm(x86::Rdx, 32);
        mc.writeRegInst(X64Instruction::Or, x86::Rdx, x86::Rax);

        mc.mov(x86::Rax, (size_t)&PERF_START[id]);
        mc.writeMem(x86::Rax, x86::Rdx, 0, 8);

        // increment counter
        mc.mov(x86::Rax, (size_t)(&PERF_COUNTERS[id]));
        mc.writeRegMemInst(X64Instruction::inc, x86::Zero, x86::Rax, 0);

        mc.pop(x86::Rcx);
        mc.pop(x86::Rdx);
        mc.pop(x86::Rax);
    }

    void putPerfEnd(size_t id) {
        mc.push(x86::Rax);
        mc.push(x86::Rdx);
        mc.push(x86::Rcx);

        // edi:eax = ticks
        mc.rdtsc();

        // rdx = edi:eax
        mc.shiftLImm(x86::Rdx, 32);
        mc.writeRegInst(X64Instruction::Or, x86::Rdx, x86::Rax);

        // rax = &PERF_START[id]
        mc.mov(x86::Rax, (size_t)&PERF_START[id]);

        // rdx = rdx - *rax
        mc.writeRegMemInst(X64Instruction::sub, x86::Rdx, x86::Rax, 0);

        // rax = &PERF_TIMES[id]
        mc.mov(x86::Rax, (size_t)&PERF_TIMES[id]);

        // *rax = rdx
        mc.writeMemRegInst(X64Instruction::add, x86::Rax, 0, x86::Rdx);

        mc.pop(x86::Rcx);
        mc.pop(x86::Rdx);
        mc.pop(x86::Rax);
    }

    void beSpetial() {
        MAKE_PERF1("valid");
        for (auto i = 0; i < 1000; i++) {
            PERF_START();
            PERF_STOP();
        }
    }

    void bindHint(std::string_view h) override {
        auto id = allocateLabel();
        this->bindRawLabel(id, HINT_ID);
        hints[id] = std::string(h);
    }

    void beforeInstruction() override {
        // PERF();
    }

    void dumpHints(std::string_view s) override {
        std::ofstream idk1{std::string(s)};

        idk1 << hints.size() << std::endl;
        for (auto &hint: hints) {
            idk1 << getBoundLabelById(hint.first).offset << std::endl;
            idk1 << escape(hint.second) << std::endl;
        }

        idk1.close();
    }

    void readField(size_t tgt, size_t self, u32 id) override {
        PERF()
        auto ctx = this->getAllocCtx();
        tgt = ctx.ensureRegWriteback(tgt);
        self = ctx.ensureReg(self);
        auto tmp = ctx.allocReg();
        readFieldIC(tgt, tmp, self, id);
        ctx.restore();
        // callBuiltin(builtin::readField, {handleToArg(self), Arg::Imm(id)}, handleToArg(tgt));
    }

    void readFieldIC(size_t tgt, size_t tmp, size_t subj, u32 id) {
        getTagFast(tgt, subj);
        checkTagFast(tgt, LoxValue::ValueType2::INSTANCE, crashLabel);

        auto fastPath = makeLabel1();
        auto done = makeLabel1();

        auto icId = allocateIC<ReadFieldIC>();

        getPtr(tmp, subj);
        READ_IC(allocator.getReg(tgt), icId, ReadFieldIC, fieldEntry.first);
        mc.writeMemRegInst(X64Instruction::cmp, allocator.getReg(tmp), offsetof(ObjectRef, shape), allocator.getReg(tgt));
        cJmp2(fastPath, JumpCondType::EQUALS);

        // slow path
        callBuiltin(builtin::readFieldIC, {handleToArg(subj), Arg::Imm(id), icToArg(icId)}, handleToArg(tgt));
        cJmp(done);

        bind(fastPath);

        READ_IC(allocator.getReg(tmp), icId, ReadFieldIC, fieldEntry.second); // offset in tmp
        getPtr(tgt, subj); // objref in tgt
        mc.writeRegMemInst(X64Instruction::mov, allocator.getReg(tgt), allocator.getReg(tgt), offsetof(ObjectRef, fields)); // deref fields
        mc.writeRegInst(X64Instruction::add, allocator.getReg(tgt), allocator.getReg(tmp)); // add offset
        mc.writeRegMemInst(X64Instruction::mov, allocator.getReg(tgt), allocator.getReg(tgt), 0); // read value

        bind(done);
    }

    void writeField(size_t self, u32 id, size_t value) override {
        PERF()
        // callBuiltin(builtin::writeField, {handleToArg(self), handleToArg(value), Arg::Imm(id)}, {});
        auto ctx = this->getAllocCtx();
        self = ctx.ensureReg(self);
        value = ctx.ensureReg(value);
        auto tmp = ctx.allocReg();
        auto tmp1 = ctx.allocReg();
        writeFieldIC(tmp, tmp1, self, value, id);
        ctx.restore();
    }

    void writeFieldIC(size_t tmp, size_t tmp1, size_t subj, size_t value, size_t id) {
        getTagFast(tmp, subj);
        checkTagFast(tmp, LoxValue::ValueType2::INSTANCE, crashLabel);

        auto fastPath = makeLabel1();
        auto done = makeLabel1();

        auto icId = allocateIC<WriteFieldIC>();

        getPtr(tmp, subj);
        READ_IC(allocator.getReg(tmp1), icId, WriteFieldIC, fieldEntry.first);
        mc.writeMemRegInst(X64Instruction::cmp, allocator.getReg(tmp), offsetof(ObjectRef, shape), allocator.getReg(tmp1));
        cJmp2(fastPath, JumpCondType::EQUALS);

        // slow path
        callBuiltin(builtin::writeFieldIC, {handleToArg(subj), handleToArg(value), Arg::Imm(id), icToArg(icId)}, {});
        cJmp(done);

        bind(fastPath);

        READ_IC(allocator.getReg(tmp1), icId, WriteFieldIC, fieldEntry.second);
        mc.writeRegMemInst(X64Instruction::mov, allocator.getReg(tmp), allocator.getReg(tmp), offsetof(ObjectRef, fields)); // deref fields
        mc.writeRegInst(X64Instruction::add, allocator.getReg(tmp), allocator.getReg(tmp1)); // add offset
        mc.writeMemRegInst(X64Instruction::mov, allocator.getReg(tmp), 0, allocator.getReg(value)); // read value

        bind(done);
    }

    void writeJ(CmpType t, size_t id) {
        if (false && id == crashLabel.id) { // used for debugging
            auto skip = makeLabel1();
            this->writeJmp(negateCmp(t), skip.id);

            mc.hlt();

            bind(skip);
        } else {
            this->writeJmp(t, id);
        }
    }

    void cJmp1(Label l, JumpCondType t) {
        writeJ(toCmpType(t), l.id);
    }

    void cJmp2(Label l, JumpCondType t) {
        writeJ(toCmpType2(t), l.id);
    }

    void print1(size_t arg) override {
        // trap();
        callBuiltin(builtin::loxPrint, {handleToArg(arg)}, {});
    }

    void numberGuard(RegAllocCtx& alloc, size_t subjReg, size_t tmp1, Label crashLabel) {
        // movUnsigned(tmp1, (LoxValue::NAN_MASK << 1));
        movReg(tmp1, subjReg);
        mc.shiftRImm(allocator.getReg(tmp1), 32-1); // truncate to 32 bits + discard IEE sign bit
        mc.cmpImm(allocator.getReg(tmp1), (i32)(LoxValue::NAN_MASK >> (32-1))); // 32bit instruction to discard upper 32 bits
        cJmp2(crashLabel, JumpCondType::GREATER);
    }

    void fastArith(size_t dst, size_t lhs, size_t rhs, BinaryOp op, Label crashLabel) {
        auto ctx = getAllocCtx();
        auto lReg = ctx.ensureReg(lhs);
        auto rReg = ctx.ensureReg(rhs);
        auto dstReg = ctx.ensureRegWriteback(dst);
        auto tmpReg = (dstReg == lReg or dstReg == rReg) ? ctx.allocReg() : dstReg;

        numberGuard(ctx, lReg, tmpReg, crashLabel);
        numberGuard(ctx, rReg, tmpReg, crashLabel);

        this->arithmeticFloat(op, FloatingPointType::Double, dstReg, lReg, rReg);

        ctx.restore();
    }

    void fastCmp(size_t dst, size_t lhs, size_t rhs, JumpCondType type, Label crashLabel) {
        auto ctx = getAllocCtx();
        auto lReg = ctx.ensureReg(lhs);
        auto rReg = ctx.ensureReg(rhs);
        auto dstReg = ctx.ensureRegWriteback(dst);
        auto tmpReg = (dstReg == lReg or dstReg == rReg) ? ctx.allocReg() : dstReg;

        auto isDoneLabel = makeLabel1();

        numberGuard(ctx, lReg, tmpReg, crashLabel);
        numberGuard(ctx, rReg, tmpReg, crashLabel);

        mc.movq(0, allocator.getReg(lReg), true);
        mc.movq(1, allocator.getReg(rReg), true);
        mc.comisd(0, 1, true);
        movUnsigned(dstReg, std::bit_cast<u64>(LoxValue::True()));
        cJmp2(isDoneLabel, type);
        movUnsigned(dstReg, std::bit_cast<u64>(LoxValue::False()));

        bind(isDoneLabel);

        ctx.restore();
    }

    template<typename T, typename... Args>
    void callBuiltin(T(*fuk)(Args...), BetterArray<Arg, sizeof...(Args)> argz, std::optional<Arg> ret) {
        chadCall(Arg::FunPtr(fuk), argz, ret);
    }

    void allocateObjectTyped(size_t tgt, Class *subj, size_t self, span<size_t> argz) override {
        callBuiltin(builtin::instantiate, {handleToArg(self)}, handleToArg(tgt));
    }

    void fastAdd(size_t dst, size_t lhs, size_t rhs, Label crashLabel) {
        auto notNumber = makeLabel1();
        auto done = makeLabel1();

        auto ADEND = -17; // shift LoxValue << (48-17) -- discarding sign bit

        bindHint("add numbers");
        getTagFast(dst, lhs, ADEND);
        checkTagFast(dst, LoxValue::ValueType2::FLOAT, notNumber, JumpCondType::GREATER, ADEND);
        getTagFast(dst, rhs, ADEND);
        checkTagFast(dst, LoxValue::ValueType2::FLOAT, crashLabel, JumpCondType::GREATER, ADEND);
        mc.addFloat(allocator.getReg(dst), allocator.getReg(lhs), allocator.getReg(rhs), true);
        cJmp(done);

        bindHint("add strings");
        bind(notNumber);
        getTagFast(dst, lhs);
        checkTagFast(dst, LoxValue::ValueType2::STRING, crashLabel, JumpCondType::NOT_EQUALS);
        getTagFast(dst, rhs);
        checkTagFast(dst, LoxValue::ValueType2::STRING, crashLabel, JumpCondType::NOT_EQUALS);
        callBuiltin(builtin::doConcat, {handleToArg(lhs), handleToArg(rhs)}, handleToArg(dst));

        bind(done);
    }

    void movLox(size_t dst, LoxValue v) {
        movUnsigned(dst, std::bit_cast<uint64_t>(v));
    }

    void fastEq(size_t dst, size_t lhs, size_t rhs, Label crashLabel, bool isNeq) {
        auto notNumber = makeLabel1();
        auto notString = makeLabel1();
        auto doFalse = makeLabel1();
        auto done = makeLabel1();

        auto ADEND = -17; // shift LoxValue << (48-17) -- discarding sign bit

        auto tru = isNeq ? LoxValue::False() :LoxValue::True();
        auto fal = isNeq ? LoxValue::True() : LoxValue::False();
        auto sCmp = isNeq ? builtin::cmpStringN : builtin::cmpString;

        auto normalCmp = JumpCondType::NOT_EQUALS;
        auto floatCmp = CmpType::NotEqual;
        auto floatParity = CmpType::Parity;

        bindHint("number equality");
        getTagFast(dst, lhs, ADEND);
        checkTagFast(dst, LoxValue::ValueType2::FLOAT, notNumber, JumpCondType::GREATER, ADEND);
        mc.movq(0, allocator.getReg(lhs), true);
        mc.movq(1, allocator.getReg(rhs), true);
        mc.ucomisd(0, 1, true);
        movLox(dst, tru);
        writeJ(floatParity, doFalse.id);
        writeJ(floatCmp, doFalse.id);
        cJmp(done);

        bindHint("\"structural equality\"");
        bind(notNumber);
        getTagFast(dst, lhs);
        checkTagFast(dst, LoxValue::ValueType2::STRING, notString, JumpCondType::NOT_EQUALS);
        getTagFast(dst, rhs);
        checkTagFast(dst, LoxValue::ValueType2::STRING, notString, JumpCondType::NOT_EQUALS);
        callBuiltin(sCmp, {handleToArg(lhs), handleToArg(rhs)}, handleToArg(dst));
        cJmp(done);

        bindHint("referential equality");
        bind(notString);
        mc.writeRegInst(X64Instruction::cmp, allocator.getReg(lhs), allocator.getReg(rhs));
        movLox(dst, tru);
        cJmp2(doFalse, normalCmp);
        cJmp(done);

        // mov falsy value
        bind(doFalse);
        movLox(dst, fal);

        bind(done);
    }

    void doBin(BinaryType type, size_t dst, size_t lhs, size_t rhs) override {
        if (type == BinaryType::SUB) {
            DO_PERF("sub")
            fastArith(dst, lhs, rhs, BinaryOp::SUB, crashLabel);
            return;
        }
        if (type == BinaryType::MUL) {
            DO_PERF("mul")
            fastArith(dst, lhs, rhs, BinaryOp::MUL, crashLabel);
            return;
        }
        if (type == BinaryType::DIV) {
            DO_PERF("div")
            fastArith(dst, lhs, rhs, BinaryOp::DIV, crashLabel);
            return;
        }
        if (type == BinaryType::GT) {
            DO_PERF("gt")
            fastCmp(dst, lhs, rhs, JumpCondType::GREATER, crashLabel);
            return;
        }
        if (type == BinaryType::LESS) {
            DO_PERF("less")
            fastCmp(dst, lhs, rhs, JumpCondType::LESS, crashLabel);
            return;
        }
        if (type == BinaryType::GEQ) {
            DO_PERF("geq")
            fastCmp(dst, lhs, rhs, JumpCondType::GREATER_OR_EQUAL, crashLabel);
            return;
        }
        if (type == BinaryType::LEQ) {
            DO_PERF("leq")
            fastCmp(dst, lhs, rhs, JumpCondType::LESS_OR_EQUAL, crashLabel);
            return;
        }
        if (type == BinaryType::ADD) {
            DO_PERF("add")
            if (dst == lhs) PANIC();
            if (dst == rhs) PANIC();
            auto ctx = this->getAllocCtx();
            dst = ctx.ensureRegWriteback(dst);
            lhs = ctx.ensureReg(lhs);
            rhs = ctx.ensureReg(rhs);
            fastAdd(dst, lhs, rhs, crashLabel);
            ctx.restore();
            return;
        }
        if (type == BinaryType::EQ) {
            DO_PERF("eq")
            if (dst == lhs) PANIC();
            if (dst == rhs) PANIC();
            auto ctx = this->getAllocCtx();
            dst = ctx.ensureRegWriteback(dst);
            lhs = ctx.ensureReg(lhs);
            rhs = ctx.ensureReg(rhs);
            fastEq(dst, lhs, rhs, crashLabel, false);
            ctx.restore();
            return;
        }
        if (type == BinaryType::NEQ) {
            DO_PERF("neq")
            if (dst == lhs) PANIC();
            if (dst == rhs) PANIC();
            auto ctx = this->getAllocCtx();
            dst = ctx.ensureRegWriteback(dst);
            lhs = ctx.ensureReg(lhs);
            rhs = ctx.ensureReg(rhs);
            fastEq(dst, lhs, rhs, crashLabel, true);
            ctx.restore();
            return;
        }

        PANIC("unimplemented op {}", (uint64_t)type);
    }

    void numberGuard(size_t tgt, size_t src) override {
        auto ctx = getAllocCtx();
        tgt = ctx.ensureRegWriteback(tgt);
        src = ctx.ensureReg(src);
        numberGuard(ctx, src, tgt, crashLabel);
        movReg(tgt, src);
        ctx.restore();
    }

    void fastToBool(size_t tgt, size_t src, size_t trueValue, size_t falseValue) {
        PERF()
        auto doneLabel = makeLabel1();
        auto falseLabel = makeLabel1();

        getTagFast(tgt, src, 1);

        checkTagFast(tgt, LoxValue::ValueType2::NIL, falseLabel, JumpCondType::EQUALS, 1);

        movUnsigned(tgt, trueValue);

        cJmp(doneLabel);

        bind(falseLabel);
        movUnsigned(tgt, falseValue);

        bind(doneLabel);
    }

    void toBool(size_t _tgt, size_t _src) override {
        auto ctx = getAllocCtx();
        auto tgt = ctx.ensureRegWriteback(_tgt);
        auto src = ctx.ensureReg(_src);
        fastToBool(tgt, src, 1, 0);
        ctx.restore();
    }

    void allocateClosed(size_t ref, size_t frameId, size_t localId, size_t value, bool isConst) override {
        PERF()
        if (isConst) {
            auto ctx = this->getAllocCtx();
            auto tmp = ctx.allocReg();

            movReg(tmp, ref);

            vector<int> derefs;
            for (size_t i = 0; i < frameId; i++) {
                derefs.push_back(offsetof(FunctionRef, parent));
            }
            derefChain(tmp, derefs);
            writeMem(tmp, value, offsetof(FunctionRef, captures)+(sizeof(LoxValue*) * localId), sizeof(LoxValue));

            ctx.restore();
            return;
        }

        // TODO();
        callBuiltin(builtin::allocateClosed, {handleToArg(ref), Arg::Imm(frameId), Arg::Imm(localId), handleToArg(value)}, nullopt);
    }

    void fasterCall(size_t tgt, size_t subj, span<size_t> argz, Label crashLabel, bool doParamCheck = true, bool doExtract = true) {
        MAKE_PERF()
        PERF_START()
        bindHint("LOX - fasterCall");
        auto funcPtr = subj;
        if (doExtract) {
            getPtr(tgt, subj); // FunctionRef* in tgt
            funcPtr = tgt;
        }

        if (doParamCheck) {
            assert(not allocator.isStack(funcPtr));
            mc.cmpImm(allocator.getReg(funcPtr), offsetof(FunctionRef, argCount), argz.size());
            cJmp1(crashLabel, JumpCondType::NOT_EQUALS);
        }

        vector<size_t> argz2;
        argz2.push_back(funcPtr);
        for (auto arg : argz) {
            argz2.push_back(arg);
        }

        assert(not allocator.isStack(tgt));
        assert(not allocator.isStack(funcPtr));
        PERF_STOP()
        callC(Arg::MemoryValue(allocator.getReg(funcPtr), offsetof(FunctionRef, fPtr), 8), argz2, tgt);

        bindHint("LOX - fasterCall END");
    }

    void putPerf(size_t perfId) {
        mc.push(x86::Rax);
        mc.push(x86::Rcx);

        mc.mov(x86::Rax, (size_t)(&PERF_COUNTERS[perfId]));
        mc.mov(x86::Rcx, 1);
        mc.writeMemRegInst(X64Instruction::add, x86::Rax, 0, x86::Rcx);

        mc.pop(x86::Rcx);
        mc.pop(x86::Rax);
    }

    void pushBytes(u8 v, size_t n) {
        for (auto i = 0UL; i < n; i++) {
            mc.pushBack(v);
        }
    }

    void initICs() {
        pushBytes(0, 16-(bytes.size() % 16));
        for (auto [icId, size] : icSizes) {
            bindHint(stringify("IC {} - {}", icId, size));
            this->bindRawLabel(icId, IC_ID);
            assert(bytes.size() % 16 == 0);
            pushBytes(0, size);
        }

        this->simpleLink(IC_ID);
    }

    void dynCall(size_t tgt, size_t subj, size_t tmp, size_t tmp1, span<size_t> argz, Label crashLabel) {
        MAKE_PERF()
        PERF_START()

        auto handleNotFunctionLabel = makeLabel1();
        auto doneLabel = makeLabel1();

        getTagFast(tmp, subj);

        checkTagFast(tmp, LoxValue::ValueType2::FUNCTION_REF, handleNotFunctionLabel);
        PERF_STOP()
        fasterCall(tgt, subj, argz, crashLabel);
        PERF_START()
        cJmp(doneLabel);

        bind(handleNotFunctionLabel);

        checkTagFast(tmp, LoxValue::ValueType2::CLASS, crashLabel);

        handleInstantiation(tgt, subj, tmp, tmp1, doneLabel, argz);

        bind(doneLabel);
    }

    void handleInstantiation(size_t tgt, size_t subj, size_t tmp, size_t tmp1, Label doneLabel, std::span<size_t> argz) {
        callBuiltin(builtin::instantiate, {handleToArg(subj)}, handleToArg(tgt));

        getPtr(tmp, tgt);

        readMem(tmp, tmp, offsetof(ObjectRef, construcor), sizeof(FunctionRef*));
        // TODO FIXME if class doenst have constuctor, and user passes arguments we wont crash

        mc.cmpImm(allocator.getReg(tmp), 0);
        cJmp1(doneLabel, JumpCondType::EQUALS);

        readMem(tmp1, tmp, offsetof(FunctionRef, captures), sizeof(LoxValue));
        writeMem(tmp, tgt, offsetof(FunctionRef, captures), sizeof(LoxValue));

        PERF_STOP()
        fasterCall(tgt, tmp, argz, crashLabel);

        writeMem(tmp, tmp1, offsetof(FunctionRef, captures), sizeof(LoxValue));
    }

    void callMethod(size_t tgt1, size_t subj1, u32 fieldId, span<size_t> argz, std::optional<size_t> methodFrame) override {
        callMethodIC(tgt1, subj1, fieldId, argz, methodFrame);
    }

    std::map<size_t, size_t> icSizes;

    template<typename T>
    size_t allocateIC() {
        auto label = allocateLabel();
        icSizes[label] = sizeof(T);

        return label;
    }

    void readIC(x86::X64Register dst, size_t id, size_t offset) {
        auto space = mc.writeRegRipInst(X64Instruction::mov, dst, offset);
        requestLabelRel4(id, space, IC_ID, offset);
    }


    Arg icToArg(size_t ic) {
        return Arg::Rel32Adr(IC_ID, ic, 0);
    }


    void callMeth(size_t tgt1, size_t subj1, u32 fieldId, span<size_t> argz, std::optional<size_t> methodFrame) {
        MAKE_PERF()
        PERF_START()
        auto ctx = this->getAllocCtx();
        auto tgt = ctx.ensureRegWriteback(tgt1);
        auto subj = ctx.ensureReg(subj1);
        auto tmp = ctx.allocReg();
        auto tmp1 = methodFrame.has_value() ? ctx.allocReg() : -1;

        auto t = ctx.originalTransform(argz);

        getTagFast(tmp, subj);
        checkTagFast(tmp, LoxValue::ValueType2::INSTANCE, crashLabel);

        // FIXME this should be push/pop ... but iam retard and use rsp for indexing :)
        // preserve `this`
        if (methodFrame.has_value()) {
            readMem(tmp1, *methodFrame, offsetof(FunctionRef, captures), sizeof(LoxValue));
        }

        PERF_STOP()
        callBuiltin(builtin::getMethod, {handleToArg(subj), Arg::Imm(fieldId), Arg::Imm(argz.size())}, handleToArg(tmp));

        // param check is done as part of getMethod
        fasterCall(tgt, tmp, t, crashLabel, false);

        // restore `this`
        if (methodFrame.has_value()) {
            writeMem(*methodFrame, tmp1, offsetof(FunctionRef, captures), sizeof(LoxValue));
        }

        ctx.restore();
    }

    void callMethodIC(size_t tgt1, size_t subj1, u32 fieldId, span<size_t> argz, std::optional<size_t> methodFrame) {
        MAKE_PERF()
        PERF_START()
        auto ctx = this->getAllocCtx();
        auto tgt = ctx.ensureRegWriteback(tgt1);
        auto subj = ctx.ensureReg(subj1);
        auto tmp = ctx.allocReg();
        auto tmp1 = methodFrame.has_value() ? ctx.allocReg() : -1;

        auto t = ctx.originalTransform(argz);

        auto fastPath = makeLabel1();
        auto doCall = makeLabel1();

        auto icId = allocateIC<MethodCallIC>();

        // BEGIN

        getTagFast(tmp, subj);
        checkTagFast(tmp, LoxValue::ValueType2::INSTANCE, crashLabel);

        // FIXME this should be push/pop ... but iam retard and use rsp for indexing :)
        // preserve `this`
        if (methodFrame.has_value()) {
            readMem(tmp1, *methodFrame, offsetof(FunctionRef, captures), sizeof(LoxValue));
        }

        getPtr(tmp, subj);
        READ_IC(allocator.getReg(tgt), icId, MethodCallIC, methodEntry.first);
        mc.writeMemRegInst(X64Instruction::cmp, allocator.getReg(tmp), offsetof(ObjectRef, shape), allocator.getReg(tgt));
        READ_IC(allocator.getReg(tmp), icId, MethodCallIC, methodEntry.second);
        cJmp2(fastPath, JumpCondType::EQUALS);

        PERF_STOP()
        callBuiltin(builtin::getMethodIC, {handleToArg(subj), Arg::Imm(fieldId), Arg::Imm(argz.size()), icToArg(icId)}, handleToArg(tmp));
        cJmp(doCall);

        bind(fastPath);
        // patch `this` for callee
        writeMem(tmp, subj, offsetof(FunctionRef, captures), sizeof(LoxValue));

        bind(doCall);
        // param check is done as part of getMethod
        fasterCall(tgt, tmp, t, crashLabel, false, false);

        // restore `this`
        if (methodFrame.has_value()) {
            writeMem(*methodFrame, tmp1, offsetof(FunctionRef, captures), sizeof(LoxValue));
        }

        ctx.restore();
    }


    void functionCall(size_t tgt, size_t subj, span<size_t> argz, bool doParamCheck) override {
        fasterCall(tgt, subj, argz, crashLabel, doParamCheck);
    }

    void dynamicCall(size_t tgt, size_t subj, span<size_t> argz) override {
        auto ctx = this->getAllocCtx();
        tgt = ctx.ensureRegWriteback(tgt);
        subj = ctx.ensureReg(subj);
        auto tmp = ctx.allocReg();
        auto tmp1 = ctx.allocReg();

        dynCall(tgt, subj, tmp, tmp1, argz, crashLabel);

        ctx.restore();
    }

    void allocateObject(size_t tgt, size_t subj, span<size_t> argz) override {
        TODO();
    }

    void allocateClosure(size_t tgt, Function *f, size_t parent) override {
        PERF()
        callBuiltin(builtin::allocateClosure, {Arg::ImmPtr(f), handleToArg(parent)}, handleToArg(tgt));
    }

    void allocateClass(size_t tgt, Class* clazz, size_t frame, std::optional<size_t> super) override {
        PERF()
        callBuiltin(builtin::allocateClass, {Arg::ImmPtr(clazz), super.has_value() ? handleToArg(*super) : Arg::Imm(LoxValue::Nil().internal), handleToArg(frame)}, handleToArg(tgt));
    }

    void readClosed(size_t dst, size_t ref, size_t frameId, size_t localId, bool isConst) override {
        PERF()
        if (frameId == 0 && isConst) {
            readMem(dst, ref, offsetof(FunctionRef, captures)+(sizeof(LoxValue*) * localId), 8);
            return;
        }

        movReg(dst, ref);

        vector<int> derefs;
        for (size_t i = 0; i < frameId; i++) {
            derefs.push_back(offsetof(FunctionRef, parent));
        }
        derefs.push_back(offsetof(FunctionRef, captures)+(sizeof(LoxValue*) * localId));
        if (not isConst) derefs.push_back(0);
        derefs.push_back(0);

        derefChain(dst, derefs);
    }

    void writeClosed(size_t ref, size_t frameId, size_t localId, size_t value, bool isConst) override {
        PERF()
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

    void negate(size_t _tgt, size_t _src) override {
        PERF()
        auto ctx = getAllocCtx();
        auto tgt = ctx.ensureRegWriteback(_tgt);
        auto src = ctx.ensureReg(_src);
        fastToBool(tgt, src, std::bit_cast<size_t>(LoxValue::False()), std::bit_cast<size_t>(LoxValue::True()));
        ctx.restore();
    }

    void copyClosed(size_t _dst, size_t _src, size_t dstId, size_t srcId) override {
        PERF()
        auto ctx = this->getAllocCtx();
        auto dst = ctx.ensureRegWriteback(_dst);
        auto src = ctx.ensureReg(_src);
        auto tmp = ctx.allocReg();
        auto tmp1 = ctx.allocReg();

        readMem(tmp1, src, offsetof(FunctionRef, captures)+(sizeof(LoxValue*)*srcId), sizeof(LoxValue*));
        getPtr(tmp, dst);
        writeMem(tmp, tmp1, offsetof(FunctionRef, captures)+(sizeof(LoxValue*)*dstId), sizeof(LoxValue*));

        ctx.restore();
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

struct MilaIrBase: IRInstruction<MilaGenCtx> {
    using IRInstruction<MilaGenCtx>::IRInstruction;

    virtual size_t tmpCount() {
        return 0;
    }

    virtual bool allowTgtAlias() {
        return true;
    }
};

template<StringLiteral S>
struct MilaIR: MilaIrBase {
    explicit MilaIR(SSARegisterHandle target): MilaIrBase(target, std::string(S.asView())) {}
};

template<StringLiteral S>
struct MilaIRNoRet: MilaIR<S> {
    MilaIRNoRet(): MilaIR<S>(SSARegisterHandle::invalid()) {

    }
};

struct MilaIRX86: MilaIrBase {
    PUB_VIRTUAL_COPY(MilaIRX86)
    using GEN_FUNC = std::function<void(MilaCodeGen&, MilaIRX86&)>;
    std::vector<SSARegisterHandle> argz;
    size_t paramCount;
    GEN_FUNC genFunction;

    void visitSrc(function<void (SSARegisterHandle &)> fn) override {
        for (auto& arg : argz) fn(arg);
    }

    MilaIRX86(SSARegisterHandle target, std::string name, std::vector<SSARegisterHandle> argz, size_t paramCount, GEN_FUNC gen): MilaIrBase(target, name), argz(argz), paramCount(paramCount), genFunction(gen) {

    }

    void generate(MilaCodeGen& gen) override {
        genFunction(gen, *this);
    }

    void print(MilaIrGen&, std::ostream& steam) override {
        basePrint(steam, "");
    }
};

struct LoxBool: public MilaIR<"lox_bool"> {
    PUB_VIRTUAL_COPY(LoxBool)
    bool v;

    LoxBool(SSARegisterHandle target, bool v) : MilaIR(target), v(v) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {}

    void print(MilaIrGen&, std::ostream& steam) override {
        basePrint(steam, "{}", v);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.movUnsigned(gen.getReg(target), std::bit_cast<uint64_t>(LoxValue::Bool(v)));
    }
};

struct LoxBranch: public MilaIRNoRet<"lox_branch"> {
    PUB_VIRTUAL_COPY(LoxBranch)
    size_t tru;
    size_t fals;
    SSARegisterHandle subj;

    LoxBranch(size_t tru, size_t fals, SSARegisterHandle subj) : MilaIRNoRet(), tru(tru), fals(fals), subj(subj) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(subj);
    }

    void print(MilaIrGen&, std::ostream& steam) override {
        basePrint(steam, "{} ? {} : {}", subj, tru, fals);
    }

    vector<size_t> branchTargets() const override {
        return {tru, fals};
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.loxBool(gen.getJmpLabelForBlock(tru), gen.getJmpLabelForBlock(fals), gen.getReg(subj), [&]{
            gen.assignPhis(tru);
        }, [&]{
            gen.assignPhis(fals);
        });
    }
};

struct LoxCallMethod: public MilaIR<"call_method"> {
    PUB_VIRTUAL_COPY(LoxCallMethod)
    SSARegisterHandle subj;
    u32 methodId;
    std::string methodName;
    std::vector<SSARegisterHandle> argz;
    std::optional<SSARegisterHandle> methodFrame;

    LoxCallMethod(SSARegisterHandle target, SSARegisterHandle subj, u32 methodId, std::string_view methodName, std::vector<SSARegisterHandle> argz, std::optional<SSARegisterHandle> methodFrame) : MilaIR(target), subj(subj), methodId(methodId), methodName(std::string(methodName)), argz(argz), methodFrame(methodFrame) {}

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


struct LoxNil: public MilaIR<"lox_nil"> {
    PUB_VIRTUAL_COPY(LoxNil)

    LoxNil(SSARegisterHandle target) : MilaIR(target) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {}

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "");
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.movUnsigned(gen.getReg(target), std::bit_cast<uint64_t>(LoxValue::Nil()));
    }
};

struct LoxNumber: public MilaIR<"lox_number"> {
    PUB_VIRTUAL_COPY(LoxNumber)
    double v;

    LoxNumber(SSARegisterHandle target, double v) : MilaIR(target), v(v) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {}

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{}", v);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.movUnsigned(gen.getReg(target), std::bit_cast<uint64_t>(LoxValue::Number(v)));
    }
};

std::unordered_set<void*> STATIC_ROOTS;

struct LoxString: public MilaIR<"lox_string"> {
    PUB_VIRTUAL_COPY(LoxString)
    string v;

    LoxString(SSARegisterHandle target, string v) : MilaIR(target), v(v) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {}

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "\"{}\"", escape(v));
    }

    void generate(MilaCodeGen& gen) override {
        auto alloc = allocateLoxString(string_view(v.data()+1, v.size()-2));
        STATIC_ROOTS.insert(alloc);
        gen.assembler.movUnsigned(gen.getReg(target), std::bit_cast<uint64_t>(LoxValue::String(alloc)));
    }
};

struct LoxNeg: public MilaIR<"lox_neg"> {
    PUB_VIRTUAL_COPY(LoxNeg)
    SSARegisterHandle v;

    LoxNeg(SSARegisterHandle target, SSARegisterHandle v) : MilaIR(target), v(v) {}

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

/*bool isOnlyNumber(BinaryType type) {
    switch (type) {
        case ADD:
        case MOD:
        case EQ:
        case NEQ:
        case AND:
        case OR:
            return false;
        case SUB:
        case DIV:
        case MUL:
        case GT:
        case GEQ:
        case LEQ:
        case LESS:
            return true;
    }
}*/

struct LoxBin: public MilaIR<"bin"> {
    PUB_VIRTUAL_COPY(LoxBin)
    BinaryType type;
    SSARegisterHandle lhs;
    SSARegisterHandle rhs;

    LoxBin(SSARegisterHandle target, BinaryType type, SSARegisterHandle lhs, SSARegisterHandle rhs) : MilaIR(target), type(type), lhs(lhs), rhs(rhs) {}

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

struct LoxReadField: public MilaIR<"read_field"> {
    PUB_VIRTUAL_COPY(LoxReadField)
    SSARegisterHandle subj;
    string fieldName;
    u32 fieldId;

    LoxReadField(SSARegisterHandle target, SSARegisterHandle subj, string fieldName, u32 fieldId) : MilaIR(target), subj(subj), fieldName(fieldName), fieldId(fieldId) {}

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

struct LoxWriteField: public MilaIRNoRet<"write_field"> {
    PUB_VIRTUAL_COPY(LoxWriteField)
    SSARegisterHandle subj;
    string fieldName;
    SSARegisterHandle v;
    u32 fieldId;

    LoxWriteField(SSARegisterHandle subj, string fieldName, SSARegisterHandle v, u32 fieldId) : MilaIRNoRet(), subj(subj), fieldName(fieldName), v(v), fieldId(fieldId) {}

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


struct LoxDynamicCall: public MilaIR<"dynamic_call"> {
    PUB_VIRTUAL_COPY(LoxDynamicCall)
    SSARegisterHandle self;
    vector<SSARegisterHandle> argz;

    LoxDynamicCall(SSARegisterHandle target, SSARegisterHandle self, vector<SSARegisterHandle> argz) : MilaIR(target), self(self), argz(argz) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(self);
        for (auto& arg : argz) fn(arg);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{} {}", self, argz);
    }

    void generate(MilaCodeGen& gen) override {
        auto args = gen.getRegs(argz);
        gen.assembler.dynamicCall(gen.getReg(target), gen.getReg(self), args);
    }
};

struct LoxFunctionCall: public MilaIR<"function_call"> {
    PUB_VIRTUAL_COPY(LoxFunctionCall)
    SSARegisterHandle self;
    vector<SSARegisterHandle> argz;
    bool doParamCheck;

    LoxFunctionCall(SSARegisterHandle target, SSARegisterHandle self, vector<SSARegisterHandle> argz, bool doParamCheck) : MilaIR(target), self(self), argz(argz), doParamCheck(doParamCheck) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(self);
        for (auto& arg : argz) fn(arg);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{} {}", self, argz);
    }

    void generate(MilaCodeGen& gen) override {
        auto args = gen.getRegs(argz);
        gen.assembler.functionCall(gen.getReg(target), gen.getReg(self), args, doParamCheck);
    }
};

struct LoxConstCall: public MilaIR<"function_call"> {
    PUB_VIRTUAL_COPY(LoxConstCall)
    SSARegisterHandle self;
    vector<SSARegisterHandle> argz;
    Function* fuk;

    LoxConstCall(SSARegisterHandle target, SSARegisterHandle self, vector<SSARegisterHandle> argz, Function* fuk) : MilaIR(target), self(self), argz(argz), fuk(fuk) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(self);
        for (auto& arg : argz) fn(arg);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{}({}) {}", self, fuk->data.name, argz);
    }

    void generate(MilaCodeGen& gen) override {
        auto args = gen.getRegs(argz);
        gen.assembler.dynamicCall(gen.getReg(target), gen.getReg(self), args);
    }
};

struct LoxAllocateObject: public MilaIR<"alloc_object"> {
    PUB_VIRTUAL_COPY(LoxAllocateObject)
    SSARegisterHandle self;
    vector<SSARegisterHandle> argz;

    LoxAllocateObject(SSARegisterHandle target, SSARegisterHandle self, vector<SSARegisterHandle> argz) : MilaIR(target), self(self), argz(argz) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(self);
        for (auto& arg : argz) fn(arg);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{} {}", self, argz);
    }

    void generate(MilaCodeGen& gen) override {
        auto args = gen.getRegs(argz);
        gen.assembler.dynamicCall(gen.getReg(target), gen.getReg(self), args);
    }
};

struct LoxAllocateObjectTyped: public MilaIR<"alloc_object_typed"> {
    PUB_VIRTUAL_COPY(LoxAllocateObjectTyped)
    Class* clazz;
    SSARegisterHandle self;
    vector<SSARegisterHandle> argz;

    LoxAllocateObjectTyped(SSARegisterHandle target, Class* clazz, SSARegisterHandle self, vector<SSARegisterHandle> argz) : MilaIR(target), clazz(clazz), self(self), argz(argz) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(self);
        for (auto& arg : argz) fn(arg);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{} {}", self, argz);
    }

    void generate(MilaCodeGen& gen) override {
        auto args = gen.getRegs(argz);
        gen.assembler.allocateObjectTyped(gen.getReg(target), clazz, gen.getReg(self), args);
    }
};

struct BuiltinPrint: public MilaIRNoRet<"print"> {
    PUB_VIRTUAL_COPY(BuiltinPrint)
    SSARegisterHandle arg;

    BuiltinPrint(SSARegisterHandle arg) : MilaIRNoRet(), arg(arg) {}

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{}", arg);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.print1(gen.getReg(arg));
    }

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(arg);
    }
};

struct LoxBooling: public MilaIR<"to_bool"> {
    PUB_VIRTUAL_COPY(LoxBooling)
    SSARegisterHandle v;

    LoxBooling(SSARegisterHandle target, SSARegisterHandle v) : MilaIR(target), v(v) {}

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

struct LoxNumberGuard: public MilaIR<"number_guard"> {
    PUB_VIRTUAL_COPY(LoxNumberGuard)
    SSARegisterHandle v;

    LoxNumberGuard(SSARegisterHandle target, SSARegisterHandle v) : MilaIR(target), v(v) {}

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

struct LoxAllocateClass: public MilaIR<"allocate_class"> {
    PUB_VIRTUAL_COPY(LoxAllocateClass)
    Class* clazz;
    std::optional<SSARegisterHandle> super;
    SSARegisterHandle frame;

    LoxAllocateClass(SSARegisterHandle target, Class* clazz, std::optional<SSARegisterHandle> super, SSARegisterHandle frame) : MilaIR(target), clazz(clazz), super(super), frame(frame) {}

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

struct LoxReadGlobal: public MilaIR<"read_global"> {
    PUB_VIRTUAL_COPY(LoxReadGlobal)
    size_t id;

    LoxReadGlobal(SSARegisterHandle target, size_t id) : MilaIR(target), id(id) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {}

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{}", id);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.readGlobal(gen.getReg(target), id);
    }
};

struct LoxWriteGlobal: public MilaIRNoRet<"write_global"> {
    PUB_VIRTUAL_COPY(LoxWriteGlobal)
    size_t id;
    SSARegisterHandle v;

    LoxWriteGlobal(size_t id, SSARegisterHandle v) : MilaIRNoRet(), id(id), v(v) {}

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

struct LoxReadCaptured: public MilaIR<"read_captured"> {
    PUB_VIRTUAL_COPY(LoxReadCaptured)
    SSARegisterHandle closure;
    size_t fId = -1;
    size_t locId = -1;
    bool isConst;

    LoxReadCaptured(SSARegisterHandle target, SSARegisterHandle closure, size_t fId, size_t locId, bool isConst) : MilaIR(target), closure(closure), fId(fId), locId(locId), isConst(isConst) {}

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

struct LoxWriteCaptured: public MilaIRNoRet<"write_captured"> {
    PUB_VIRTUAL_COPY(LoxWriteCaptured)
    SSARegisterHandle closure;
    size_t fId = -1;
    size_t locId = -1;
    SSARegisterHandle v;
    bool isConst;

    LoxWriteCaptured(SSARegisterHandle closure, size_t fId, size_t locId, SSARegisterHandle v, bool isConst) : MilaIRNoRet(), closure(closure), fId(fId), locId(locId), v(v), isConst(isConst) {}

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

struct LoxAllocCaptured: public MilaIRNoRet<"alloc_captured"> {
    PUB_VIRTUAL_COPY(LoxAllocCaptured)
    SSARegisterHandle closure;
    size_t fId = -1;
    size_t locId = -1;
    SSARegisterHandle v;
    bool isConst;

    LoxAllocCaptured(SSARegisterHandle closure, size_t fId, size_t locId, SSARegisterHandle v, bool isConst) : MilaIRNoRet(), closure(closure), fId(fId), locId(locId), v(v), isConst(isConst) {}

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

struct LoxReadLocal: public MilaIR<"read_local"> {
    PUB_VIRTUAL_COPY(LoxReadLocal)
    size_t localId;
    string name;
    SSARegisterHandle frame;

    LoxReadLocal(SSARegisterHandle target, size_t localId, string name, SSARegisterHandle frame) : MilaIR(target), localId(localId), name(name), frame(frame) {}

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

struct LoxWriteLocal: public MilaIRNoRet<"write_local"> {
    PUB_VIRTUAL_COPY(LoxWriteLocal)
    size_t localId;
    string name;
    SSARegisterHandle v;
    SSARegisterHandle frame;

    LoxWriteLocal(size_t localId, string name, SSARegisterHandle v, SSARegisterHandle frame) : MilaIRNoRet(), localId(localId), name(name), v(v), frame(frame) {}

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

struct LoxAllocateClosure: public MilaIR<"allocate_closure"> {
    PUB_VIRTUAL_COPY(LoxAllocateClosure)
    Function* func;
    SSARegisterHandle parent;

    LoxAllocateClosure(SSARegisterHandle target, Function* func, SSARegisterHandle parent) : MilaIR(target), func(func), parent(parent) {}

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

struct LoxCopyCapture: public MilaIRNoRet<"copy_capture"> {
    PUB_VIRTUAL_COPY(LoxCopyCapture)
    SSARegisterHandle tgt;
    size_t tgtId;
    SSARegisterHandle src;
    size_t srcId;

    LoxCopyCapture(SSARegisterHandle tgt, size_t tgtId, SSARegisterHandle src, size_t srcId) : MilaIRNoRet(), tgt(tgt), tgtId(tgtId), src(src), srcId(srcId) {}

    void visitSrc(std::function<void(SSARegisterHandle&)> fn) override {
        fn(src);
        fn(tgt);
    }

    void print(MilaIrGen&, std::ostream& stream) override {
        basePrint(stream, "{}#{} <- {}#{}", tgt, tgtId, src, srcId);
    }

    void generate(MilaCodeGen& gen) override {
        gen.assembler.copyClosed(gen.getReg(tgt), gen.getReg(src), tgtId, srcId);
    }
};

struct LoxGetSuper: public MilaIR<"get_super"> {
    PUB_VIRTUAL_COPY(LoxGetSuper)
    SSARegisterHandle self;

    LoxGetSuper(SSARegisterHandle target, SSARegisterHandle self) : MilaIR(target), self(self) {}

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
    MilaReg(size_t blockId, string name, MilaDataType type1, Type type, std::optional<SSARegisterHandle> prev): SSARegister(blockId, name, type), dataType(type1) {
        this->previous = prev;
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
        return current().pushRegister(make_unique<MilaReg>(0, std::move(name), type, type1, prev));
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
                return *getCtx().lookupLocal(stringify("{}-{}", name, s.id));
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
            case HookedVariableType::LOCAL: {
                auto fullName = stringify("{}-{}", name, s.id);
                auto local = getCtx().gen.lookupOptionalLocal(fullName, getCtx().current());
                auto newVer = local.has_value() ? getCtx().generateNewVersion(*local) : getCtx().pushRegister(fullName, MilaDataType{}, {}, SSARegister::Type::VAR);
                // std::\cout << "GEN NEW WRITE: " << newVer.toString() << " - " << (local.has_value() ? local->toString() : "no-papa"s) << std::endl;
                getCtx().pushInstruction<instructions::Assign>(newVer, v);
                break;
            }
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
            curRet = getCtx().push<LoxDynamicCall>(MilaDataType{}, v, argz);
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
        std::optional<size_t> constructorParamCount;
        for (auto& methdod : it.data.methods) {
            auto isConstructor = methdod->data.name == "init";
            compFunk(*methdod, true, isConstructor);
            if (isConstructor) {
                constructorParamCount = methdod->data.argz.size();
            }
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
                    v3 = getCtx().push<instructions::IntLiteral>(MilaDataType{}, 1, false);
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
        auto closureReg = IR_GEN_CTX.pushRegister(CURRENT_CLOSSURE, MilaDataType{}, {}, SSARegister::Type::ARG);
        IR_GEN_CTX.pushInstruction<instructions::Arg>(closureReg);
        // auto localsCount = it.locals.size()-it.upValCount();

        pushCtx(IR_GEN_CTX);
        functionStack.push_back({&it, {isConstructor, isMethod}});

        size_t UP_VAL_OFFSET = isMethod ? 1 : 0;

        size_t upValId = UP_VAL_OFFSET;
        size_t localId = 0;

        for (auto i = 0u; i < it.data.argz.size(); i++) {
            auto arg = it.data.argz[i];
            auto poop = IR_GEN_CTX.pushRegister(arg, MilaDataType{}, {}, SSARegister::Type::ARG);
            IR_GEN_CTX.pushInstruction<instructions::Arg>(poop);

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
        assert(not functionStack.empty());
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
        auto clazz = new ClassRef{&it, super, currentFrame, nullptr, LoxMap{}};
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

        auto* res = builtin::rawInstant(clazz);

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

auto CLOCK = makeStuff2<Function>("clock"s, vector<string>{}, vector<ASTStm>{});

struct AnalyzeGlobals {
    struct GlobalData {
        std::set<Function*> observedFuncs;
        std::set<Class*> observedClasses;
        std::string name;
        bool isFucked; // we couldn't ded
    };
    std::map<size_t, GlobalData> data;

    void processInstruction(ControlFlowGraph<MilaGenCtx>& cfg, size_t globalId, SSARegisterHandle value, std::set<SSARegisterHandle>& visited) {
        if (visited.contains(value)) return;
        visited.insert(value);

        auto valueInst = cfg.resolveInstruction(value);

        if (auto f = valueInst->template cst<LoxAllocateClosure>(); f) {
            data[globalId].observedFuncs.insert(f->func);
        } else if (auto c = valueInst->cst<LoxAllocateClass>()) {
            data[globalId].observedClasses.insert(c->clazz);
        } else if (instructions::PhiFunction<MilaGenCtx>* phi = valueInst->cst<instructions::PhiFunction>(); phi) {
            for (auto v : phi->getVersions()) {
                processInstruction(cfg, globalId, v, visited);
            }
        } else if (instructions::Assign<MilaGenCtx>* ass = valueInst->cst<instructions::Assign>(); ass) {
            processInstruction(cfg, globalId, ass->value, visited);
        } else {
            // println("BAIL OUT! of analysis bcs {} - {}", valueInst->name, valueInst->target);
            data[globalId].isFucked = true;
        }
    }

    void analyze(ControlFlowGraph<MilaGenCtx>& cfg) {
        data[0].observedFuncs.insert(CLOCK);
        cfg.forEachInstruction([&](auto& instruction) {
            if (LoxWriteGlobal* g = instruction.template cst<LoxWriteGlobal>(); g) {
                std::set<SSARegisterHandle> pepa;
                processInstruction(cfg, g->id, g->v, pepa);
            }
        });
    }

    void dump() {
        println("=== GLOBAL ANALYSIS ===");
        for (auto& [id, data] : data) {
            println("{} - {}", id, data.isFucked);
            for (auto klass : data.observedClasses) {
                println("  class {}", klass->data.name);
            }
            for (auto func : data.observedFuncs) {
                println("  function {}", func->data.name);
            }
        }
        println("=== END GLOBAL ANALYSIS ===");
    }
};

void lower(ControlFlowGraph<MilaGenCtx>& cfg) {
    std::vector<std::function<void()>> toRewrite;


    cfg.forEachInstruction([&](IRInstruction<MilaGenCtx>& instruction, auto& block) mutable {
        if (auto jmp = instruction.cst<instructions::Jump>(); jmp) {
            auto toAssign = cfg.getBlock(jmp->value).getPhis(block.id());

            toRewrite.push_back([&block, jmp, toAssign] {
                auto id = block.getId(jmp);
                for (auto [value, target] : toAssign) {
                    block.insert(std::make_unique<instructions::Assign<MilaGenCtx>>(target, value), id);
                }
                jmp->shouldAssign = false;
            });
        }
    });

    for (auto toR : toRewrite) {
        toR();
    }
}

struct DeVirtualize {
    struct GlobalData {
        std::set<Function*> observedFuncs;
        std::set<Class*> observedClasses;
        bool isFucked; // we couldn't ded
    };
    std::map<LoxDynamicCall*, GlobalData> data;
    AnalyzeGlobals& globs;

    DeVirtualize(AnalyzeGlobals& g): globs(g) {

    }

    void processInstruction(ControlFlowGraph<MilaGenCtx>& cfg, LoxDynamicCall* globalId, SSARegisterHandle value, std::set<SSARegisterHandle>& visited) {
        if (visited.contains(value)) return;
        visited.insert(value);

        auto valueInst = cfg.resolveInstruction(value);

        if (auto f = valueInst->template cst<LoxAllocateClosure>(); f) {
            data[globalId].observedFuncs.insert(f->func);
        } else if (auto c = valueInst->cst<LoxAllocateClass>()) {
            data[globalId].observedClasses.insert(c->clazz);
        } else if (LoxReadGlobal* glob = valueInst->cst<LoxReadGlobal>(); glob) {
            auto pepa = globs.data[glob->id];
            data[globalId].observedFuncs.insert(pepa.observedFuncs.begin(), pepa.observedFuncs.end());
            data[globalId].observedClasses.insert(pepa.observedClasses.begin(), pepa.observedClasses.end());
            data[globalId].isFucked = globs.data[glob->id].isFucked;
        }  else if (instructions::PhiFunction<MilaGenCtx>* phi = valueInst->cst<instructions::PhiFunction>(); phi) {
            for (auto v : phi->getVersions()) {
                processInstruction(cfg, globalId, v, visited);
            }
        } else if (instructions::Assign<MilaGenCtx>* ass = valueInst->cst<instructions::Assign>(); ass) {
            processInstruction(cfg, globalId, ass->value, visited);
        } else {
            // println("BAIL OUT! of analysis bcs {} - {}", valueInst->name, valueInst->target);
            data[globalId].isFucked = true;
        }
    }

    void analyze(ControlFlowGraph<MilaGenCtx>& cfg) {
        cfg.forEachInstruction([&](auto& instruction) {
            if (LoxDynamicCall* g = instruction.template cst<LoxDynamicCall>(); g) {
                std::set<SSARegisterHandle> pepa;
                processInstruction(cfg, g, g->self, pepa);
                auto d = data[g];
                if (not d.isFucked && d.observedClasses.empty() && !d.observedFuncs.empty()) {
                    size_t invalid = 9999;
                    size_t commonSize = invalid;
                    for (auto o : d.observedFuncs) {
                        if (commonSize == o->data.argz.size()) continue;
                        if (commonSize == invalid) commonSize = o->data.argz.size();
                        commonSize = invalid;
                        break;
                    }
                    cfg.patchInst<LoxFunctionCall>(g, g->target, g->self, g->argz, commonSize != invalid);
                }
                if (not d.isFucked && !d.observedClasses.empty() && d.observedFuncs.empty()) {

                }
            }
        });
    }

    void dump() {
        println("=== VIRT ANALYSIS ===");
    /*    for (auto& [id, data] : data) {
            println("{} - {}", id->target, data.isFucked);
            for (auto klass : data.observedClasses) {
                println("  class {}", klass->data.name);
            }
            for (auto func : data.observedFuncs) {
                println("  function {}", func->data.name);
            }
        }*/
        println("=== END VIRT ANALYSIS ===");
    }
};

struct GenLoxBranch {
    void analyze(ControlFlowGraph<MilaGenCtx>& cfg) {
        std::vector<SSARegisterHandle> stuff;

        cfg.forEachInstruction([&](auto& instruction) {
            if (instructions::Branch<MilaGenCtx>* branch = instruction.template cst<instructions::Branch>(); branch) {
                auto source = cfg.resolveInstruction(branch->condition);
                if (LoxBooling* booling = source->cst<LoxBooling>(); booling) {
                    cfg.patchInst<LoxBranch>(branch, branch->scopeT, branch->scopeF, booling->v);
                    stuff.push_back(booling->target);
                }
            }
        });

        for (auto d : stuff) {
            cfg.removeInstruction(d);
        }
    }
};

using BaseIR = IRInstruction<MilaGenCtx>;

struct PlatformGen {

};

struct X86PlatformGen: PlatformGen {

};

#define CAS(name, type) type* name = instruction.template cst<type>(); name
#define REF(...) ({auto _it = _VA_ARGS_; _it})
void rewriteInstructions(ControlFlowGraph<MilaGenCtx>& cfg) {
    cfg.forEachInstruction([&](BaseIR& instruction) {
        if (CAS(call, LoxDynamicCall)) {
            cfg.patchInst<MilaIRX86>(instruction.target, "LoxDynamicCall", call->getSources(), 2, [=](MilaCodeGen& gen, MilaIRX86& self) {
                auto& assm = dynamic_cast<X86MilaAssembler&>(gen.assembler);
                assm.dynCall(gen.getReg(call->target), gen.getReg(call->self), gen.getTmp(0), gen.getTmp(1), ({auto _it = gen.getRegs(call->argz); std::span<size_t> xd = _it; xd;}), assm.crashLabel);
            });
        }
    });
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
            return "true";
        case BOOL_FALSE:
            return "false";
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
        heapSize = 2048ul*1024ul*1024ul;
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

        // collectManagedPtr(self->fields);
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
        auto stackEnd = STACK_BASE_ADDRESS();

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

        // collectManagedPtr(self->fields);
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

struct EpsilonHeap {
    char* heap;
    size_t offset;
    bool debugGc;
    void* stackStart;
    size_t gcCount = 0;

    void setupPages() {
        heap = (char*)mmap(nullptr, 16*2048*1024*1024ul, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        // memset(heap, 0, 2048*1024*1024ul);
        offset = 0;
    }

    bool isManagedPtr(void* ptr) {
        return true;
    }

    void* allocate(AllocType type, size_t size) {
        auto ptr = heap+offset;
        offset += align(size, 16);
        return ptr;
    }
};

struct EpsilonMallocHeap {
    char* heap;
    size_t offset;
    bool debugGc;
    void* stackStart;
    size_t gcCount = 0;

    void setupPages() {
    }

    bool isManagedPtr(void* ptr) {
        return true;
    }

    void* allocate(AllocType type, size_t size) {
        return malloc(size);
    }
};

EpsilonHeap heap;
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
    auto v1 = heap.allocate(type, size);
    // std::memset(v1, 0, size);
    return v1;

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
 /*   cpu_set_t mask;
    int cpu = 10; // CPU core to pin to

    // Initialize the CPU set
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    sched_setaffinity(0, sizeof(mask), &mask);*/
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

    CLOCK->runtimeData = (void*)&loxClock; // used by jit
    CLOCK->native = [&](ASTExecutor& ctx) { ctx.push(LoxValue::Number(duration_cast<std::chrono::microseconds>((std::chrono::high_resolution_clock::now()-start1)).count()/1'000'000.0)); }; // used by interp
    auto clkk = builtin::allocateClosure(CLOCK, nullptr);
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

    if (true) {
/*        AnalyzeGlobals anallizer;

        for (auto& funk : comp.irGens) {
            anallizer.analyze(funk->graph);
        }
        // anallizer.dump();

        DeVirtualize deVirt(anallizer);

        for (auto& funk : comp.irGens) {
            deVirt.analyze(funk->graph);
        }*/
        // deVirt.dump();

        GenLoxBranch bench;
        for (auto& funk : comp.irGens) {
            bench.analyze(funk->graph);
        }
    }

    std::vector<std::pair<Function*, void*>> perfs;

    for (size_t i = 0; i < comp.funks.size(); i++) {
        auto funk = comp.funks[i];
        std::vector<size_t> argSizes;
        argSizes.push_back(sizeof(FunctionRef*));
        for (size_t j = 0; j < funk->data.argz.size(); j++) {
            argSizes.push_back(sizeof(LoxValue));
        }

        X86MilaAssembler assm(argSizes, sizeof(LoxValue));
        if (funk == globalFunc) {
            assm.beSpetial();
        }
        SimpleAlloc<MilaGenCtx> sAlloc;
        BetterAllocator<MilaGenCtx> bAlloc(assm.allocator);
        MilaCodeGen ggs(assm, *comp.irGens[i], funk->data.name);
        ggs.allocator = &bAlloc; // false ? (Allocator<MilaGenCtx>*)&sAlloc : (Allocator<MilaGenCtx>*)&bAlloc;
        if (DEBUG_JIT) ggs.dumpGraphPNG = true;

        if (DEBUG_JIT) {
            ggs.printLinearized = true;
            ggs.printLiveRanges = true;
            ggs.warnLeak = true;
        }

        UNWRAPV(ggs.gen());

        assm.bind(assm.crashLabel);
        assm.mc.hlt();


        auto stackSize = assm.preserveCalleeRegs();

        assm.patchStackSize(align(stackSize, X86MilaAssembler::STACK_ALIGNMENT));

        // UNWRAPV(linkRelative(assm.bytes.data(), assm.spaces, assm.labels));
        assm.linkJumps();

        assm.mc.hlt();
        assm.initICs();
        assm.mc.hlt();

        // assm.linkPerf();

        ggs.assembler.dumpHints(stringify("hints/{}", comp.funks[i]->data.name));

        auto codeSize = assm.bytes.size();
        auto code = cg::allocateJIT(codeSize, &PERF_START);

        // auto codeAdr = (uintptr_t)code;
        // auto perfAdr = (uintptr_t)&PERF_START;
        // auto mag = std::max(codeAdr, perfAdr);
        // auto mig = std::min(codeAdr, perfAdr);
        // auto diff = mag-mig;
        // std::cout << "ADDR DIFF IS: " << diff << "B" << " - " << diff/4096 << "pages " << diff/(1024*1024ul*1024ul) << "GB" << std::endl;
        // perfs.emplace_back(funk, ((char*)code)+assm.getPerfOffset());

        std::memcpy(code, assm.bytes.data(), codeSize);

        cg::makeRXW(code, codeSize);

        comp.funks[i]->runtimeData = code;

        writeBytesToFile(stringify("bins/{}", comp.funks[i]->data.name), assm.bytes);
    }

    assert(comp.funks[0]->runtimeData != nullptr);
    auto funk = reinterpret_cast<GlobalFunck>(comp.funks[0]->runtimeData);

    auto ex1 = std::chrono::high_resolution_clock::now();

    auto funk1 = builtin::allocateClosure(globalFunc, nullptr);
    // auto startTicks = __builtin_ia32_rdtsc();

    auto x = funk1.asFunction();
    funk(x);

    // auto endTicks = __builtin_ia32_rdtsc();

    auto ex2 = std::chrono::high_resolution_clock::now();

    // auto ticksDur = endTicks-startTicks;

    // std::cout << "execution took: " << std::chrono::duration_cast<std::chrono::milliseconds>(ex2-ex1).count() << std::endl;

    if (DEBUG_JIT)
        std::cout << "execution took: " << std::chrono::duration_cast<std::chrono::milliseconds>(ex2-ex1).count() << std::endl;


    int total = 0;
    size_t badStuff = 0;
    for (auto i = 0; i < 128; i++) {
        if (PERF_NAMES[i] == nullptr) continue;
        if (strcmp(PERF_NAMES[i],"doStuff") == 0) total = PERF_COUNTERS[i];
        if (strcmp(PERF_NAMES[i],"valid") == 0) badStuff = PERF_TIMES[i]/PERF_COUNTERS[i];
    }

    // PerfEntry perfs[100];
/*
    for (auto [funk, perf] : perfs) {
        PerfEntry* entries = (PerfEntry*)perf;
        for (auto i = 0; i < 128; i++) {
            auto idk = PERF_NAMES[i];
            if (idk == nullptr) continue;

            auto entry = entries[i];

            auto cost = ((entry.counter == 0) ? 0.0 : ((entry.time*1000)/entry.counter)/1000.0);
            auto fixed_time = entry.time-22*entry.counter;
            auto fixed_cost = ((entry.counter == 0) ? 0.0 : ((fixed_time*1000)/entry.counter)/1000.0);

            std::cout << idk << " # count=" << entry.counter << ", time=" << entry.time << ", cost=" << cost << ", fixed_cost=" << fixed_cost << std::endl;
        }
    }*/

    // badStuff /= 2;
 /*   std::cout << "TOTOAL: " << ticksDur << " - " << badStuff << std::endl;
    for (auto i = 0; i < 128; i++) {
        auto idk = PERF_NAMES[i];
        if (idk == nullptr) continue;

        auto part = ticksDur/100'000;
        auto fixup = PERF_COUNTERS[i]*badStuff;

        double percent = ((double)PERF_COUNTERS[i]/(double)total)*100;
        if (PERF_TIMES[i] < fixup) TODO();
        auto percent1 = ((PERF_TIMES[i])/part)/1'000.0;
        auto percent2 = ((PERF_TIMES[i]-fixup)/part)/1'000.0;

        std::cout << idk << " - " << PERF_TIMES[i]/(1000*1000) << "ms" << " - " << PERF_COUNTERS[i] << " - " << percent << "%" << " - " << percent1 << "%" << " - " << percent2 << "%" << " - " << PERF_TIMES[i]-fixup << " - " << PERF_TIMES[i]/PERF_COUNTERS[i] << std::endl;
    }*/
}