#pragma once

#include <iostream>
#include <memory>

#include "forward.h"
#include "../lexing/LexingUnit.h"
#include "ParsingUnit.h"
#include "TokenProvider.h"

#define assertTok(token) TRY(this->getAssert(token, __FUNCTION__))
#define assertToken(token) TRY(parser.getAssert(token, this->className()))

template<typename IN1, typename OUT1, typename STATE, typename ERR>
class Parser: public TokenProvider<IN1> {
public:
    explicit Parser(span<Token<IN1>> tokens, span<unique_ptr<ParsingUnit<IN1, OUT1, STATE, ERR>>> units, STATE state)
            : TokenProvider<IN1>(tokens), units(std::move(units)), state(state) {

    }

    auto parseManyWithSeparatorUntil3(IN1 sep, IN1 end, auto fn) -> expected<vector<remove_reference_t<decltype(fn().value())>>, ERR> {
        if (this->isPeekType(end)) return {};

        vector<remove_reference_t<decltype(fn().value())>> result;

        while (!this->isPeekType(end)) {
            result.emplace_back(TRY(fn()));

            if (!this->isPeekType(end)) {
                TRY(this->getAssert(sep));
            }
        }

        return result;
    }

    const BaseParser<IN1, OUT1, STATE, ERR>* getSelfConst() const {
        return static_cast<const BaseParser<IN1, OUT1, STATE, ERR>*>(this);
    }

    BaseParser<IN1, OUT1, STATE, ERR>* getSelf() {
        return static_cast<BaseParser<IN1, OUT1, STATE, ERR>*>(this);
    }

    bool canParse(LookDirection direction) {
        for (auto &unit: this->units) {
            if (unit->lookDirection() == direction && unit->canParse(*getSelfConst()))
                return true;
        }

        return false;
    }

    const ParsingUnit<IN1, OUT1, STATE, ERR>* getParsingUnit(LookDirection type) const {
        for (auto const &unit: this->units) {
            if (unit->lookDirection() != type) continue;

            if (unit->canParse(*getSelfConst())) {
                return unit.get();
            }
        }

        return nullptr;
    }

    expected<bool, ERR> parseOne(LookDirection direction) {
        auto parsingUnit = getParsingUnit(direction);

        if (parsingUnit == nullptr) return false;

        auto pos = this->tokens[this->counter].position;

        if (this->verbose) {
            for (auto i = 0; i < this->depth; i++) {
                print("  ");
            }

            this->depth += 1;

            println("* {} {}:{}:{}", parsingUnit->className(), this->fileHint.has_value() ? *this->fileHint : "{no file}", (pos.row+1)+rowOffset, (pos.column)+colOffset);
        }

        // cout << "Parsing unit " << (*parsingUnit)->className() << endl;
        auto start = this->counter;
        auto result = TRY(parsingUnit->parse(*getSelf()));
        auto end = this->counter;

        // FIXME not that much portable requires `result->tokens` instead of `result.tokens`
        if constexpr (requires { requires std::same_as<decltype(result->tokens), vector<Token<IN1>>>; }) {
            for (auto i : views::iota(start, end)) {
                result->tokens.push_back(this->tokens[i]);
            }
        }

        this->depth -= 1;

        this->buffer.push_back(std::move(result));

        return true;
    }

    vector<OUT1>& getBuffer() {
        return buffer;
    }

    vector<OUT1> movBuf() {
        return std::move(buffer);
    }

    STATE& getStateMut() {
        return state;
    }

    const STATE& getState() const {
        return state;
    }

    void setState(STATE s) {
        state = s;
    }

    ParserResult<OUT1, IN1> prevPop() {
        if (not canPop()) {
            return unexpected{OutOfTokens{}};
        }

        return unsafePop();
    }

    bool canPop() const {
        return not this->buffer.empty();
    }

    OUT1 unsafePop() {
        auto buf = std::move(this->buffer[this->buffer.size() - 1]);
        this->buffer.pop_back();
        return buf;
    }

    OUT1& unsafePeekStack() {
        return this->buffer[this->buffer.size() - 1];
    }

    const OUT1& unsafePeekStackConst() const {
        return this->buffer[this->buffer.size() - 1];
    }

    bool isPeekStack(std::function<bool(const OUT1&)> func) const {
        if (!canPop()) return false;

        return func(unsafePeekStackConst());
    }

