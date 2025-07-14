#pragma once
#include <optional>
#include <expected>
#include <string>
#include <variant>
#include "../lexing/Token.h"
#include "../utils/Variant.h"

using namespace std;

template<typename T>
struct WrongToken {
    T expected;
    optional<Token<T>> actual;
    string unitName;
};

template<typename T>
struct CantParse {
    Token<T> nextToken;
};

struct OutOfTokens {};

struct EmptyStack {};

struct InvalidError {
    InvalidError() {
        PANIC();
    }
};

template<typename T>
using ParserError = std::variant<InvalidError, CantParse<T>, WrongToken<T>, OutOfTokens, EmptyStack>;

template<typename T>
string toStringParserError(const ParserError<T>& err) {
    return Variant(err).template match<string>(
        CASE(CantParse<T>) {
            return stringify("CantParse({})", it.nextToken.content);
        },
        CASE(WrongToken<T>) {
            return "WrontToken"s;
        },
        CASE(OutOfTokens) {
            return "OurOfTokens"s;
        },
        CASE(EmptyStack) {
            return "EmptyStack"s;
        },
        CASE(InvalidError) {
            return "InvalidError"s;
        }
    );
}

template<typename V, typename T>
using ParserResult = std::expected<V, ParserError<T>>;