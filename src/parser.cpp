// Michal Vlasák, FIT CTU, 2024

// This file hosts the parser for the Lox language [1], created by Robert
// Nystrom [2] for his book Crafting Interpreters [3]. The parser here, includes
// it's own lexer, and it's main goal has been to show custom memory management
// with Arena Allocation, and a very simple interface which consists of a single
// function `parse`, which returns either an `AstProgram` or a vector of errors.
//
// The lexer is a straightforward finite state machine (FSM), which recognizes
// tokens.
//
// The parses is a recursive descent parser, which uses the Pratt parsing
// technique for pasing expressions. The recursive descent part mostly just
// follows the language grammar [4]. The Pratt parser for expressions actually
// assigns "binding powers" [5] to form precedence levels, and the "null" and
// "left" denotations [6, 7].
//
// The code makes use of X macros [8] and member function pointers [9], which
// makes it a fairly declarative, and hopefully easy to extend, even if a bit
// opaque magical.
//
// See `allocator.hpp` and especially documentation of `StackingArray` to some
// of the memory management techniques used.
//
// [1]: https://craftinginterpreters.com/the-lox-language.html
// [2]: https://stuffwithstuff.com/
// [3]: https://craftinginterpreters.com/
// [4]: https://craftinginterpreters.com/appendix-i.html
// [5]: https://matklad.github.io/2020/04/13/simple-but-powerful-pratt-parsing.html
// [6]: https://tdop.github.io/
// [7]: https://www.engr.mun.ca/%7Etheo/Misc/pratt_parsing.htm
// [8]: https://en.wikipedia.org/wiki/X_macro
// [10]: https://isocpp.org/wiki/faq/pointers-to-members

#include <cstddef>
#include <string>
#include <cstring>
#include <span>

#include "parser.hpp"
#include "ast.hpp"
#include "allocator.hpp"


enum LexState {
	LS_START,
	LS_IDENTIFIER,
	LS_NUMBER,
	LS_NUMBER_DECIMALS,
	LS_STRING,

	LS_LESS,
	LS_GREATER,
	LS_EQUAL,
	LS_BANG,
	LS_SLASH,
	LS_LINE_COMMENT,
};

enum Associativity {
	ASSOC_LEFT,
	ASSOC_RIGHT,
};

#define TOKENS(KW, PU, OT) \
	/* token            repr             nud + prec   led + prec + assoc */ \
	OT(NUMBER,          "a number",      primary,   0, lefterr,  99, LEFT)  \
	OT(IDENTIFIER,      "an identifier", ident,     0, lefterr,  99, LEFT)  \
	OT(STRING,          "a string",      primary,   0, lefterr,  99, LEFT)  \
	                                                                        \
	PU(PLUS,            "+",             nullerr,   0, binop,     6, LEFT)  \
	PU(MINUS,           "-",             unop,      8, binop,     6, LEFT)  \
	PU(LESS,            "<",             nullerr,   0, binop,     5, LEFT)  \
	PU(LESS_EQUAL,      "<=",            nullerr,   0, binop,     5, LEFT)  \
	PU(GREATER,         ">",             nullerr,   0, binop,     5, LEFT)  \
	PU(GREATER_EQUAL,   ">=",            nullerr,   0, binop,     5, LEFT)  \
	PU(EQUAL,           "=",             nullerr,   0, assign,    1, RIGHT) \
	PU(EQUAL_EQUAL,     "==",            nullerr,   0, binop,     4, LEFT)  \
	PU(BANG,            "!",             unop,      8, lefterr,  99, LEFT)  \
	PU(BANG_EQUAL,      "!=",            nullerr,   0, binop,     4, LEFT)  \
	PU(DOT,             ".",             nullerr,   0, field,     9, LEFT)  \
	PU(SLASH,           "/",             nullerr,   0, binop,     7, LEFT)  \
	PU(ASTERISK,        "*",             nullerr,   0, binop,     7, LEFT)  \
	                                                                        \
	PU(LPAREN,          "(",             paren,     0, call,      9, LEFT)  \
	PU(RPAREN,          ")",             nullerr,   0, stop,      0, LEFT)  \
	PU(LBRACE,          "{",             nullerr,   0, lefterr,  99, LEFT)  \
	PU(RBRACE,          "}",             nullerr,   0, lefterr,  99, LEFT)  \
	PU(SEMICOLON,       ";",             nullerr,   0, stop,      0, LEFT)  \
	PU(COMMA,           ",",             nullerr,   0, stop,      0, LEFT)  \
	                                                                        \
	KW(AND,             "and",           nullerr,   0, logop,     3, LEFT)  \
	KW(CLASS,           "class",         nullerr,   0, lefterr,  99, LEFT)  \
	KW(ELSE,            "else",          nullerr,   0, lefterr,  99, LEFT)  \
	KW(FALSE,           "false",         primary,   0, lefterr,  99, LEFT)  \
	KW(FOR,             "for",           nullerr,   0, lefterr,  99, LEFT)  \
	KW(FUN,             "fun",           nullerr,   0, lefterr,  99, LEFT)  \
	KW(IF,              "if",            nullerr,   0, lefterr,  99, LEFT)  \
	KW(NIL,             "nil",           primary,   0, lefterr,  99, LEFT)  \
	KW(OR,              "or",            nullerr,   0, logop,     2, LEFT)  \
	KW(PRINT,           "print",         nullerr,   0, lefterr,  99, LEFT)  \
	KW(RETURN,          "return",        nullerr,   0, lefterr,  99, LEFT)  \
	KW(SUPER,           "super",         super,     0, lefterr,  99, LEFT)  \
	KW(THIS,            "this",          primary,   0, lefterr,  99, LEFT)  \
	KW(TRUE,            "true",          primary,   0, lefterr,  99, LEFT)  \
	KW(VAR,             "var",           nullerr,   0, lefterr,  99, LEFT)  \
	KW(WHILE,           "while",         nullerr,   0, lefterr,  99, LEFT)  \
	                                                                        \
	OT(EOF,             "end",           nullerr,   0, lefterr,   0, LEFT)  \
	OT(ERROR,           "lex error",     nullerr,   0, lefterr,  99, LEFT)

