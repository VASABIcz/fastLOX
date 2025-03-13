#include "../lexing/Token.h"
#include "../lexing/SourceProvider.h"
#include "../lexing/lexerExceptions.h"
#include "../lexing/lexingUnits.h"
#include "../lexing/tokenize.h"
#include "../parsing/Parser.h"
#include "../parsing/exceptions.h"
// #include "../codegen/SSARegister.h"
// #include "../codegen/IRGen.h"
// #include "../codegen/CodeGen.h"
// #include "../codegen/IRGenCtx.h"
// #include "../codegen/x86/X86Assembler.h"
#include "../utils/pdo_utils.h"
#include <filesystem>
#include <cstring>
#include <cstdlib>

#define VERBOSE 0

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
struct MilaGenRet {};

struct ASTNode {
    virtual ~ASTNode() = default;
    // ASTNode() = default;

    vector<Token<TokenType1>> tokens;

    virtual void visit(ASTVisitor& it) = 0;
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
    ALLOC_UPVAL
};

struct SpecializedVariable {
    HookedVariableType type;
    size_t id;
    size_t frameId;
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

    vector<bool> locals;
    vector<size_t> captures; // list of parent function up-local-ids that we capture
    // size_t upValCount = 0;
    SpecTarget hookedTarget;
    std::function<void(ASTExecutor&)> native;

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
};

struct Class: Statement {
    STRUCT_BEGIN(Class)
    string name;
    optional<string> superClass;
    vector</*unique_ptr<*/Function*/*>*/> methods;
    STRUCT_END(Class)

    SpecTarget hookedTarget;
    SpecTarget hookedDst;

