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

    virtual void invoke(SpecializedVariable& it) {}

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
    unique_ptr<Expression> fName;
    vector<unique_ptr<Expression>> args;
})
};

struct Decl: ASTNode {

};

struct ASTExecutor;

struct Function: Statement {
    STRUCT_BEGIN(Function)
    string name;
    vector<string> argz;
    vector<unique_ptr<Statement>> body;
    STRUCT_END(Function)

    vector<bool> locals;
    vector<size_t> captures; // list of parent function up-local-ids that we capture
    // size_t upValCount = 0;
    unique_ptr<SpecializedVariable> hookedTarget = nullptr;
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
    vector<unique_ptr<Function>> methods;
    STRUCT_END(Class)

    unique_ptr<SpecializedVariable> hookedTarget = nullptr;
    unique_ptr<SpecializedVariable> hookedDst = nullptr;

    Function* getMethod(string_view name) {
        for (auto& f : data.methods) {
            if (f->data.name == name) return f.get();
        }
        return nullptr;
    }
};

struct This: Expression {
    STRUCT_BEGIN(This)
    STRUCT_END(This)
    unique_ptr<SpecializedVariable> hookedTarget;
};

struct Super: Expression {
    STRUCT_BEGIN(Super)
    STRUCT_END(Super)
    unique_ptr<SpecializedVariable> hookedTarget;
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
    unique_ptr<Expression> lhs;
    unique_ptr<Expression> rhs;
    STRUCT_END(Binary)
};

struct IF: Statement {
    STRUCT_BEGIN(IF)
    unique_ptr<Expression> cond;
    unique_ptr<Statement> ifBody;
    optional<unique_ptr<Statement>> elsBody;
    STRUCT_END(IF)
};

struct StatmentExpr: Statement {
    STRUCT_BEGIN(StatmentExpr)
    unique_ptr<Expression> inner;
    STRUCT_END(StatmentExpr)
};

struct Identifier: Expression {
    STRUCT_BEGIN(Identifier)
    string value;
    unique_ptr<SpecializedVariable> hookedTarget = nullptr;
    STRUCT_END(Identifier)
};

struct VariableDeclaration: Statement {
    STRUCT_BEGIN(VariableDeclaration)
    string dst;
    optional<unique_ptr<Expression>> value;
    unique_ptr<SpecializedVariable> hookedTarget = nullptr;
    STRUCT_END(VariableDeclaration)
};

struct Return: Statement {
    STRUCT_BEGIN(Return)
    optional<unique_ptr<Expression>> value;
    STRUCT_END(Return)
};

struct Print: Statement {
    STRUCT_BEGIN(Print)
    unique_ptr<Expression> value;
    STRUCT_END(Print)
};

struct Block: Statement {
    STRUCT_BEGIN(Block)
    vector<unique_ptr<Statement>> statements;
    STRUCT_END(Block)
};

struct While: Statement {
    STRUCT_BEGIN(While)
    optional<unique_ptr<Expression>> cond;
    unique_ptr<Statement> body;
    STRUCT_END(While)
};

/*struct For: Statement {
    STRUCT_BEGIN(For)
    unique_ptr<Statement> setup;
    unique_ptr<Expression> cond;
    unique_ptr<Statement> post;
    Block statements;
    STRUCT_END(For)
};*/

struct Assign: Expression {
    STRUCT_BEGIN(Assign)
    unique_ptr<Expression> dst;
    unique_ptr<Expression> value;
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
    unique_ptr<Expression> subject;
    string fieldName;
    STRUCT_END(FieldAccess)
};

struct Negate: Expression {
    STRUCT_BEGIN(Negate)
        unique_ptr<Expression> inner;
    STRUCT_END(Negate)
};

struct NilLiteral: Expression {
    STRUCT_BEGIN(NilLiteral)
    STRUCT_END(NilLiteral)
};

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

typedef ParsingUnit<TokenType1, unique_ptr<ASTNode>, MilaState, MilaParseError> MilaParsingUnit;

template<typename T>
using MilaResult = std::expected<T, MilaParseError>;

template<>
class BaseParser<TokenType1, unique_ptr<ASTNode>, MilaState, MilaParseError> : public Parser<TokenType1, unique_ptr<ASTNode>, MilaState, MilaParseError> {
public:
    using Parser::Parser;

    MilaResult<unique_ptr<Expression>> parseExpression() {
        auto stuff = TRY(parseSomethingTerminator(TokenType1::Semicolon)).release();
        auto csted = dynamic_cast<Expression*>(stuff);
        if (csted == nullptr) {
            delete stuff;
            return unexpected{NotAExpr{}};
        }

        return unique_ptr<Expression>(csted);
    }