enum TokenKind {
	#define TOK_ENUM(tok, ...) TK_##tok,
	TOKENS(TOK_ENUM, TOK_ENUM, TOK_ENUM)
	#undef TOK_ENUM
};

static struct {
	const char *str;
	size_t len;
	TokenKind tok;
} keywords[] = {
	#define TOK_KW(tok, str, ...) { str, std::char_traits<char>::length(str), TK_##tok },
	#define TOK_OTHER(tok, str, ...)
	TOKENS(TOK_KW, TOK_OTHER, TOK_OTHER)
	#undef TOK_KW
	#undef TOK_OTHER
};

struct Token {
	TokenKind kind;
	std::string_view str;
	Location loc;
};


class Lexer {
public:
	Lexer() = delete;
	explicit Lexer(std::string_view source);

	void next(Token& token);

private:
	const char *pos;
	const char *end;
	const char *line_start;
	size_t line_num;
};


Lexer::Lexer(std::string_view source) :
	pos(source.data()),
	end(source.data() + source.length()),
	line_start(source.data()),
	line_num(1)
{
}

#define ALPHA '_': \
	case 'a': case 'b': case 'c': case 'd': case 'e': case 'f': \
	case 'g': case 'h': case 'i': case 'j': case 'k': case 'l': \
	case 'm': case 'n': case 'o': case 'p': case 'q': case 'r': \
	case 's': case 't': case 'v': case 'u': case 'w': case 'x': \
	case 'y': case 'z': case 'A': case 'B': case 'C': case 'D': \
	case 'E': case 'F': case 'G': case 'H': case 'I': case 'J': \
	case 'K': case 'L': case 'M': case 'N': case 'O': case 'P': \
	case 'Q': case 'R': case 'S': case 'T': case 'V': case 'U': \
	case 'W': case 'X': case 'Y': case 'Z'

#define DIGIT '0': \
	case '1': case '2': case '3': case '4': case '5': case '6': case '7': \
	case '8': case '9'

