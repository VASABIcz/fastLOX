#pragma once

#include "forward.h"
#include <cstdio>
#include <string>
#include <optional>
#include <functional>
#include <utility>
#include <iostream>
#include "Position.h"
#include "../utils/utils.h"
#include "../utils/Errorable.h"
#include "LexingUnit.h"

template <typename T>
class SourceProvider {
public:
    explicit SourceProvider(string_view data) : data(data) {

    }

    [[nodiscard]] bool isPeekStr(string_view view) const {
        // cout << remaining().SIZE() << " " << view.SIZE() << endl;
        if (remaining().size() < view.size()) return false;
        auto res = remaining().starts_with(view);
        // cout << "isPeekStr " << remaining() << " " << view << " " << res << endl;

        return res;
    }

    [[nodiscard]] bool isPeekChar(char c) const {
        return isPeekCharOffset(c, 0);
    }

    bool isPeekCharConsume(char c) {
        if (isPeekCharOffset(c, 0)) {
            consumeOne();
            return true;
        }
        return false;
    }

    bool isPeekStrConsume(string_view str) {
        if (isPeekStr(str)) {
            consumeMany(str.size());
            return true;
        }
        return false;
    }

    bool isPeekCharFnOffset(const function<bool(char)>& fn, size_t offset) const {
        auto peek = peekCharOffset(offset);
        if (!peek.has_value()) return false;

        return fn(peek.value());
    }

    bool isPeekCharFn(const function<bool(char)>& fn) const {
        return isPeekCharFnOffset(fn, 0);
    }

    [[nodiscard]] bool isPeekCharOffset(char c, size_t offset) const {
        auto peek = peekCharOffset(offset);
        if (!peek.has_value()) return false;

        return *peek == c;
    }

    char consume() {
        return consumeOne();
    }

    [[nodiscard]] Position getPosition() const {
        return Position(row, column, counter);
    }

    [[nodiscard]] string_view sliceToHere(const Position& position) const {
        return string_view{data.begin()+position.index, data.begin()+counter};
    }

    Result<void> consumeAssert(const string_view& name) {
        if (!isPeekStr(name)) {
            return FAILF("tried to assert `{}` but remaining is `{}`", escape(name), escape(remaining()));
        }

        consumeMany(name.size());

        return {};
    }

    [[nodiscard]] bool isDone() const {
        if (counter != 0) {
            return counter >= data.size();
        }

        return counter >= data.size();
    }

    Token<T> createToken(const Position& position, T type) const {
        return Token<T>(position, sliceToHere(position), type);
    }

    [[nodiscard]] string_view remaining() const {
        return string_view{data.begin()+counter, data.end()};
    }
private:
    char consumeOne() {
        if (isPeekChar('\n')) {
            row++;
            column = 0;
        }
        counter++;
        column++;
        return data[counter-1];
    }

    void consumeMany(size_t amount) {
        auto save = counter;
        repeat1(amount) {
            consumeOne();
        }
        assert(counter == save+amount);
        (void)save;
    }

    [[nodiscard]] optional<char> peekCharOffset(size_t offset) const {
        size_t index = counter + offset;

        if (index >= data.size()) {
            return {};
        }

        return data[index];
    }

    string_view data;
    size_t counter = 0;
    size_t row = 0;
    size_t column = 0;
};