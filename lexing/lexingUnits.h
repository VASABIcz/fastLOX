#pragma once
#include "LexingUnit.h"

template<typename T>
class KeywordLexingUnit: public LexingUnit<T> {
DEBUG_INFO(KeywordLexingUnit)
public:
    explicit KeywordLexingUnit(string keyword, T value): keyword(std::move(keyword)), value(value) {}
    bool canParse(const SourceProvider<T> &src) const override {
        return src.isPeekStr(keyword);
    }

    Result<optional<Token<T>>> parse(SourceProvider<T> &src) const override {
        auto p = src.getPosition();

        TRYV(src.consumeAssert(keyword));

        auto token = src.createToken(p, value);

        return {token};
    }
private:
    string keyword;
    T value;
};

template<typename T>
class AlphabeticLexingUnit: public LexingUnit<T> {
DEBUG_INFO(AlphabeticLexingUnit)
public:
    explicit AlphabeticLexingUnit(string keyword, T value): keyword(std::move(keyword)), value(value) {}

    bool canParse(const SourceProvider<T> &src) const override {
        return src.isPeekStr(keyword)
               &&
               (!src.isPeekCharOffset('_', keyword.size()) && !src.isPeekCharFnOffset([](auto it){ return isdigit(it) || isalpha(it); }, keyword.size()));
    }

    Result<optional<Token<T>>> parse(SourceProvider<T> &src) const override {
        auto p = src.getPosition();

        TRYA(src.consumeAssert(keyword));
        auto p1 = src.getPosition();
        if (p1.index - p.index <= 0) {
            println("{} {} `{}`", p1.index, p.index, keyword);
            // println("FUCK MY LIFE {} {}", keyword, value.toString());
            assert(false);
        }

        auto token = src.createToken(p, value);

        return {token};
    }
private:
    string keyword;
    T value;
};

template<typename T>
class RangeLexingUnit: public LexingUnit<T> {
DEBUG_INFO(RangeLexingUnit)
public:
    explicit RangeLexingUnit(string beginning, string end, optional<T> value): beginning(std::move(beginning)), end(std::move(end)), value(value) {}

    bool canParse(const SourceProvider<T> &src) const override {
        return src.isPeekStr(beginning);
    }

    Result<optional<Token<T>>> parse(SourceProvider<T> &src) const override {
        auto p = src.getPosition();

        TRYV(src.consumeAssert(beginning));

        while (!src.isPeekStr(end) && !src.isDone()) {
            src.consume();
        }

        TRYV(src.consumeAssert(end));

        if (!value.has_value()) {
            return {};
        }
        else {
            const Token<T> &token = src.createToken(p, *value);
            return {token};
        }
    }
private:
    string beginning;
    string end;
    optional<T> value;
};

template<typename T>
class WhitespaceLexingUnit: public LexingUnit<T> {
DEBUG_INFO(WhitespaceLexingUnit)
public:
    bool canParse(const SourceProvider<T> &provider) const override {
        return provider.isPeekCharFn([](auto it) { return isspace(it); });
    }

    Result<optional<Token<T>>> parse(SourceProvider<T> &provider) const override {
        while (provider.isPeekCharFn([](auto it) { return isspace(it); })) {
            provider.consume();
        }

        return {};
    }
};

template<typename T>
class IdentifierLexingUnit: public LexingUnit<T> {
DEBUG_INFO(IdentifierLexingUnit)
public:
    explicit IdentifierLexingUnit(T value): value(value) {}

    bool canParse(const SourceProvider<T> &provider) const override {
        return provider.isPeekCharFn([](auto it){ return isalpha(it) || isalnum(it) || it == '_'; });
    }

    Result<optional<Token<T>>> parse(SourceProvider<T> &provider) const override {
        auto start = provider.getPosition();

        while (provider.isPeekCharFn([](auto it){ return isalpha(it) || isdigit(it) || it == '_'; })) {
            provider.consume();
        }

        auto token = provider.createToken(start, value);

        return {token};
    }