void
Lexer::next(Token& token)
{
	LexState state = LS_START;
	TokenKind tok = TK_ERROR;
	int end_offset = 0;
	const char *start = pos;
	const char *token_line_start = line_start;
	while (pos != end) {
		char c = *pos;
		switch (state) {
		case LS_START: switch (c) {
			case ' ': case '\t': case '\r': start += 1; break;
			case '\n': token_line_start = line_start = start += 1; line_num += 1; break;
			case ALPHA: state = LS_IDENTIFIER; break;
			case DIGIT: state = LS_NUMBER; break;
			case '"': state = LS_STRING; start += 1; break;
			case '<': state = LS_LESS; break;
			case '>': state = LS_GREATER; break;
			case '=': state = LS_EQUAL; break;
			case '!': state = LS_BANG; break;
			case '/': state = LS_SLASH; break;
			case '+': tok = TK_PLUS; goto done;
			case '-': tok = TK_MINUS; goto done;
			case '(': tok = TK_LPAREN; goto done;
			case ')': tok = TK_RPAREN; goto done;
			case '{': tok = TK_LBRACE; goto done;
			case '}': tok = TK_RBRACE; goto done;
			case ';': tok = TK_SEMICOLON; goto done;
			case '.': tok = TK_DOT; goto done;
			case ',': tok = TK_COMMA; goto done;
			case '*': tok = TK_ASTERISK; goto done;
			default:  tok = TK_ERROR; goto done;
		} break;
		case LS_IDENTIFIER: switch (c) {
			case ALPHA: case DIGIT: break;
			default: tok = TK_IDENTIFIER; goto prev_done;
		} break;
		case LS_NUMBER: switch (c) {
			case DIGIT: break;
			case '.': state = LS_NUMBER_DECIMALS; break;
			default: tok = TK_NUMBER; goto prev_done;
		} break;
		case LS_NUMBER_DECIMALS: switch (c) {
			case DIGIT: break;
			default: tok = TK_NUMBER; goto prev_done;
		} break;
		case LS_STRING: switch (c) {
			case '"': tok = TK_STRING; end_offset = -1; goto done;
			case '\n': line_start = pos + 1; line_num += 1; break;
		} break;
		case LS_LESS: switch(c) {
			case '=': tok = TK_LESS_EQUAL; goto done;
			default: tok = TK_LESS; goto prev_done;
		} break;
		case LS_GREATER: switch(c) {
			case '=': tok = TK_GREATER_EQUAL; goto done;
			default: tok = TK_GREATER; goto prev_done;
		} break;
		case LS_EQUAL: switch (c) {
			case '=': tok = TK_EQUAL_EQUAL; goto done;
			default: tok = TK_EQUAL; goto prev_done;
		} break;
		case LS_BANG: switch (c) {
			case '=': tok = TK_BANG_EQUAL; goto done;
			default: tok = TK_BANG; goto prev_done;
		} break;
		case LS_SLASH: switch (c) {
			case '/': state = LS_LINE_COMMENT; start += 2; break;
			default: tok = TK_SLASH; goto prev_done;
		} break;
		case LS_LINE_COMMENT: switch (c) {
			// Also works with CR LF
			case '\n': state = LS_START; token_line_start = line_start = start = pos + 1; line_num += 1; break;
		} break;
		}

		pos += 1;
	}

	switch (state) {
		case LS_START: tok = TK_EOF; goto prev_done;
		case LS_IDENTIFIER: tok = TK_IDENTIFIER; goto prev_done;
		case LS_NUMBER: tok = TK_NUMBER; goto prev_done;
		case LS_NUMBER_DECIMALS: tok = TK_NUMBER; goto prev_done;
		case LS_STRING: goto err;
		case LS_LESS: tok = TK_LESS; goto prev_done;
		case LS_GREATER: tok = TK_GREATER; goto prev_done;
		case LS_EQUAL: tok = TK_EQUAL; goto prev_done;
		case LS_BANG: tok = TK_BANG; goto prev_done;
		case LS_SLASH: tok = TK_SLASH; goto prev_done;
		case LS_LINE_COMMENT: tok = TK_EOF; goto prev_done;
	}

done:
	pos += 1;
prev_done:
err:
	size_t length = pos - start + end_offset;
	size_t line = line_num;
	size_t col = start - token_line_start + 1;
	if (tok == TK_IDENTIFIER) {
		for (auto & keyword : keywords) {
			if (keyword.len == length && memcmp(start, keyword.str, length) == 0) {
				tok = keyword.tok;
				break;
			}
		}
	}
	token.kind = tok;
	token.str = std::string_view(start, length);
	token.loc = Location { line, col };
}

