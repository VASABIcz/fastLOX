// Michal Vlasák, FIT CTU, 2024

// Implementation of output operators for ASTs, just to show how to work with
// ASTs.

#include "ast.hpp"

std::ostream &operator<<(std::ostream &os, UnOp op) {
	switch (op) {
	case UnOp::NOT:    return os << "!";
	case UnOp::NEGATE: return os << "-";
	}
	unreachable();
}

std::ostream &operator<<(std::ostream &os, BinOp op) {
	switch (op) {
	case BinOp::EQUAL:            return os << "==";
	case BinOp::NOT_EQUAL:        return os << "!=";
	case BinOp::LESS_THAN:        return os << "<";
	case BinOp::LESS_OR_EQUAL:    return os << "<=";
	case BinOp::GREATER_THAN:     return os << ">";
	case BinOp::GREATER_OR_EQUAL: return os << ">=";
	case BinOp::ADD:              return os << "+";
	case BinOp::SUBTRACT:         return os << "-";
	case BinOp::MULTIPLY:         return os << "*";
	case BinOp::DIVIDE:           return os << "/";
	}
	unreachable();
}

std::ostream &operator<<(std::ostream &os, LogicalOp op) {
	switch (op) {
	case LogicalOp::AND: return os << "and";
	case LogicalOp::OR:  return os << "or";
	}
	unreachable();
}
std::ostream &operator<<(std::ostream &os, Expression *expr) {
	visit(expr,
	[&] (AstNil *) {
		os << "nil";
	},
	[&] (AstBoolean *boolean) {
		os << (boolean->value ? "true" : "false");
	},
	[&] (AstNumber *number) {
		os << number->value;
	},
	[&] (AstString *string) {
		os << string->value;
	},

	[&] (AstThis *) {
		os << "this";
	},
	[&] (AstSuper *super) {
		os << "(super " << super->method << ")";
	},

	[&] (AstUnaryOperation *unop) {
		os << "(" << unop->op << " " << unop->argument << ")";
	},
	[&] (AstBinaryOperation *binop) {
		os << "(" << binop->op << " " << binop->left << " ";
		os << binop->right << ")";
	},

	[&] (AstLogicalOperation *logop) {
		os << "(" << logop->op << " " << logop->left << " ";
		os << logop->right << ")";
	},

	[&] (AstVariableAccess *variable_access) {
		os << variable_access->name;
	},
	[&] (AstVariableAssignment *variable_assignment) {
		os << "(= " << variable_assignment->name << " ";
		os << variable_assignment->value << ")";
	},

	[&] (AstFieldAccess *field_access) {
	      os << "(. " << field_access->object;
	      os << " " << field_access->field << ")";
	},
	[&] (AstFieldAssignment *field_assignment) {
	      os << "(= " << field_assignment->object << " ";
	      os << field_assignment->field << " ";
	      os << field_assignment->value << ")";
	},

	[&] (AstCall *function_call) {
		os << "(call " << function_call->callee;
		for (Expression *arg : function_call->arguments) {
			os << " " << arg;
		}
		os << ")";
	});
	return os;
}

std::ostream &operator<<(std::ostream &os, Statement *stmt) {
	visit(stmt,
	[&] (AstDefinition *definition) {
		os << "(var " << definition->name;
		if (definition->value) {
			os << " = " << definition->value;
		}
		os << ")";
	},

	[&] (AstFunction *function) {
		os << "(fun " << function->name << "(";
		if (!function->parameters.empty()) {
			os << function->parameters[0];
		}
		for (size_t i = 1; i < function->parameters.size(); i++) {
			os << " " << function->parameters[i];
		}
		os << ") ";
		for (Statement *stmt : function->body->statements) {
			os << stmt;
		}
		os << ")";
	},
	[&] (AstClass *ast_klass) {
		os << "(class " << ast_klass->name << "(";
		if (ast_klass->superclass) {
			os << " < " << ast_klass->superclass;
		}
		for (AstFunction *method : ast_klass->methods) {
			os << " " << method;
		}
		os << ")";
	},

	[&] (AstReturn *ret) {
		os << "(return";
		if (ret->value) {
			os << " " << ret->value;
		}
		os << ")";
	},

	[&] (AstIf *if_stmt) {
		if (if_stmt->alternative) {
			os << "(if-else " << if_stmt->condition << " ";
			os << if_stmt->consequent << " " << if_stmt->alternative;
		} else {
			os << "(if " << if_stmt->condition << " ";
			os << if_stmt->consequent;
		}
		os << ")";
	},
	[&] (AstWhile *while_stmt) {
		os << "(while " << while_stmt->condition << " ";
		os << while_stmt->body << ")";
	},
	[&] (AstPrint *print) {
		os << "(print " << print->value << ")";
	},
	[&] (AstExpressionStatement *expression_statement) {
		os << "(; " << expression_statement->expression << ")";
	},
	[&] (AstBlock *block) {
		os << "(block ";
		for (Statement *stmt : block->statements) {
			os << stmt;
		}
		os << ")";
	});
	return os;
}