    Function* getMethod(string_view name) {
        for (auto& f : data.methods) {
            if (f->data.name == name) return f/*.get()*/;
        }
        return nullptr;
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
    REM,
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
        case TokenType1::Div: return optional{pair{BinaryType::DIV, 0}};
        case TokenType1::Mul: return optional{pair{BinaryType::MUL, 0}};
        case TokenType1::Modulo: return optional{pair{BinaryType::MOD, 0}};
        case TokenType1::OPB: return optional{pair{BinaryType::LESS, 0}};
        case TokenType1::CPB: return optional{pair{BinaryType::GT, 0}};
        case TokenType1::Plus: return optional{pair{BinaryType::ADD, 0}};
        case TokenType1::Minus: return optional{pair{BinaryType::SUB, 0}};
        case TokenType1::Equals: return optional{pair{BinaryType::EQ, 0}};
        case TokenType1::NotEquals: return optional{pair{BinaryType::NEQ, 0}};
        case TokenType1::LEQ: return optional{pair{BinaryType::LEQ, 0}};
        case TokenType1::GEQ: return optional{pair{BinaryType::GEQ, 0}};
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

            methods.push_back(makeStuff2<Function>(std::move(methodName), std::move(params), std::move(body)));
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


struct FunctionRef {
    FunctionRef* parent;
    Function* func;
    LoxValue* captures[];

    LoxValue read(size_t id);

    void write(size_t id, LoxValue val);
};

// closed variables are allocated separately on heap (boxed)
// if closed variables are arguments, we move them into boxes
// we hook variable accesses to assign/read to boxes
// we will only read upvalus of our parent function
// we need to propagate captured values

struct ClassRef {
    Class* clazz;
    ClassRef* super;
    FunctionRef* parent;

    Function* getConstructor() {
        auto res = clazz->getMethod("init");

        if (res != nullptr)  return res;

        if (super != nullptr) return super->getConstructor();;

        return nullptr;
    }
};

struct ObjectRef;

struct LoxValue {
    static constexpr u_int64_t NAN_MASK  = 0x7FFC000000000000; // 13 bits
    static constexpr u_int64_t TAG_MASK  = 0x8003000000000000; // 3 bits
    static constexpr u_int64_t DATA_MASK = 0x0000ffffffffffff; // 48 bits
    static constexpr u_int64_t INV_DATA_MASK = ~0x0000ffffffffffff;

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

    static bool isNan(u_int64_t value) {
        return (value & NAN_MASK) == NAN_MASK;
    }

    static ValueType2 toType(u_int64_t v) {
        if (!isNan(v)) { // not nan must be number i guess?
            return ValueType2::FLOAT;
        }
        auto tagBits = (v & TAG_MASK);

        return bit_cast<ValueType2>(tagBits);
    }

    static bool decodeBool(u_int64_t v) {
        return toType(v) == ValueType2::BOOL;
    }

    static u_int64_t encodeBool(bool v) {
        return NAN_MASK | (v ? bit_cast<uint64_t>(ValueType2::BOOL) : bit_cast<uint64_t>(ValueType2::BOOL_FALSE));
    }

    static void* decodePointer(u_int64_t v) {
        return bit_cast<void*>((v & DATA_MASK) << 2);
    }

    static u_int64_t encodePointer(u_int64_t tag, void* ptr1) {
        auto ptr = bit_cast<u_int64_t>(ptr1);
        assert((ptr & 3) == 0);
        assert(((ptr >> 2) & INV_DATA_MASK) == 0);
        assert(((ptr >> 2) & DATA_MASK) == (ptr >> 2));
        assert((DATA_MASK & NAN_MASK & tag) == 0);
        return tag | NAN_MASK | (ptr >> 2);
    }

    static u_int64_t encodeNil() {
        return NAN_MASK | bit_cast<uint64_t>(ValueType2::NIL);
    }

    u_int64_t internal;

    ValueType2 getType() const {
        return toType(internal);
    }

    double asNumber() const {
        return std::bit_cast<double>(internal);
    }

    string_view asString() const {
        return string_view{*(string*)decodePointer(internal)};
    }

    FunctionRef* asFunction() const {
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

    static LoxValue String(string_view s) {
        return{encodePointer((uint64_t)ValueType2::STRING, new string(s))}; // LoxValue{.v=STRING, .str=new string(s)};
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

    string toString() const {
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
                return stringify("{} instance", (*((ClassRef**)asObject()))->clazz->data.name);
                break;
        }
        // println("AAAAAAAAAAAAASDADASDASD {}", (long)v);
        UNREACHABLE();
    }

    bool toBool() const {
        switch (getType()) {
            case FLOAT:
            case CLASS:
            case INSTANCE:
            case FUNCTION_REF:
            case STRING:
                return true;
            case NIL:
                return false;
            case BOOL:
            case BOOL_FALSE:
                return asBool();
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

struct ObjectRef {
    ClassRef* clazz;
    ObjectRef* proto;
    map<string, LoxValue>* fields;
    vector<FunctionRef*> methods;

    LoxValue getMethod(const string& name) {
        for (auto m : methods) {
            if (m->func->data.name == name) return LoxValue::Function(m);
        }
        if (proto != nullptr) return proto->getMethod(name);
        PANIC();
    }

    LoxValue read(const string& name) {
        if (fields->contains(name)) return fields->at(name);
        return getMethod(name);
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

// GOALS:
// determine the capture chain of functions
// translate local names to index in parameter table
// - deshadow locals
// - determine if local is captured
struct Linerizer: ASTVisitor {
    struct HookData {
        SpecTarget* toPatch;
        Function* source;
        size_t relFrameId;
        size_t relLocalId;
        bool isDecl;
        Function* sourceUpFunction;

        bool isEscaped() {
            return source->locals[relLocalId];
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
        Function* function;
        vector<bool> isUpVal;

        vector<LexScope> lexicals;
        bool isGlobal = false;

        optional<size_t> getLocal(const string& s) {
            for (auto scope : lexicals | views::reverse) {
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

            isUpVal.push_back(false);
            lexicals.back().vars.emplace_back(s, isUpVal.size()-1);

            return isUpVal.size()-1;
        }

        size_t putRootLocal(const string& s) {
            assert(not lexicals.empty());
            auto cur = lexicals[0].getLocal(s);
            if (cur.has_value()) PANIC("local {} already defiend in curren scope", s);

            isUpVal.push_back(false);
            lexicals[0].vars.emplace_back(s, isUpVal.size()-1);

            return isUpVal.size()-1;
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
                if (isEscaped && f.isDecl) { // declaration of escaping upVal (redeclare it/alocate new one)
                    *f.toPatch = SpecializedVariable(HookedVariableType::ALLOC_UPVAL, f.source->getUpValId(f.relLocalId), 0);
                } else if (isEscaped) { // access to localy defined upval
                    *f.toPatch = SpecializedVariable(HookedVariableType::UPVAL, self->getUpValId(f.relLocalId), 0);
                } else { // access to ordinary local
                    *f.toPatch = SpecializedVariable(HookedVariableType::LOCAL, self->getLocalId(f.relLocalId), 0);
                }
            } else if (f.relFrameId == 1) { // capturing parents local
                auto self = f.sourceUpFunction;
                auto localId = self->putCapture(f.relLocalId);

                assert(not f.isDecl);
                *f.toPatch = SpecializedVariable(HookedVariableType::UPVAL, self->getUpValId(localId), 0); // offset 0, we copied upval to our frame
            } else { // capturing nested local
                auto closestChild = f.sourceUpFunction;
                auto someParentsId = closestChild->putCapture(f.relLocalId);

                assert(not f.isDecl);
                *f.toPatch = SpecializedVariable(HookedVariableType::UPVAL, closestChild->getUpValId(someParentsId), f.relFrameId-1); // relFrameId-1, we access version stored in nodes child
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
        hookIdent("__super", &it.hookedTarget, false);
    }

    void invoke(This& it) override {
        hookIdent("__this", &it.hookedTarget, false);
    }

    void invoke(FieldAccess &it) override {
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

        hookIdent(it.data.name, &it.hookedDst, true);

        if (it.data.superClass.has_value()) {
            hookIdent(*it.data.superClass, &it.hookedTarget, false);
        }

        for (auto& method : it.data.methods) {
            stack.emplace_back();
            stack.back().lexicals.emplace_back();
            stack.back().function = method/*.get()*/;

            for (auto a : method->data.argz) {
                stack.back().putLocal(a);
            }

            stack.back().markUpVal(stack.back().putLocal("__this"));
            stack.back().markUpVal(stack.back().putLocal("__super"));

            for (auto& s : method->data.body) {
                hook(s);
            }

            method->locals = stack.back().isUpVal;

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

    void hookIdent(const string& name, SpecTarget* hookedTarget, bool isDecl) {
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

                putPatch(hookedTarget, scope.function, i, *up, isDecl, (i == 0) ? nullptr : stack[stack.size()-i].function);
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
        hookIdent(it.data.value, &it.data.hookedTarget, false);
    }

    void hook(ASTExpr& tgt) {
        tgt->visit(*this);
    }

    void hook(ASTStm& tgt) {
        tgt->visit(*this);
    }

    void putPatch(SpecTarget* toPatch1, Function* source, size_t relFrameId, size_t relLocalId, bool isDecl, Function* up) {
        hookList.emplace_back(toPatch1, source, relFrameId, relLocalId, isDecl, up);
    }

    void invoke(VariableDeclaration& it) override {
        if (VERBOSE) println("VISITING VariableDeclaratio {}", it.data.dst);

        if (it.data.value.has_value()) {
            hook(*it.data.value);
        }

        putIdent(it.data.dst);
        hookIdent(it.data.dst, &it.data.hookedTarget, true);
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
        hookIdent(it.data.name, &it.hookedTarget, true);

        stack.emplace_back();
        stack.back().lexicals.emplace_back();
        stack.back().function = &it;

        for (auto& n : it.data.argz) {
            stack.back().putLocal(n);
        }

        for (auto& idk : it.data.body) {
            hook(idk);
        }

        it.locals = stack.back().isUpVal;
        stack.pop_back();
    }

    void invoke(Assign& it) override {
        hook(it.data.value);
        auto dst = it.data.dst/*.get()*/;

        if (dynamic_cast<Identifier*>(dst) != nullptr) {
            auto& ident = dynamic_cast<Identifier*>(dst)->data;
            auto name = ident.value;
            if (VERBOSE) println("VISITING assing {}", name);
            hookIdent(name, &ident.hookedTarget, false);
        } else if (dynamic_cast<FieldAccess*>(dst) != nullptr) {
            hook(it.data.dst);
        } else {
            PANIC("CANT ASSING TO STUFF");
        }
    }

    void invoke(BoolLiteral1& it) override {}


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
        auto idk = (FunctionRef*)malloc(sizeof(FunctionRef)+(f.totalUpValCount()*sizeof(LoxValue*)));
        idk->func = &f;

        std::memset(idk->captures, 0, f.totalUpValCount()*sizeof(LoxValue*));

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
        assert(it.hookedTarget.get() != nullptr);
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
            auto v = getSpecVar(it.hookedTarget);
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

    ObjectRef* rawInstant(ClassRef* clazz, map<string, LoxValue>* data) {
        ObjectRef* proto = nullptr;
        if (clazz->super != nullptr) {
            proto = rawInstant(clazz->super, data);
        }
        auto me = new ObjectRef{clazz, proto, data};
        for (auto& c : clazz->clazz->data.methods) {
            auto f = allocateFunctionRef(*c);
            f->func = c/*.get()*/;
            f->parent = clazz->parent;
            for (auto i = 0UL; i < c->captures.size(); i++) {
                f->captures[c->upValCount()+i] = getUpVal(c->captures[i]);
            }
            f->captures[c->getParamUpValOffset()] = new LoxValue(LoxValue::Object(me));
            f->captures[c->getParamUpValOffset() + 1] = new LoxValue(
                    proto == nullptr ? LoxValue::Nil() : LoxValue::Object(proto));
            me->methods.push_back(f);
        }

        return me;
    }

    void instantiate(ClassRef* clazz, span<ASTExpr> argz) {
        auto constructor = clazz->getConstructor();
        if (constructor == nullptr && not argz.empty()) PANIC();
        if (constructor != nullptr && constructor->data.argz.size() != argz.size()) PANIC();

        auto* res = rawInstant(clazz, new map<string, LoxValue>{});

        if (constructor != nullptr) {
            auto f = res->getMethod("init");
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

        if (shouldReturn) {
            shouldReturn = false;
        } else {
            push(LoxValue::Nil());
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
        assert(subj.isObject());
        push(subj.asObject()->read(it.data.fieldName));
    }

    void invoke(Identifier& it) override {
        // TODO();
        assert(it.data.hookedTarget != nullptr);
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
        assert(it.data.hookedTarget.get() != nullptr);
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

        LoxValue res;
        switch (it.data.type) {
            case BinaryType::ADD:
                if (rhs.isString()) {
                    assert(lhs.isString());
                    auto idk = string{};
                    idk += lhs.asString();
                    idk += rhs.asString();
                    res = LoxValue::String(std::move(idk));
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
                // FIXME
                res = LoxValue::Number((double) ((long) lhs.asNumber() % (long) rhs.asNumber()));
                break;
            case BinaryType::REM:
            TODO();
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
                if (not rhs.matchesType(lhs)) {
                    res = LoxValue::False();
                } else if (rhs.isNumber()) {
                    res = LoxValue::Bool(rhs.asNumber() == lhs.asNumber());
                } else if (rhs.isFunction()) {
                    res = LoxValue::Bool(rhs.asFunction() == lhs.asFunction());
                } else if (rhs.isString()) {
                    res = LoxValue::Bool(lhs.asString() == rhs.asString());
                } else if (rhs.isBool()) {
                    res = LoxValue::Bool(lhs.asBool() == rhs.asBool());
                } else if (rhs.isNil()) {
                    res = LoxValue::True();
                } else if (rhs.isClass()) {
                    res = LoxValue::Bool(lhs.asClass() == rhs.asClass());
                } else {
                    TODO();
                }

                if (it.data.type == BinaryType::NEQ) {
                    res = LoxValue::Bool(!res.asBool());
                }
                break;
            }
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

        push(res);
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
            assert(obj.isObject());
            auto v = pop();
            (*obj.asObject()->fields)[dynamic_cast<FieldAccess*>(dst)->data.fieldName] = v;
            push(v); // FIXME this is retarded
        } else {
            TODO();
        }
    }

    void invoke(StringLiteral1& it) override {
        string_view v(it.data.value.data()+1, it.data.value.size()-2);
        push(LoxValue::String(v));
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

int main(int argc, const char** argv) {
    string filePath{argv[1]};
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

    Linerizer linerizer;
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


    globalFunc->locals = linerizer.stack.back().isUpVal;

    auto start1 = std::chrono::high_resolution_clock::now();

    linerizer.realFix();
    // return 3;

    ASTExecutor executor;

    executor.currentFrame = executor.allocateFunctionRef(*globalFunc);
    executor.stackBase -= globalFunc->locals.size()-globalFunc->totalUpValCount();

    auto clock = makeStuff2<Function>(clk, vector<string>{}, vector<ASTStm>{});
    clock->native = [&](ASTExecutor& ctx) {
        ctx.push(LoxValue::Number(duration_cast<std::chrono::microseconds>((std::chrono::high_resolution_clock::now()-start1)).count()/1'000'000.0));
    };
    executor.globals[clockId] = LoxValue::Function(new FunctionRef{nullptr, clock/*.get()*/});
    // executor.frames.push_back(new StackFrame());

    size_t ip = 0;
    while (ip < pepa.buffer.size()) {
        pepa.buffer[ip]->visit(executor);
        ip += 1;
    }
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
// Dockerfile to build interpreter
// FUCKING JIT IT
// merge requests for checking stuff...