class Parser {
public:
	Parser(ArenaAllocator *arena, StackingAllocator *scratch, std::string_view source) :
		arena(arena),
		scratch(scratch),
		lexer(source),
		had_error(false),
		panic_mode(false)
	{
		// Initialize with first token
		discard();
	}

private:
	ArenaAllocator *arena;
	StackingAllocator *scratch;
	Lexer lexer;
	Token lookahead;
	Token prev;
public:
	std::vector<ParserError> errors;
	bool had_error;
private:
	bool panic_mode;

	template <typename T, typename... As>
	[[nodiscard]] inline T *create(Location loc, As&&... args) {
		void *mem = arena->allocate<T>();
		return new(mem) T { { { loc }, kind_of_type<T>::KIND }, std::forward<As>(args)... };
	}


	void error(Token errtok, bool panic, const char *msg) {
		if (!this->panic_mode) {
			errors.emplace_back(errtok.loc, std::string(errtok.str), msg);
			this->had_error = true;
			this->panic_mode = panic;
		}
	}

	[[nodiscard]] TokenKind peek() const {
		return this->lookahead.kind;
	}

	[[nodiscard]] Token prev_tok() const {
		return this->prev;
	}

	Token discard() {
		this->prev = this->lookahead;
		this->lexer.next(this->lookahead);
		if (this->lookahead.kind == TK_ERROR) {
			error(this->lookahead, true, "Unexpected character");
		}
		return this->prev;
	}

	Token eat(TokenKind kind, const char *msg) {
		TokenKind tok = peek();
		if (tok != kind) {
			error(this->lookahead, true, msg);
		}
		return discard();
	}

	std::string_view eat_identifier(const char *msg) {
		eat(TK_IDENTIFIER, msg);
		return get_string(prev_tok().str);
	}

	bool try_eat(TokenKind kind) {
		if (peek() == kind) {
			discard();
			return true;
		}
		return false;
	}

	std::string_view get_string(std::string_view str) {
		// We want to return only strings allocated in the arena.
		// Because then the strings will have the same life time
		// as the entire AST, and not be tied to lifetime of the input.
		std::span<char> span = arena->allocate_array<char>(str.size());
		memcpy(span.data(), str.data(), span.size() * sizeof(char));
		return {span.data(), span.size()};
	}

	Expression *expression() {
		return expression_bp(1);
	}

	std::span<Expression*> expression_list(TokenKind separator, TokenKind terminator, const char *msg) {
		StackedArray<Expression *> expressions(scratch);
		while (!try_eat(terminator)) {
			expressions.push_back(expression());
			if (!try_eat(separator)) {
				eat(terminator, msg);
				break;
			}
		}
		return expressions.get(arena);
	}

	std::span<std::string_view> identifier_list(TokenKind separator, TokenKind terminator, const char *ident_msg, const char *term_msg) {
		StackedArray<std::string_view> identifiers(scratch);
		while (!try_eat(terminator)) {
			identifiers.push_back(eat_identifier(ident_msg));
			if (!try_eat(separator)) {
				eat(terminator, term_msg);
				break;
			}
		}
		return identifiers.get(arena);
	}

	Expression *nullerr(int rbp) {
		(void) rbp;
		error(this->lookahead, true, "Expect expresssion");
		return dummy_expression();
	}

	Expression *primary(int rbp) {
		(void) rbp;
		Token token = discard();
		switch (token.kind) {
		case TK_NIL: {
			return create<AstNil>(token.loc);
		}
		case TK_TRUE: {
			return create<AstBoolean>(token.loc, true);
		}
		case TK_FALSE: {
			return create<AstBoolean>(token.loc, false);
		}
		case TK_NUMBER: {
			const char *pos = token.str.data();
			const char *end = pos + token.str.length();
			double value;
			std::from_chars_result res = std::from_chars(pos, end, value);
			if (res.ptr != end) {
				error(token, false, "Failed to parse number");
			}
			return create<AstNumber>(token.loc, value);
		}
		case TK_STRING: {
			return create<AstString>(token.loc, get_string(token.str));
		}
		case TK_THIS: {
			return create<AstThis>(token.loc);
		}
		default:
			unreachable();
		}
	}