    MilaResult<unique_ptr<Statement>> parseStatement() {
        auto stuff = TRY(parseSomethingTerminator(TokenType1::Semicolon)).release();
        auto csted = dynamic_cast<Statement*>(stuff);
        if (csted == nullptr) {
            auto csted1 = dynamic_cast<Expression*>(stuff);
            if (csted1 != nullptr) {
                return makeStuff<StatmentExpr>(unique_ptr<Expression>(csted1));
            }
            delete stuff;
            return unexpected{NotAStatement{}};
        }

        return unique_ptr<Statement>(csted);
    }

    MilaResult<vector<unique_ptr<Statement>>> parseBody() {
        vector<unique_ptr<Statement>> body;

        TRY(getAssert(TokenType1::OCB));

        while (!isPeekTypeConsume(TokenType1::CCB)) {
            body.push_back(TRY(parseStatement()));
        }

        return body;
    }

    MilaResult<string> assertIdent() {
        return string(TRY(this->getAssert(TokenType1::Idntifier)).content);
    }

    MilaResult<vector<unique_ptr<Statement>>> parseStatementsUntil(initializer_list<TokenType1> toks) {
        vector<unique_ptr<Statement>> body;

        while (!isPeekTypeOneOf(toks)) {
            body.push_back(TRY(parseStatement()));
        }

        TRY(this->consumeToken());

        return body;
    }

    template<typename T>
    bool isPrev() const {
        return isPeekStack([&](auto& it) { return dynamic_cast<const T*>(it.get()); });
    }

    bool isPrevExp() const {
        return isPrev<Expression>();
    }

    MilaResult<unique_ptr<Expression>> popExpr() {
        if (not isPrevExp()) return unexpected{NotAExpr{}};

        auto preExpr = prevPop();
        auto* ptr = preExpr->release();
        return unique_ptr<Expression>(dynamic_cast<Expression*>(ptr));
    }

    void pushExp(unique_ptr<Expression> exp) {
        this->buffer.push_back(std::move(exp));
    }
};