    T value;
};

template<typename T, T value>
class CaseInsensitiveLexingUnit: public LexingUnit<T> {
DEBUG_INFO(CaseInsensitiveLexingUnit)
public:
    explicit CaseInsensitiveLexingUnit(string keyword): keyword(std::move(keyword)) {}

    bool canParse(const SourceProvider<T> &src) const override {
        return src.isPeekStr(keyword)
               &&
               (!src.isPeekCharOffset('_', keyword.size()) && !src.isPeekCharFnOffset([](auto it){ return isdigit(it) || isalpha(it); }, keyword.size()));
    }

    Result<optional<Token<T>>> parse(SourceProvider<T> &src) const override {
        auto p = src.getPosition();

        TRYV(src.consumeAssert(keyword));

        auto token = src.createToken(p, value);

        return {token};
    }
private:
    string keyword;
};

template<typename T, T value>
class NumericLexingUnit: public LexingUnit<T> {
    DEBUG_INFO(NumericLexingUnit)
public:
    bool canParse(const SourceProvider<T>& provider) const override {
        return provider.isPeekCharFn([](auto it){ return isdigit(it); }) || provider.isPeekStr("0x") || provider.isPeekStr("0b");
    }

    Result<optional<Token<T>>> parse(SourceProvider<T>& provider) const override {
        auto start = provider.getPosition();

        enum Type {
            DECIMAL,
            HEX,
            BINARY
        };

        auto specialization = DECIMAL;
        if (provider.isPeekStrConsume("0x")) {
            specialization = HEX;
        } else if (provider.isPeekStrConsume("0b")) {
            specialization = BINARY;
        }

        while (provider.isPeekCharFn([&](auto it) {
            if (specialization == DECIMAL) return (bool)isdigit(it);
            if (specialization == HEX) return (bool)isdigit(it) || (it >= 'a' && it <= 'f') || (it >= 'A' && it <= 'F');
            if (specialization == BINARY) return it == '1' || it =='0';
            return false;
        })) {
            provider.consume();
        }

        if (specialization == DECIMAL) {
            if (provider.isPeekChar('.') && provider.isPeekCharFnOffset([](auto c) { return isdigit(c); }, 1)) {
                provider.consume();
                while (provider.isPeekCharFn([](auto it){ return isdigit(it) || it == '_'; })) {
                    provider.consume();
                }
            }
        }

        auto sufixes = array{"f", "F", "f32", "f64", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "cint", "clong"};

        for (auto sufix : sufixes) {
            if (provider.isPeekStrConsume(sufix)) {
                break;
            }
        }

        auto token = provider.createToken(start, value);

        return {token};
    }
};

template<typename T>
class LineCommentLexingUnit: public LexingUnit<T> {
    DEBUG_INFO(LineCommentLexingUnit)
    public:
    bool canParse(const SourceProvider<T> &src) const override {
        return src.isPeekStr("//");
    }

    Result<optional<Token<T>>> parse(SourceProvider<T> &src) const override {
        while (!src.isPeekChar('\n') && !src.isDone()) {
            src.consume();
        }

        return {};
    }
};

template<typename T>
class StringLexingUnit: public LexingUnit<T> {
    DEBUG_INFO(StringLexingUnit)
    public:
    T tokenType;

    explicit StringLexingUnit(T tokenType): tokenType(tokenType) {}

    bool canParse(const SourceProvider<T> &src) const override {
        return src.isPeekChar('"');
    }

    Result<optional<Token<T>>> parse(SourceProvider<T> &src) const override {
        auto pos = src.getPosition();
        src.consume();
        while (!src.isPeekChar('"') && !src.isDone()) {
            auto next = src.consume();
            if (next == '\\') {
                if (src.isDone()) break;
                src.consume();
            }
        }
        TRYV(src.consumeAssert("\""));

        return src.createToken(pos, tokenType);
    }
};