	Expression *super(int rbp) {
		(void) rbp;
		Location loc = eat(TK_SUPER, "Expect 'super'").loc;
		eat(TK_DOT, "Expect '.' after 'super'");
		std::string_view method = eat_identifier("Expect superclass method name");
		return create<AstSuper>(loc, method);
	}

	Expression *ident(int rbp) {
		(void) rbp;
		std::string_view name = eat_identifier("Expect variable name");
		return create<AstVariableAccess>(prev_tok().loc, name);
	}

	Expression *unop(int rbp) {
		Token optok = discard();

		UnOp op;
		switch (optok.kind) {
		case TK_BANG:  op = UnOp::NOT;    break;
		case TK_MINUS: op = UnOp::NEGATE; break;
		default: unreachable();
		}
		Expression *argument = expression_bp(rbp);
		return create<AstUnaryOperation>(optok.loc, op, argument);
	}

	AstBlock *block(const char *msg = "Expect '{'") {
		Location loc = eat(TK_LBRACE, msg).loc;
		StackedArray<Statement *> statements(scratch);
		while (peek() != TK_RBRACE && peek() != TK_EOF) {
			statements.push_back(declaration());
		}
		eat(TK_RBRACE, "Expect '}' after block");
		return create<AstBlock>(loc, statements.get(arena));
	}

	Statement *var_decl() {
		Location loc = eat(TK_VAR, "Expect 'var'").loc;
		std::string_view name = eat_identifier("Expect variable name");
		Expression *value = nullptr;
		if (try_eat(TK_EQUAL)) {
			loc = this->lookahead.loc;
			value = expression();
		}
		eat(TK_SEMICOLON, "Expect ';' after variable declaration");
		return create<AstDefinition>(loc, name, value);
	}

	Statement *cond() {
		Location loc = eat(TK_IF, "Expect 'if'").loc;
		eat(TK_LPAREN, "Expect '(' after 'if'");
		Expression *condition = expression();
		eat(TK_RPAREN, "Expect ')' after condition");
		Statement *consequent = statement();
		Statement *alternative = nullptr;
		if (try_eat(TK_ELSE)) {
			alternative = statement();
		}
		return create<AstIf>(loc, condition, consequent, alternative);
	}

	Statement *return_stmt() {
		Location loc = eat(TK_RETURN, "Expect 'return'").loc;
		Expression *value = nullptr;
		if (!try_eat(TK_SEMICOLON)) {
			value = expression();
			eat(TK_SEMICOLON, "Expect ';' after return value");
		}
		return create<AstReturn>(loc, value);
	}

	Statement *while_loop() {
		Location loc = eat(TK_WHILE, "Expect 'while'").loc;
		eat(TK_LPAREN, "Expect '(' after 'while'");
		Expression *condition = expression();
		eat(TK_RPAREN, "Expect ')' after condition");
		Statement *body = statement();
		return create<AstWhile>(loc, condition, body);
	}

	Statement *for_loop() {
		Location loc = eat(TK_FOR, "Expect 'for'").loc;
		eat(TK_LPAREN, "Expect '(' after 'for'");

		Statement *initializer;
		if (peek() == TK_VAR) {
			initializer = var_decl();
		} else if (try_eat(TK_SEMICOLON)) {
			initializer = nullptr;
		} else {
			initializer = expression_statement();
		}

		Expression *condition = nullptr;
		if (peek() != TK_SEMICOLON) {
			condition = expression();
		}
		Location condition_end_loc = eat(TK_SEMICOLON, "Expect ';' after loop condition").loc;

		Statement *increment = nullptr;
		if (peek() != TK_RPAREN) {
			Expression *expr = expression();
			increment = create<AstExpressionStatement>(expr->loc, expr);
		}
		eat(TK_RPAREN, "Expect ')' after for clauses");

		Statement *body = statement();

		if (increment) {
			body = create<AstBlock>(loc, arena->allocate_array({ body, increment }));
		}

		if (!condition) {
			condition = create<AstBoolean>(condition_end_loc, true);
		}

		Statement *loop = create<AstWhile>(loc, condition, body);

		if (initializer) {
			return create<AstBlock>(loc, arena->allocate_array({ initializer, loop }));
		} else {
			return loop;
		}
	}

	Statement *print() {
		Location loc = eat(TK_PRINT, "Expect 'print'").loc;
		Expression *value = expression();
		eat(TK_SEMICOLON, "Expect ';' after value");
		return create<AstPrint>(loc, value);
	}

