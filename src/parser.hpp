#pragma once

// Michal Vlasák, FIT CTU, 2024

// A parser is included in `parser.cpp`. It's interface is minimal, and mostly
// consists of the `parse` function, which returns a `std::variant` of
// `AstProgram` or `std::vector` of `ParserError`s. (See the bottom of this
// file.)
//
// The `ParserError` class contains basic information about errors, in
// particular what they are about, and where in the source they were found. They
// are not fully optimized, as hopefully error handling is a rare "slow path".
//
// The parser was adapted from a different language to Lox, and while currently
// tries to return error messages compatible with the reference clox and jlox
// implementations, it may not be feature complete in this regard.
//
// The `ParserError::print` method allows a (mostly) clox and jlox compatible
// printing. The `ParserError::print_with_context` method allows to print errors
// with the line they occured on, which is more helpful.

#include <string_view>
#include <ostream>
#include <iomanip>
#include <variant>
#include <vector>

#include "ast.hpp"
#include "allocator.hpp"


class ParserError {
public:
	ParserError(Location loc, std::string token, const char *msg)
		: loc(loc), token(std::move(token)), message(msg) {}

	Location location() {
		return loc;
	}

	void print(std::ostream& os) {
		os << "[line " << loc.line << "] Error";
		os << " at '" << token << "'";
		os << ": " << message << "\n";
	}

	void print_with_context(std::ostream& os, std::string_view source) {
		const char *line_start = source.data();
		const char *end = source.data() + source.size();
		for (size_t i = 0; i < loc.line - 1; i++) {
			while (*line_start != '\n' && line_start != end) {
				line_start++;
       			}
			line_start++;
		}
		const char *line_end = line_start;
		while (*line_end != '\n' && line_end != end) {
			line_end++;
		}

		os << '[' << loc.line << ':' << loc.col << "]: parser error: " << message << "\n  ";
		size_t pad = loc.col - 1 + 1;
		for (const char *c = line_start; c < line_end; c++) {
			if (*c == '\t') {
				os << "    ";
				if (c < line_start + loc.col - 1) {
					pad += 3;
				}
			} else {
				os << *c;
			}
		}
		os << "\n  " << std::setw(pad) << '^' << "\n";
	}

private:
	Location loc;
	std::string token;
	const char *message;
};

// The very minimal interface to the parser. Requires the caller to provide
// memory allocators. The `ArenaAllocator` will be used for storing the AST,
// while the `StackingAllocator` will be used as a scratch space. The scratch
// space will be always restored to state before calling `parse`.
// `ArenaAllocator` will be restored before returning, in case any error is
// encountered.
//
// The AST is fully allocated in the Arena. There are no references to external
// memory, in particular there will be no references to the source string. The
// information in `ParserError` is just _offsets_ to the source.
//
// The parser optimizes for the "fast path" where a correct program is parsed.
// Errors are handled with basic "panic mode" recovery, though because of the
// design, incorrect programs may incur more memory usage than anticipated.
std::variant<AstProgram, std::vector<ParserError>>
parse(ArenaAllocator *arena, StackingAllocator *scratch, std::string_view source);