typedef BaseParser<TokenType1, unique_ptr<ASTNode>, MilaState, MilaParseError> MilaParser;

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

    std::expected<unique_ptr<ASTNode>, MilaParseError> parse(MilaParser& parser) const override {
        assertToken(TokenType1::Function);

        auto name = TRY(parser.assertIdent());


        TRY(parser.getAssert(TokenType1::ORB));

        auto params = TRY(parser.parseManyWithSeparatorUntil3(TokenType1::Comma, TokenType1::CRB,[&] {
            return parser.assertIdent();
        }));

        TRY(parser.getAssert(TokenType1::CRB));

        auto body = TRY(parser.parseBody());

        return makeStuff<Function>(string(name), std::move(params), std::move(body));
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

    MilaResult<unique_ptr<ASTNode>> parse(MilaParser& parser) const override {
        assertToken(TokenType1::If);

        auto cond = TRY(parser.parseExpression());

        auto ifBody  = TRY(parser.parseStatement());
        optional<unique_ptr<Statement>> elsBody;

        if (parser.isPeekTypeConsume(TokenType1::Else)) {
            elsBody = TRY(parser.parseStatement());
        }

        return makeStuff<IF>(std::move(cond), std::move(ifBody), std::move(elsBody));
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

    MilaResult<unique_ptr<ASTNode>> parse(MilaParser& parser) const override {
        auto subject = TRY(parser.popExpr());

        assertToken(TokenType1::ORB);

        auto argz = TRY(parser.parseManyWithSeparatorUntil3(TokenType1::Comma, TokenType1::CRB, [&] {
           return parser.parseExpression();
        }));

        assertToken(TokenType1::CRB);

        return makeStuff<Call>(std::move(subject), std::move(argz));
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

    MilaResult<unique_ptr<ASTNode>> parse(MilaParser& parser) const override {
        assertToken(TokenType1::While);

        auto cond = TRY(parser.parseExpression());

        auto body = TRY(parser.parseStatement());

        return makeStuff<While>(std::move(cond), std::move(body));
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

    MilaResult<unique_ptr<ASTNode>> parse(MilaParser& parser) const override {
        assertToken(TokenType1::For);

        assertToken(TokenType1::ORB);

        optional<unique_ptr<Statement>> start;
        if (not parser.isPeekTypeConsume(TokenType1::Semicolon)) {
            start = TRY(parser.parseStatement());
        }

        optional<unique_ptr<Expression>> cond;
        if (not parser.isPeekTypeConsume(TokenType1::Semicolon)) {
            cond = TRY(parser.parseExpression());
        }

        optional<unique_ptr<Statement>> update;
        if (not parser.isPeekTypeConsume(TokenType1::CRB)) {
            update = TRY(parser.parseStatement());
            assertToken(TokenType1::CRB);
        }

        vector<unique_ptr<Statement>> body;
        body.push_back(TRY(parser.parseStatement()));
        if (update.has_value()) {
            body.push_back(std::move(*update));
        }

        auto whajl = makeStuff<While>(std::move(cond), makeStuff<Block>(std::move(body)));

        auto block = makeStuff<Block>();
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

    MilaResult<unique_ptr<ASTNode>> parse(MilaParser& parser) const override {
        auto tgt = TRY(parser.popExpr());

        assertToken(TokenType1::Assign);

        auto value = TRY(parser.parseExpression());

        return makeStuff<Assign>(std::move(tgt), std::move(value));
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

    MilaResult<unique_ptr<ASTNode>> parse(MilaParser& parser) const override {
        assertToken(TokenType1::Var);

        auto name = TRY(parser.assertIdent());

        optional<unique_ptr<Expression>> value;
        if (parser.isPeekTypeConsume(TokenType1::Assign)) {
            value = TRY(parser.parseExpression());
        }

        return makeStuff<VariableDeclaration>(std::move(name), std::move(value));
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

    MilaResult<unique_ptr<ASTNode>> parse(MilaParser& parser) const override {
        if (parser.isPeekTypeConsume(TokenType1::This)) {
            return makeStuff<This>();
        } else {
            assertToken(TokenType1::Super);
            return makeStuff<Super>();
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

    MilaResult<unique_ptr<ASTNode>> parse(MilaParser& parser) const override {
        auto ident = TRY(parser.assertIdent());

        return makeStuff<Identifier>(std::move(ident));
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

    MilaResult<unique_ptr<ASTNode>> parse(MilaParser& parser) const override {
        auto ident = string(assertToken(TokenType1::String).content);

        return makeStuff<StringLiteral1>(std::move(ident));
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

    MilaResult<unique_ptr<ASTNode>> parse(MilaParser& parser) const override {
        bool isTrue = true;
        if (parser.isPeekTypeConsume(TokenType1::True)) {

        } else {
            isTrue = false;
            assertToken(TokenType1::False);
        }

        return makeStuff<BoolLiteral1>(isTrue);
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

    MilaResult<unique_ptr<ASTNode>> parse(MilaParser& parser) const override {
        assertToken(TokenType1::Minus);

        TRY(parser.parseBinaryArm());

        auto expr = TRY(parser.popExpr());

        if (auto v = dynamic_cast<IntLiteral*>(expr.get()); v) {
            return makeStuff<IntLiteral>(-v->data.value);
        }

        return makeStuff<Binary>(BinaryType::SUB, makeStuff<IntLiteral>(0.0), std::move(expr));
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

    MilaResult<unique_ptr<ASTNode>> parse(MilaParser& parser) const override {
        assertToken(TokenType1::Nil);

        return makeStuff<NilLiteral>();
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

    MilaResult<unique_ptr<ASTNode>> parse(MilaParser& parser) const override {
        auto ident = TRY(parser.getAssert(TokenType1::NumberLiteral)).content;

        // FIXME mby handle error?
        double idk = std::strtod(ident.begin(), nullptr);

        return makeStuff<IntLiteral>(idk);
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

    MilaResult<unique_ptr<ASTNode>> parse(MilaParser& parser) const override {
        assertToken(TokenType1::Class);

        auto className = TRY(parser.assertIdent());

        optional<string> superName;
        if (parser.isPeekTypeConsume(TokenType1::OPB)) {
            superName = TRY(parser.assertIdent());
        }

        assertToken(TokenType1::OCB);

        vector<unique_ptr<Function>> methods;

        while (!parser.isPeekTypeConsume(TokenType1::CCB)) {
            auto methodName = TRY(parser.assertIdent());

            assertToken(TokenType1::ORB);
            auto params = TRY(parser.parseManyWithSeparatorUntil3(TokenType1::Comma, TokenType1::CRB, [&] {
               return parser.assertIdent();
            }));

            assertToken(TokenType1::CRB);

            auto body = TRY(parser.parseBody());

            methods.push_back(makeStuff<Function>(std::move(methodName), std::move(params), std::move(body)));
        }

        return makeStuff<Class>(std::move(className), std::move(superName), std::move(methods));
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

    MilaResult<unique_ptr<ASTNode>> parse(MilaParser& parser) const override {
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

    MilaResult<unique_ptr<ASTNode>> parse(MilaParser& parser) const override {
        assertToken(TokenType1::Return);

        optional<unique_ptr<Expression>> inner;
        if (parser.isPeekType(TokenType1::Semicolon)) {
        } else {
            inner = TRY(parser.parseExpression());
        }

        return makeStuff<Return>(std::move(inner));
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

    MilaResult<unique_ptr<ASTNode>> parse(MilaParser& parser) const override {
        assertToken(TokenType1::Print);

        auto inner = TRY(parser.parseExpression());

        return makeStuff<Print>(std::move(inner));
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

    MilaResult<unique_ptr<ASTNode>> parse(MilaParser& parser) const override {
        auto block = TRY(parser.parseBody());

        return makeStuff<Block>(std::move(block));
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

    MilaResult<unique_ptr<ASTNode>> parse(MilaParser& parser) const override {
        auto lhs = TRY(parser.popExpr());

        auto tok = toType(TRY(parser.consumeToken()).type);
        assert(tok.has_value());

        TRY(parser.parseBinaryArm());
        auto rhs = TRY(parser.popExpr());

        // is there any other binary?
        if (not parser.hasToken() || not toType(parser.getToken()->type).has_value()) {
            return makeStuff<Binary>(tok->first, std::move(lhs), std::move(rhs));
        }
        // yes

        auto typ1 = toType(parser.getToken()->type);

        if (typ1->second > tok->second) {
            parser.pushExp(std::move(rhs));
            TRY(parser.parseOne(LookDirection::Around));
            auto newRhs = TRY(parser.popExpr());

            return makeStuff<Binary>(tok->first, std::move(lhs), std::move(newRhs));
        } else {
            parser.pushExp(makeStuff<Binary>(tok->first, std::move(lhs), std::move(rhs)));
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

    MilaResult<unique_ptr<ASTNode>> parse(MilaParser& parser) const override {
        auto subject = TRY(parser.popExpr());

        assertToken(TokenType1::Dot);

        auto fieldName = TRY(parser.assertIdent());

        return makeStuff<FieldAccess>(std::move(subject), std::move(fieldName));
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

    MilaResult<unique_ptr<ASTNode>> parse(MilaParser& parser) const override {
        assertToken(TokenType1::Negate);

        auto inner = TRY(parser.parseExpression());

        return makeStuff<Negate>(std::move(inner));
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

enum ValueType1 {
    FLOAT,
    FUNCTION_REF,
    NIL,
    BOOL,
    STRING,
    CLASS,
    INSTANCE
};

struct LoxValue {
    ValueType1 v;

   union {
       double flot;
       FunctionRef* ref;
       ClassRef* classRef;
       ObjectRef* objectRef;
       bool buul;
       string* str;
   };

   bool isFloat() const {
       return v == FLOAT;
   }

    bool isString() const {
        return v == STRING;
    }

    bool isNil() const {
        return v == NIL;
    }


    string toString() const {
        switch (v) {
            /*case NUMBER:
                return to_string(number);*/
            case FLOAT: {
                char pepa[16];
                snprintf(pepa, 16, "%G", flot);
                return {pepa};
          /*      double intpart;
                if (modf(flot, &intpart) == 0.0) {
                    return to_string((long long int)intpart);
                } else {
                    return to_string(flot);
                }*/
            }
            case FUNCTION_REF:
                return (ref->func->native) ? "<native fn>" : stringify("<fn {}>", ref->func->data.name);
            case NIL:
                return "nil";
            case BOOL:
                return buul ? "true" : "false";
            case STRING:
                return *str;
            case CLASS:
                return classRef->clazz->data.name;
                break;
            case INSTANCE:
                return stringify("{} instance", (*((ClassRef**)objectRef))->clazz->data.name);
                break;
        }
        println("AAAAAAAAAAAAASDADASDASD {}", (long)v);
        UNREACHABLE();
    }

    bool toBool() const {
        switch (v) {
            case FLOAT:
            case CLASS:
            case INSTANCE:
            case FUNCTION_REF:
            case STRING:
                return true;
            case NIL:
                return false;
            case BOOL:
                return buul;
        }
        PANIC()
    }

    static LoxValue Bool(bool v) {
        return LoxValue{.v=BOOL, .buul=v};
    }

    static LoxValue True() {
        return LoxValue{.v=BOOL, .buul=true};
    }

    static LoxValue False() {
        return LoxValue{.v=BOOL, .buul=false};
    }

    static LoxValue Nil() {
        return LoxValue{.v=NIL};
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
            if (m->func->data.name == name) return LoxValue{.v=FUNCTION_REF, .ref=m};
        }
        if (proto != nullptr) return proto->getMethod(name);
        PANIC();
    }

    LoxValue read(const string& name) {
        if (fields->contains(name)) return fields->at(name);
        return getMethod(name);
    }
};

enum class HookedVariableType {
    LOCAL,
    UPVAL,
    GLOBAL,
    ALLOC_UPVAL
};

struct SpecializedVariable: Expression {
    HookedVariableType type;
    size_t id;
    size_t frameId;

    SpecializedVariable(HookedVariableType type, size_t id, size_t frameId): type(type), id(id), frameId(frameId) {
    }

    void visit(ASTVisitor& it) override {
        it.invoke(*this);
    }
};

struct StackFrame {
    Function* function = nullptr;
    StackFrame* parent = nullptr;
    map<string, LoxValue> values;

    optional<LoxValue*> getVar(const string& s) {
        if (values.contains(s)) {
            return &values.at(s);
        } else if (parent != nullptr) {
            return parent->getVar(s);
        } else {
            return {};
        }
    }

    LoxValue getVarVal(const string& s) {
        auto v = getVar(s);
        if (not v.has_value()) return LoxValue{NIL, 0};

        return **v;
    }

    void setVar(const string& name, LoxValue value) {
        auto tgt = getVar(name);
        assert(tgt.has_value());
        **tgt = value;
    }

    void putVar(const string& name, LoxValue value) {
        assert(not values.contains(name));

        values[name] = value;
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
        unique_ptr<SpecializedVariable>* toPatch;
        Function* source;
        size_t relFrameId;
        size_t relLocalId;
        bool isDecl;
        Function* sourceUpFunction;

        bool isEscaped() {
            return source->locals[relLocalId];
        }
    };

    // map<Function*, vector<bool>> globalScope; // locals layout of every function
    // map<ASTNode*, tuple<size_t, size_t, Function*>> toPatch; // ast nodes that need to be patched
    // map<ASTNode*, unique_ptr<SpecializedVariable>*> resolvedSlot; // addresses of nodes to be patched
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

        // map<ASTNode*, size_t> variableFixups;
        // map<ASTNode*, unique_ptr<Expression>*> tier2fixup;
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
                    *f.toPatch = make_unique<SpecializedVariable>(HookedVariableType::ALLOC_UPVAL, f.source->getUpValId(f.relLocalId), 0);
                } else if (isEscaped) { // access to localy defined upval
                    *f.toPatch = make_unique<SpecializedVariable>(HookedVariableType::UPVAL, self->getUpValId(f.relLocalId), 0);
                } else { // access to ordinary local
                    *f.toPatch = make_unique<SpecializedVariable>(HookedVariableType::LOCAL, self->getLocalId(f.relLocalId), 0);
                }
            } else if (f.relFrameId == 1) { // capturing parents local
                auto self = f.sourceUpFunction;
                auto localId = self->putCapture(f.relLocalId);

                assert(not f.isDecl);
                *f.toPatch = make_unique<SpecializedVariable>(HookedVariableType::UPVAL, self->getUpValId(localId), 0); // offset 0, we copied upval to our frame
            } else { // capturing nested local
                auto closestChild = f.sourceUpFunction;
                auto someParentsId = closestChild->putCapture(f.relLocalId);

                assert(not f.isDecl);
                *f.toPatch = make_unique<SpecializedVariable>(HookedVariableType::UPVAL, closestChild->getUpValId(someParentsId), f.relFrameId-1); // relFrameId-1, we access version stored in nodes child
            }
        }

  /*      for (auto g : globalScope) {
            g.first->locals = g.second;
            g.first->upValCount = calcUpValCount(g.second);
        }*/
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
            stack.back().function = method.get();

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

    map<VariableDeclaration*, unique_ptr<Statement>> declarationsToPatch;

    void hookIdent(const string& name, unique_ptr<SpecializedVariable>* hookedTarget, bool isDecl) {
        if (VERBOSE) println("VISITING identifier {}", name);
        if (isInRealGlobal()) {
            auto g = getGlobal(name);
            if (not g.has_value()) {
                g = globals.putRootLocal(name);
            }

            *hookedTarget = make_unique<SpecializedVariable>(HookedVariableType::GLOBAL, *g, 0);

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
            *hookedTarget = make_unique<SpecializedVariable>(HookedVariableType::GLOBAL, *g, 0);
            return;
        }

        // global var hasent been declared yet i guess
        *hookedTarget = make_unique<SpecializedVariable>(HookedVariableType::GLOBAL, globals.putRootLocal(name), 0);
        // PANIC("variable {} never defined", it.data.value);
    }

    void invoke(Identifier& it) override {
        hookIdent(it.data.value, &it.data.hookedTarget, false);
    }

    void hook(unique_ptr<Expression>& tgt) {
        tgt->visit(*this);
    }

    void hook(unique_ptr<Statement>& tgt) {
        tgt->visit(*this);
    }

    void putPatch(unique_ptr<SpecializedVariable>* toPatch1, Function* source, size_t relFrameId, size_t relLocalId, bool isDecl, Function* up) {
        // this->resolvedSlot[nullptr] = toPatch1;
        // this->toPatch[nullptr] = {relFrameId, relLocalId, source};
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

/*        if (stack.empty()) {
            globals.enterScope();

            for (auto& s : it.data.statements) {
                hook(s);
            }

            globals.exitScope();
        } else {
            stack.back().enterScope();

            for (auto& s : it.data.statements) {
                hook(s);
            }

            stack.back().exitScope();
        }*/
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
/*        if (stack.empty()) {
            globals.enterScope();

            hook(it.data.cond);

            globals.enterScope();
            hook(it.data.ifBody);
            globals.exitScope();

            if (it.data.elsBody.has_value()) {
                globals.enterScope();
                hook(*it.data.elsBody);
                globals.exitScope();
            }

            globals.exitScope();
        } else {
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
        }*/
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

/*    void fixLocals() {
        if (VERBOSE) {
            for (auto c : this->globalScope) {
                println("== FUNC STACK {} {}", c.first->data.name, c.second.size());
            }

            println("== GLOBALS {}", globals.isUpVal.size());

            println("AAAAAAAAAAAAAA {} -- {} -- {} -- {}", this->globals.lexicals.size(), this->globalScope.size(), this->toPatch.size(), this->resolvedSlot.size());
        }
        assert(this->toPatch.size() == this->resolvedSlot.size());
        for (auto [key, value] : this->toPatch) {
            if (VERBOSE) println("PATCHING {}", key);
            auto varId = std::get<1>(value);
            auto frameId = std::get<0>(value);
            auto* func = std::get<2>(value);
            const auto& locals = this->globalScope[func];
            assert(globalScope.contains(func));
            assert(varId < globalScope[func].size());
            auto isUpVal = this->globalScope[func][varId];
            func->locals = locals;
            func->upValCount = calcUpValCount(locals);
            if (isUpVal) {
                *this->resolvedSlot[key] = make_unique<SpecializedVariable>(HookedVariableType::UPVAL, toUpvalId(locals, varId), frameId);
            } else {
                *this->resolvedSlot[key] = make_unique<SpecializedVariable>(HookedVariableType::LOCAL, toLocalId(locals, varId), 0);
            }
        }
    }*/

    void invoke(Assign& it) override {
        hook(it.data.value);
        auto dst = it.data.dst.get();

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

    /*
    struct Assigner: ASTVisitor {
        LoxValue value;
        ASTExecutor* executor;

        void invoke(Identifier& it) override {
            executor->getFrame()->setVar(it.data.value, value);
            executor->push(value);
        }
    };*/

    LoxValue pop() {
        // assert(not valueStack.empty());

        auto v = valueStack.back(); valueStack.pop_back();

        return v;
    }

    void push(LoxValue val) {
        // println("PUSHING!!! {}", val.toString());
        valueStack.push_back(val);
    }

    /*StackFrame* getFrame() {
        return frames.back();
    }*/

    void invoke(IntLiteral& it) override {
        push(LoxValue{ValueType1::FLOAT, it.data.value});
    }

    size_t funcId = 0;

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
        push(getSpecVar(*it.hookedTarget));
    }

    void invoke(Super &it) override {
        push(getSpecVar(*it.hookedTarget));
    }

    void invoke(Function& it) override {
        assert(it.hookedTarget.get() != nullptr);
        auto specVar = it.hookedTarget.get();
        auto* fRef = allocateFunctionRef(it);
        fRef->func = &it;
        fRef->parent = currentFrame;

        setSpecVar(*specVar, LoxValue{.v=ValueType1::FUNCTION_REF, .ref=fRef});

        for (auto i = 0UL; i < it.captures.size(); i++) {
            fRef->captures[it.upValCount()+i] = getUpVal(it.captures[i]);
        }
    }

    void invoke(Class& it) override {
        ClassRef* super = nullptr;
        if (it.data.superClass.has_value()) {
            auto v = getSpecVar(*it.hookedTarget);
            assert(v.v == CLASS);
            super = v.classRef;
        }
        auto clazz = new ClassRef{&it, super, currentFrame};
        auto loxClass = LoxValue{.v=CLASS, .classRef=clazz};

        setSpecVar(*it.hookedDst, loxClass);
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
            f->func = c.get();
            f->parent = clazz->parent;
            for (auto i = 0UL; i < c->captures.size(); i++) {
                f->captures[c->upValCount()+i] = getUpVal(c->captures[i]);
            }
            f->captures[c->getParamUpValOffset()] = new LoxValue(LoxValue{.v=INSTANCE, .objectRef=me});
            f->captures[c->getParamUpValOffset()+1] = new LoxValue(proto == nullptr ? LoxValue::Nil() : LoxValue{.v=INSTANCE, .objectRef=proto});
            me->methods.push_back(f);
        }

        return me;
    }

    void instantiate(ClassRef* clazz, span<unique_ptr<Expression>> argz) {
        auto constructor = clazz->getConstructor();
        if (constructor == nullptr && not argz.empty()) PANIC();
        if (constructor != nullptr && constructor->data.argz.size() != argz.size()) PANIC();

        auto* res = rawInstant(clazz, new map<string, LoxValue>{});

        if (constructor != nullptr) {
            auto f = res->getMethod("init");
            call(*f.ref, argz);
            pop();
        }

        push(LoxValue{.v=INSTANCE, .objectRef=res});
    }

    void call(FunctionRef& f, span<std::unique_ptr<Expression>> argz) {
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
            if(VERBOSE)println("CALLING WITH: {}", poop.toString());
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
            push(LoxValue{.v=NIL});
        }
 /*       if (f.func->data.name ==  "init")  {
            pop();
            push(currentFrame->read(currentFrame->func->upValCount()));
        }*/
        currentFrame = oldFrame;
        stackBase = oldStackBase;
    }

    void invoke(Call& it) override {
        it.fName->visit(*this);

        auto value = pop();

        if (value.v == ValueType1::CLASS) {
            instantiate(value.classRef, it.args);
            return;
        }

        if (value.v != ValueType1::FUNCTION_REF) println("AAAAAAAAAAAAAAAAAA {}", value.toString());
        assert(value.v == ValueType1::FUNCTION_REF);
        call(*value.ref, it.args);
    }

    void invoke(NilLiteral& it) override {
        push(LoxValue{.v = NIL});
    }

    void invoke(FieldAccess &it) override {
        it.data.subject->visit(*this);
        auto subj = pop();
        assert(subj.v == INSTANCE);
        push(subj.objectRef->read(it.data.fieldName));
    }

    void invoke(Identifier& it) override {
        // TODO();
        assert(it.data.hookedTarget != nullptr);
        push(getSpecVar(*it.data.hookedTarget));
        // push(getFrame()->getVarVal(it.data.value));
    }

    void invoke(Print& it) override {
        it.data.value->visit(*this);

        auto value = pop();

        cout << value.toString() << endl;
    }

    void invoke(VariableDeclaration& it) override {
        auto value = LoxValue{NIL, 0};
        if (it.data.value.has_value()) {
            (*it.data.value)->visit(*this);
            value = pop();
        }
        assert(it.data.hookedTarget.get() != nullptr);
        setSpecVar(*it.data.hookedTarget.get(), value);
        // getFrame()->putVar(it.data.dst, value);
    }

    void invoke(Return& it) override {
        if (it.data.value.has_value()) {
            (*it.data.value)->visit(*this);
        } else {
            push(LoxValue{.v=NIL});
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
                    auto* idk = new string{};
                    *idk += *lhs.str;
                    *idk += *rhs.str;
                    res = LoxValue{.v=ValueType1::STRING, .str=idk};
                } else {
                    assert(rhs.v == ValueType1::FLOAT);
                    assert(lhs.v == ValueType1::FLOAT);
                    res = LoxValue{ValueType1::FLOAT, lhs.flot+rhs.flot};
                }
                break;
            case BinaryType::SUB:
                assert(rhs.v == ValueType1::FLOAT);
            assert(lhs.v == ValueType1::FLOAT);
                res = LoxValue{ValueType1::FLOAT, lhs.flot-rhs.flot};
            break;
            case BinaryType::DIV:
                assert(rhs.v == ValueType1::FLOAT);
            assert(lhs.v == ValueType1::FLOAT);
                res = LoxValue{ValueType1::FLOAT, lhs.flot/rhs.flot};
            break;
            case BinaryType::MOD:
                assert(rhs.v == ValueType1::FLOAT);
            assert(lhs.v == ValueType1::FLOAT);
                // FIXME
                res = LoxValue{ValueType1::FLOAT, (double)((long)lhs.flot%(long)rhs.flot)};
            break;
            case BinaryType::REM:
                TODO();
                break;
            case BinaryType::MUL:
                assert(rhs.v == ValueType1::FLOAT);
            assert(lhs.v == ValueType1::FLOAT);
                res = LoxValue{ValueType1::FLOAT, lhs.flot*rhs.flot};
                break;
            case BinaryType::GT:
                assert(rhs.v == ValueType1::FLOAT);
            assert(lhs.v == ValueType1::FLOAT);
                res = LoxValue{.v=ValueType1::BOOL, .buul=lhs.flot>rhs.flot};
            break;
            case BinaryType::LESS:
                assert(rhs.v == ValueType1::FLOAT);
            assert(lhs.v == ValueType1::FLOAT);
                res = LoxValue{.v=ValueType1::BOOL, .buul=lhs.flot<rhs.flot};
            break;
            case BinaryType::EQ:
            case BinaryType::NEQ: {
                if (rhs.v != lhs.v) {
                    res = LoxValue::False();
                } else if (rhs.v == ValueType1::FLOAT) {
                    assert(lhs.v == ValueType1::FLOAT);
                    res = LoxValue{.v = BOOL, .buul = rhs.flot == lhs.flot};
                } else if (rhs.v == ValueType1::FUNCTION_REF) {
                    res = LoxValue{.v=BOOL, .buul = rhs.ref == lhs.ref};
                } else if (rhs.v == ValueType1::STRING) {
                    res = LoxValue::Bool(*lhs.str == *rhs.str);
                } else if (rhs.v == ValueType1::BOOL) {
                    res = LoxValue::Bool(lhs.buul == rhs.buul);
                } else if (rhs.v == ValueType1::NIL) {
                    res = LoxValue::True();
                } else if (rhs.v == ValueType1::CLASS) {
                    res = LoxValue::Bool(lhs.classRef == rhs.classRef);
                } else {
                    TODO();
                }

                if (it.data.type == BinaryType::NEQ) {
                    res = LoxValue::Bool(!res.buul);
                }
                break;
            }
            break;
            case BinaryType::GEQ:
                assert(rhs.v == ValueType1::FLOAT);
                assert(lhs.v == ValueType1::FLOAT);
                res = LoxValue{.v=ValueType1::BOOL, .buul=lhs.flot>=rhs.flot};
                break;
            case BinaryType::LEQ:
                assert(rhs.v == ValueType1::FLOAT);
                assert(lhs.v == ValueType1::FLOAT);
                res = LoxValue{.v=ValueType1::BOOL, .buul=lhs.flot<=rhs.flot};
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

        auto* dst = it.data.dst.get();

        if (dynamic_cast<Identifier*>(dst) != nullptr) {
            auto v = pop();
            setSpecVar(*dynamic_cast<Identifier*>(dst)->data.hookedTarget, v);
            push(v); // FIXME this is retarded
        } else if (dynamic_cast<FieldAccess*>(dst) != nullptr) {
            dynamic_cast<FieldAccess*>(dst)->data.subject->visit(*this);
            auto obj = pop();
            assert(obj.v == ValueType1::INSTANCE);
            auto v = pop();
            (*obj.objectRef->fields)[dynamic_cast<FieldAccess*>(dst)->data.fieldName] = v;
            push(v); // FIXME this is retarded
        } else {
            TODO();
        }
    }

    void invoke(StringLiteral1& it) override {
        string_view v(it.data.value.data()+1, it.data.value.size()-2);
        push(LoxValue{.v=ValueType1::STRING, .str=new string(v)});
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
        push(LoxValue{.v=ValueType1::BOOL, .buul=it.data.value});
    }

    void invoke(SpecializedVariable& it) override {
        TODO();
    }

    void invoke(Negate& it) override {
        it.data.inner->visit(*this);
        auto v = pop();
        push(LoxValue{.v=BOOL, .buul=!v.toBool()});
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


    auto globalFunc = makeStuff<Function>("_global");

    Linerizer linerizer;
    linerizer.stack.emplace_back();
    linerizer.stack.back().function = globalFunc.get();
    linerizer.stack.back().lexicals.emplace_back();
    linerizer.globals.isGlobal = true;
    auto clk = "clock"s;
    linerizer.globals.enterScope();
    auto clockId = linerizer.globals.putLocal(clk);
    for (auto& node : pepa.buffer) {
        node->visit(linerizer);
    }


    globalFunc->locals = linerizer.stack.back().isUpVal;

    linerizer.realFix();
    // return 3;

    ASTExecutor executor;

    executor.currentFrame = executor.allocateFunctionRef(*globalFunc);
    executor.stackBase -= globalFunc->locals.size()-globalFunc->totalUpValCount();

    auto clock = makeStuff<Function>(clk, vector<string>{}, vector<unique_ptr<Statement>>{});
    clock->native = [&](ASTExecutor& ctx) {
        ctx.push(LoxValue{.v=FLOAT, .flot=0.0});
    };
    executor.globals[clockId] = LoxValue{.v=FUNCTION_REF, .ref=new FunctionRef{nullptr, clock.get()}};
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