	Expression *paren(int rbp) {
		(void) rbp;
		eat(TK_LPAREN, "Expect '('");
		Expression *ast = expression();
		eat(TK_RPAREN, "Expect ')' after expression");
		return ast;
	}

	AstFunction *function() {
		std::string_view name = eat_identifier("Expect function name");
		Location loc = prev_tok().loc;
		eat(TK_LPAREN, "Expect '(' after function name");
		std::span<std::string_view> parameters = identifier_list(TK_COMMA, TK_RPAREN, "Expect parameter name", "Expect ')' after parameters");
		AstBlock *body = block("Expect '{' before function body");
		return create<AstFunction>(loc, name, parameters, body);
	}

	Statement *fun_decl() {
		eat(TK_FUN, "Expect 'fun'");
		AstFunction *fun = function();
		return fun;
	}

	Statement *class_decl() {
		Location loc = eat(TK_CLASS, "Expect 'class'").loc;
		std::string_view name = eat_identifier("Expect class name");
		AstVariableAccess *superclass = nullptr;
		if (try_eat(TK_LESS)) {
			std::string_view superclass_name = eat_identifier("Expect superclass name");
			superclass = create<AstVariableAccess>(prev.loc, superclass_name);
			if (name == superclass_name) {
				error(prev, false, "A class can't inherit from itself");
			}
		}
		eat(TK_LBRACE, "Expect '{' before class body");
		StackedArray<AstFunction *> methods(scratch);
		while (peek() != TK_RBRACE && peek() != TK_EOF) {
			methods.push_back(function());
		}
		eat(TK_RBRACE, "Expect '}' after class body");
		return create<AstClass>(loc, name, superclass, methods.get(arena));
	}


	Expression *stop(Expression *left, int rbp) {
		(void) left;
		(void) rbp;
		unreachable();
	}

	static Expression *dummy_expression() {
		static AstNil nil { { { Location(0, 0) }, ExpressionKind::AstNil } };
		return &nil;
	}

	Expression *lefterr(Expression *left, int rbp) {
		(void) left;
		(void) rbp;
		error(this->lookahead, true, "Expect operator or end of expression");
		// Set the current token to something with low binding power to not get
		// into infinite loop of `lefterr`s on the same token.
		this->lookahead.kind = TK_EOF;
		return dummy_expression();
	}

	Expression *binop(Expression *left, int rbp) {
		Token optok = discard();

		BinOp op;
		switch (optok.kind) {
		case TK_PLUS:          op = BinOp::ADD;              break;
		case TK_MINUS:         op = BinOp::SUBTRACT;         break;
		case TK_LESS:          op = BinOp::LESS_THAN;        break;
		case TK_LESS_EQUAL:    op = BinOp::LESS_OR_EQUAL;    break;
		case TK_GREATER:       op = BinOp::GREATER_THAN;     break;
		case TK_GREATER_EQUAL: op = BinOp::GREATER_OR_EQUAL; break;
		case TK_EQUAL_EQUAL:   op = BinOp::EQUAL;            break;
		case TK_BANG_EQUAL:    op = BinOp::NOT_EQUAL;        break;
		case TK_SLASH:         op = BinOp::DIVIDE;           break;
		case TK_ASTERISK:      op = BinOp::MULTIPLY;         break;
		default: unreachable();
		}
		Expression *right = expression_bp(rbp);
		return create<AstBinaryOperation>(optok.loc, op, left, right);
	}

	Expression *logop(Expression *left, int rbp) {
		Token optok = discard();

		LogicalOp op;
		switch (optok.kind) {
		case TK_AND: op = LogicalOp::AND; break;
		case TK_OR:  op = LogicalOp::OR;  break;
		default: unreachable();
		}
		Expression *right = expression_bp(rbp);
		return create<AstLogicalOperation>(optok.loc, op, left, right);
	}

	Expression *call(Expression *left, int rbp) {
		(void) rbp;
		Location loc = eat(TK_LPAREN, "Expect '('").loc;
		std::span<Expression*> arguments = expression_list(TK_COMMA, TK_RPAREN, "Expect ')' after arguments");
		return create<AstCall>(loc, left, arguments);
	}

