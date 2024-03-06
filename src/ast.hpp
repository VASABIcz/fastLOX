#pragma once

// Michal Vlasák, FIT CTU, 2024

// This file contains the definitions of the Lox AST. This AST representation
// has been very loosely inspired by the jlox implementation [1], though it
// doesn't use visitor pattern, but custom LLVM inspired RTTI "variant based"
// approach combined with inheritance. See `variant.hpp` for a smaller example.
//
// Most of the AST definitions are just plain data holders. On top of that, the
// data is not owned by the AST nodes, all pointers like std::string_view or
// std::span are non-owning and are meant to point into a single arena, which
// deallocates all memory allocated for the ASTs en masse. This also means that
// our AST doesn't need to define any destructors.
//
// To get AST, see `parser.hpp` and in particular the `parse` function.
//
// To see how to work with ASTs see `ast.cpp` which has an implementation of a
// printer largely equivalent to the one in the book. The printer is of little
// use otherwise, except for maybe debugging.
//
// Notably there is no `AstFor`, since for loops are desugared into other
// expressions in the parser.
//
// [1]: (https://craftinginterpreters.com/a-tree-walk-interpreter.html

#include <cstddef>
#include <string_view>
#include <span>

#include "variant.hpp"
#include "utils.hpp"

struct Location {
	size_t line;
	size_t col;
};

struct Ast {
	Location loc;
};

#define EXPRESSIONS(VARIANT) \
	VARIANT(AstNil) \
	VARIANT(AstBoolean) \
	VARIANT(AstNumber) \
	VARIANT(AstString) \
	VARIANT(AstThis) \
	VARIANT(AstSuper) \
	VARIANT(AstUnaryOperation) \
	VARIANT(AstBinaryOperation) \
	VARIANT(AstLogicalOperation) \
	VARIANT(AstVariableAccess) \
	VARIANT(AstVariableAssignment) \
	VARIANT(AstFieldAccess) \
	VARIANT(AstFieldAssignment) \
	VARIANT(AstCall) \

DEFINE_VARIANT_ENUM(Expression, EXPRESSIONS)

struct Expression : public Ast {
	ExpressionKind kind;
};

struct AstNil : public Expression {
};

struct AstBoolean : public Expression {
	bool value;
};

struct AstNumber : public Expression {
	double value;
};

struct AstString : public Expression {
	std::string_view value;
};

struct AstThis : public Expression {
};

struct AstSuper : public Expression {
	std::string_view method;
};

enum class UnOp {
	NOT,    // !
	NEGATE, // -
};

struct AstUnaryOperation : public Expression {
	UnOp op;
	Expression *argument;
};

enum class BinOp {
	EQUAL,
	NOT_EQUAL,
	LESS_THAN,
	LESS_OR_EQUAL,
	GREATER_THAN,
	GREATER_OR_EQUAL,
	ADD,
	SUBTRACT,
	MULTIPLY,
	DIVIDE,
};

struct AstBinaryOperation : public Expression {
	BinOp op;
	Expression *left;
	Expression *right;
};

enum class LogicalOp {
	AND,
	OR,
};

struct AstLogicalOperation : public Expression {
	LogicalOp op;
	Expression *left;
	Expression *right;
};

struct AstVariableAccess : public Expression {
	std::string_view name;
};

struct AstVariableAssignment : public Expression {
	std::string_view name;
	Expression *value;
};

struct AstFieldAccess : public Expression {
	Expression *object;
	std::string_view field;
};

struct AstFieldAssignment : public Expression {
	Expression *object;
	std::string_view field;
	Expression *value;
};

struct AstCall : public Expression {
	Expression *function;
	std::span<Expression*> arguments;
};

DEFINE_VARIANT_KIND_TYPE_MAP(Expression, EXPRESSIONS)
DEFINE_VISITOR(Expression, EXPRESSIONS)



#define STATEMENTS(VARIANT) \
	VARIANT(AstDefinition) \
	VARIANT(AstFunction) \
	VARIANT(AstClass) \
	VARIANT(AstReturn) \
	VARIANT(AstIf) \
	VARIANT(AstWhile) \
	VARIANT(AstPrint) \
	VARIANT(AstExpressionStatement) \
	VARIANT(AstBlock) \

DEFINE_VARIANT_ENUM(Statement, STATEMENTS)

struct Statement : public Ast {
	StatementKind kind;
};

struct Statement;

struct AstDefinition : public Statement {
	std::string_view name;
	Expression *value;
};

struct AstIf : public Statement {
	Expression *condition;
	Statement *consequent;
	Statement *alternative;
};

struct AstWhile : public Statement {
	Expression *condition;
	Statement *body;
};

struct AstPrint : public Statement {
	Expression *value;
};

struct AstExpressionStatement : public Statement {
	Expression *expression;
};

struct AstBlock : public Statement {
	std::span<Statement*> statements;
};

struct AstFunction : public Statement {
	std::string_view name;
	std::span<std::string_view> parameters;
	AstBlock *body;
};

struct AstClass : public Statement {
	std::string_view name;
	AstVariableAccess *superclass;
	std::span<AstFunction *> methods;
};

struct AstReturn : public Statement {
	Expression *value;
};

struct AstProgram {
	std::span<Statement*> top_level_statements;
};

DEFINE_VARIANT_KIND_TYPE_MAP(Statement, STATEMENTS)
DEFINE_VISITOR(Statement, STATEMENTS)

std::ostream &operator<<(std::ostream &os, UnOp op);
std::ostream &operator<<(std::ostream &os, BinOp op);
std::ostream &operator<<(std::ostream &os, LogicalOp op);
std::ostream &operator<<(std::ostream &os, Expression *expr);
std::ostream &operator<<(std::ostream &os, Statement *stmt);
