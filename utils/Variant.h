#pragma once
#include <variant>

template<typename... Ts>
struct Overloads : Ts... { using Ts::operator()...; };

template<typename... Ts>
struct Variant {
    const std::variant<Ts...>& inner;

    template<typename Ret, typename ...Cs>
    Ret match(Cs&&... cs) {
        return std::visit(Overloads{std::forward<Cs>(cs)...}, inner);
    }

    template<typename ...Cs>
    auto match(Cs&&... cs) {
        return std::visit(Overloads{std::forward<Cs>(cs)...}, inner);
    }
};

template<typename... Ts>
struct MutVariant {
    std::variant<Ts...>& inner;

    template<typename Ret, typename ...Cs>
    Ret match(Cs&&... cs) {
        return std::visit(Overloads{std::forward<Cs>(cs)...}, inner);
    }

    template<typename ...Cs>
    auto match(Cs&&... cs) {
        return std::visit(Overloads{std::forward<Cs>(cs)...}, inner);
    }
};

#define CASE(v) [&](const v& it)
#define CASE_REF(v) [&](v& it)
#define CASE1(v) [&](v&& it)
#define CASE_VAL(v) [&](v it)
#define DEFAULT [&] (auto&& it)
#define DEFAULT_VAL [&] (auto it)