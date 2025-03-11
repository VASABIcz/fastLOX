#pragma once

#include <cstdio>
#include <algorithm>
#include <string_view>

using namespace std;

template<size_t N>
struct StringLiteral {
    constexpr StringLiteral(const char (&str)[N]) {
        std::copy_n(str, N, value);
    }

    constexpr string_view operator()() {
        return asView();
    }

    [[nodiscard]] constexpr auto asView() const -> string_view {
        auto v = string_view(value, N-1);

        return v;
    }

    constexpr static const size_t size = N;

    char value[N];
};