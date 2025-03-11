#pragma once
#include <utility>
#include <vector>
#include <memory>

#include "../lexing/LexingUnit.h"
#include "../utils/utils.h"
#include "exceptions.h"

template<typename T>
class TokenProvider {
public:
    explicit TokenProvider(span<Token<T>> tokens) : tokens(std::move(tokens)) {

    }

    bool isPeekTypeOffset(T type, int offset) const {
        auto token = getTokenOffset(offset);

        if (!token.has_value()) return false;

        return token->type == type;
    }

    bool isPeekType(T type) const {
        return isPeekTypeOffset(type, 0);
    }

    bool isPeekSeq(span<const T> args) const {
        auto i = 0u;

        for (auto item : args) {
            if (!isPeekTypeOffset(item, i)) return false;
            i++;
        }

        return true;
    }

    bool isPeekSeq(initializer_list<T> args) const {
        return isPeekSeq(span(args.begin(), args.end()));
    }

    bool isPeekTypeOneOf(initializer_list<T> types) const {
        for (auto&& type : types) {
            if (isPeekType(type)) return true;
        }

        return false;
    }

    [[nodiscard]] bool isDone() const {
        return counter >= tokens.size();
    }

    span<Token<T>> remaining() {
        return {&tokens[counter], tokens.size()-counter};
    }

    bool isPeekTypeConsume(T type) {
        if (isPeekType(type)) {
            consume();
            return true;
        }
        return false;
    }

    optional<Token<T>> getPeekConsume(T type) {
        if (isPeekType(type)) {
            auto res = this->getToken();
            consume();
            return res;
        }
        return {};
    }

    bool isPeekTypeOffsetOneOf(int offset, std::initializer_list<T> types) const {
        for(auto const& type : types) {
            if (isPeekTypeOffset(type, offset)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]]
    ParserResult<Token<T>, T> getAssert(T type) {
        if (!isPeekType(type)) return unexpected(WrongToken<T>(type, getToken(), ""));

        auto t = getTokenOffset(0).value();
        consume();
        return t;
    }

    [[nodiscard]]
    ParserResult<Token<T>, T> getAssert(T type, string_view name) {
        if (!isPeekType(type)) return unexpected(WrongToken<T>(type, getToken(), string(name)));

        auto t = getTokenOffset(0).value();
        consume();
        return t;
    }

    ParserResult<Token<T>, T> getNext() {
        if (this->isDone()) return unexpected{OutOfTokens{}};

        auto t =  *this->getToken();
        consume();

        return t;
    }

    void consume() {
        // cout << "consuming" << endl;
        counter++;
    }

    bool isAvailableOffset(int offset) const {
        return isValidOffset(offset);
    }

    template<typename R>
auto parseManyWithSeparatorUntil(T sep, T end, auto fn) -> R {
        if (this->isPeekType(end)) return {};

        vector<remove_reference_t<decltype(fn().value())>> result;

        while (!this->isPeekType(end)) {
            result.emplace_back(TRY(fn()));

            if (!this->isPeekType(end)) {
                TRY(getAssert(sep));
            }
        }

        return result;
    }

    template<template<typename> typename RESULT>
auto parseManyWithSeparatorUntil2(T sep, T end, auto fn) -> RESULT<vector<remove_reference_t<decltype(fn().value())>>> {
        if (this->isPeekType(end)) return {};

        vector<remove_reference_t<decltype(fn().value())>> result;

        while (!this->isPeekType(end)) {
            result.emplace_back(TRY(fn()));

            if (!this->isPeekType(end)) {
                TRY(getAssert(sep));
            }
        }

        return result;
    }

// protected:
    optional<Token<T>> getToken() const {
        return getTokenOffset(0);
    }

    Token<T> getTokenAt(size_t index) const {
        return tokens[index];
    }

    optional<Token<T>> getTokenOffset(int offset) const {
        if (!isValidOffset(offset)) return {};

        return tokens[counter+offset];
    }

    [[nodiscard]] bool isValidOffset(int offset) const {
        return counter + offset < tokens.size();
    }

    bool hasToken() const {
        return not isDone();
    }

    span<Token<T>> tokens;
    size_t counter = 0;
};