	Expression *field(Expression *left, int rbp) {
		(void) rbp;
		Location loc = eat(TK_DOT, "Expect '.'").loc;
		std::string_view field = eat_identifier("Expect property name after '.'");
		return create<AstFieldAccess>(loc, left, field);
	}

	Expression *assign(Expression *left, int rbp) {
		Location loc = eat(TK_EQUAL, "Expect '='").loc;
		if (auto *variable_access = dyn_cast<AstVariableAccess>(left)) {
			Expression *value = expression_bp(rbp);
			return create<AstVariableAssignment>(loc, variable_access->name, value);
		} else if (auto *field_access = dyn_cast<AstFieldAccess>(left)) {
			Expression *value = expression_bp(rbp);
			return create<AstFieldAssignment>(loc, field_access->object, field_access->field, value);
		} else {
			error(this->prev, false, "Invalid assignment target");
			return left;
		}
	}

	struct NullInfo {
		Expression *(Parser::*nud)(int rbp);
		int rbp;
	};
	static NullInfo null_info[];

	struct LeftInfo {
		Expression *(Parser::*led)(Expression *left, int rbp);
		int lbp;
		int rbp;
	};
	static LeftInfo left_info[];


	Expression *expression_bp(int bp) {
		NullInfo ni = null_info[peek()];
		Expression *left = (this->*ni.nud)(ni.rbp);

		for (;;) {
			LeftInfo li = left_info[peek()];
			if (li.lbp < bp) {
				break;
			}
			left = (this->*li.led)(left, li.rbp);
		}

		return left;
	}

	Statement *expression_statement() {
		Expression *expr = expression();
		eat(TK_SEMICOLON, "Expect ';' after expression");
		return create<AstExpressionStatement>(expr->loc, expr);
	}

	Statement *statement() {
		switch (peek()) {
		case TK_IF:
			return cond();
		case TK_PRINT:
			return print();
		case TK_RETURN:
			return return_stmt();
		case TK_WHILE:
			return while_loop();
		case TK_FOR:
			return for_loop();
		case TK_LBRACE:
			return block();
		default: {
			return expression_statement();
		}
		}
	}

	Statement *declaration() {
		Statement *stmt;
		switch (peek()) {
		case TK_CLASS:
			stmt = class_decl();
			break;
		case TK_FUN:
			stmt = fun_decl();
			break;
		case TK_VAR:
			stmt = var_decl();
			break;
		default:
			stmt = statement();
			break;
		}

		if (panic_mode) {
			synchronize();
		}

		return stmt;
	}

	void synchronize() {
		panic_mode = false;
		while (peek() != TK_EOF) {
			if (prev.kind == TK_SEMICOLON) {
				return;
			}
			switch (peek()) {
			case TK_CLASS:
			case TK_FUN:
			case TK_VAR:
			case TK_FOR:
			case TK_IF:
			case TK_WHILE:
			case TK_PRINT:
			case TK_RETURN:
				return;
			default:;
			}
			discard();
		}
	}

public:
	AstProgram program()
	{
		StackedArray<Statement *> top_level_statements(scratch);
		while (!try_eat(TK_EOF)) {
			top_level_statements.push_back(declaration());
		}
		return AstProgram { top_level_statements.get(arena) };
	}

};

Parser::NullInfo Parser::null_info[] = {
	#define TOK_NULL(tok, str, nud, rbp, ...) { &Parser:: nud, rbp },
	TOKENS(TOK_NULL, TOK_NULL, TOK_NULL)
	#undef TOK_NULL
};

Parser::LeftInfo Parser::left_info[] = {
	#define TOK_LEFT(tok, str, nud, nrbp, led, lbp, assoc) { &Parser:: led, lbp, lbp + (ASSOC_##assoc == ASSOC_LEFT) },
	TOKENS(TOK_LEFT, TOK_LEFT, TOK_LEFT)
	#undef TOK_LEFT
};

std::variant<AstProgram, std::vector<ParserError>>
parse(ArenaAllocator *arena, StackingAllocator *scratch, std::string_view source)
{
	size_t start_pos = arena->save();
	Parser parser { arena, scratch, source };
	VMLogger::get().log("Parser START");
	AstProgram prog = parser.program();
	VMLogger::get().log("Parser END");
	if (parser.had_error) {
		arena->restore(start_pos);
		return std::move(parser.errors);
	}
	return prog;
}
