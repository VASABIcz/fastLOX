#pragma once

#include "../utils/Errorable.h"
#include "../utils/utils.h"

#include <utility>

class UnknownToken: public Errorable {
    DEBUG_INFO(UnknownToken)
public:
    explicit UnknownToken(string token): token(std::move(token)) {}

    [[nodiscard]] string_view category() const override {
        return "Lexer";
    }

    [[nodiscard]] string_view type() const override {
        return "UnknownToken";
    }

    string message(ExceptionContext &) override {
        return stringify("Encountered left invalid token:  \"{}\"", token);
    }

private:
    std::string token;
};