    ParserResult<OUT1*, IN1> peekStack() {
        if (!canPop()) return unexpected{OutOfTokens{}};
        return unsafePeekStack();
    }

    // template<typename FN>
    bool isPeek(const std::function<bool(const Token<IN1>&)>& fn) const {
        auto tok = this->getToken();
        if (not tok.has_value()) return false;

        return fn(*tok);
    }

    ParserResult<Token<IN1>, IN1> getTok() {
        if (this->isDone()) return unexpected{OutOfTokens{}};

        return *this->getToken();
    }

    ParserResult<Token<IN1>, IN1> consumeToken() {
        if (this->isDone()) return unexpected{OutOfTokens{}};

        auto tok = this->getToken();
        this->counter += 1;
        return *tok;
    }

    void prevPush(OUT1 node) {
        buffer.push_back(std::move(node));
    }

    expected<OUT1, ERR> parseSomething() {
        TRY(parseOne(Ahead));

        while (TRY(parseOne(Behind))) {}

        while (TRY(parseOne(Around))) {}

        if (buffer.empty()) return EmptyStack{};

        return prevPop();
    }

    expected<OUT1, ERR> parseSomethingTerminator(IN1 terminator) {
        TRY(parseOne(LookDirection::Ahead));
        if (this->isPeekTypeOffset(terminator, -1)) goto exit;
        if (this->isPeekTypeConsume(terminator)) goto exit;

        while (TRY(parseOne(LookDirection::Behind))) {
            if (this->isPeekTypeOffset(terminator, -1)) goto exit;
            if (this->isPeekTypeConsume(terminator)) goto exit;
        }

        while (TRY(parseOne(LookDirection::Around))) {
            if (this->isPeekTypeOffset(terminator, -1)) goto exit;
            if (this->isPeekTypeConsume(terminator)) goto exit;
        }

        exit:
        return TRY(this->prevPop());
    }

    expected<void, ERR> parseFile(IN1 terminator) {
        while (TRY(parseOne(LookDirection::Ahead))) {
            this->isPeekTypeConsume(terminator);
        }

        if (not this->isDone())
            return unexpected{ParserError<IN1>{CantParse{TRY(this->getTok())}}};

        return {};
    }

    expected<bool, ERR> parseBinaryArm() {
        TRY(parseOne(LookDirection::Ahead));

        while (TRY(parseOne(LookDirection::Behind))) {}

        return {};
    }

    expected<void, ERR> parse() {
        while (!this->isDone()) {
            if (this->buffer.empty()) {
                if (TRY(parseOne(Ahead))) continue;
            }

            if (TRY(parseOne(Behind))) continue;

            if (TRY(parseOne(Around))) continue;

            if (TRY(parseOne(Ahead))) continue;

            return unexpected{ParserError<IN1>{CantParse<IN1>{TRY(this->getTok())}}};
        }

        return {};
    }

    expected<void, ERR> parse(IN1 terminator) {
        while (!this->isDone()) {
            if (this->buffer.empty()) {
                if (TRY(parseOne(Ahead))) continue;
            }

            if (this->isPeekTypeConsume(terminator)) continue;

            if (TRY(parseOne(Behind))) continue;

            if (this->isPeekTypeConsume(terminator)) continue;

            if (TRY(parseOne(Around))) continue;

            if (this->isPeekTypeConsume(terminator)) continue;

            if (TRY(parseOne(Ahead))) continue;

            if (this->isPeekTypeConsume(terminator)) continue;

            return unexpected{ParserError<IN1>{CantParse<IN1>{TRY(this->getTok())}}};
        }

        return {};
    }

    expected<OUT1, ERR> parseStuff() {
        TRY(parseOne(Ahead));

        while (TRY(parseOne(Behind))) {}

        return this->prevPop();
    }

    span<unique_ptr<ParsingUnit<IN1, OUT1, STATE, ERR>>> units;
    STATE state;
    vector<OUT1> buffer{};
    int depth = 0;
    optional<string> fileHint;
    int rowOffset = 0;
    int colOffset = 0;
    bool verbose = false;
};

template<typename IN1, typename OUT1, typename STATE, typename ERR>
class BaseParser: public Parser<IN1, OUT1, STATE, ERR> {
public:
    using Parser<IN1, OUT1, STATE, ERR>::Parser;
};