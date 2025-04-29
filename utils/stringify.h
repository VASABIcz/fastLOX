#pragma once

#include <iostream>
#include <expected>
#include <cassert>
#include <set>
#include <unordered_map>
#include <map>
#include <vector>
#include <algorithm>
#include <sstream>
#include <memory>

#include "StringLiteral.h"

using namespace std;

auto hexify(auto v) {
    std::stringstream stream;
    stream << "0x" << std::hex << v;
    return stream.str();
}

template<typename T>
auto hasValueType() -> T::value_type;

template<typename T>
auto hasKeyType() -> T::key_type;

template<typename T>
concept MapLike = requires(T t) {
    hasValueType<T>();
    hasKeyType<T>();
    { *t.begin() } -> std::convertible_to<pair<typename T::key_type, typename T::value_type>>;
};

template<typename T>
concept PairLike = requires(T t) {
    { t.first } -> std::convertible_to<typename T::first_type>;
    { t.second } -> std::convertible_to<typename T::second_type>;
};

template<typename T>
concept VectorLike = requires(T) {
    hasValueType<T>();
};

template<typename T>
concept hasToStringC = requires(T t) {
    { t.toString() } -> std::convertible_to<string>;
};

struct StringifyCtx {
    string_view sep = ""sv;
    string_view beg = ""sv;
    string_view end = ""sv;
    string_view pair = ""sv;
    bool hexify = false;
};

template<typename T>
constexpr string stringify(const T& value, initializer_list<StringifyCtx> ctx = {}, size_t depth = 0u);

constexpr string stringifyMap(const MapLike auto& vec, initializer_list<StringifyCtx> ctx, size_t depth);

constexpr string stringifyVec(const VectorLike auto& vec, initializer_list<StringifyCtx> ctx, size_t depth);

static StringifyCtx MAP_DEFAULT_CTX  = {", ", "{", "}", ": ", false};
static StringifyCtx VEC_DEFAULT_CTX  = {", ", "[", "]", ": ", false};
static StringifyCtx PAIR_DEFAULT_CTX = {", ", "[", "]", ": ", false};

static StringifyCtx getCtx(initializer_list<StringifyCtx> ctx, size_t index, StringifyCtx def) {
    if (index >= ctx.size()) return def;
    return *(ctx.begin()+index);
}

template<typename T>
constexpr string stringify(const T& value, initializer_list<StringifyCtx> ctx, size_t depth) {
    if constexpr (std::is_same_v<T, char>) {
        string idk;
        idk.push_back(value);
        return idk;
    }
    if constexpr (std::is_convertible_v<T, string>) {
        return value;
    }
    else if constexpr (std::is_constructible_v<string, T>) {
        return string(value);
    }
    else if constexpr (MapLike<T>) {
        return stringifyMap(value, ctx, depth);
    }
    else if constexpr (requires { requires std::same_as<unique_ptr<typename T::element_type, typename T::deleter_type>, std::remove_reference_t<T>>; }) {
        return hexify(value.get());
    }
    else if constexpr (VectorLike<T>) {
        return stringifyVec(value, ctx, depth);
    }
    else if constexpr (PairLike<T>) {
        auto current = getCtx(ctx, depth, PAIR_DEFAULT_CTX);
        auto buf = stringify(value.first, ctx, depth + 1);
        buf += current.pair;
        buf += stringify(value.second, ctx, depth + 1);

        return buf;
    }
    else if constexpr (hasToStringC<T>) {
        return value.toString();
    }
    else if constexpr (is_pointer_v<T>) {
        return hexify(value);
    }
    else if constexpr (is_integral_v<T>) {
        auto current = getCtx(ctx, depth, VEC_DEFAULT_CTX);
        return current.hexify ? hexify(value) : to_string(value);
    }
    else if constexpr (requires { { to_string(value) } -> std::convertible_to<string>; }) {
        return to_string(value);
    }
    else {
        static_assert("provided type could not be stringified :(");
    }
}

constexpr string stringifyMap(const MapLike auto& vec, initializer_list<StringifyCtx> ctx, size_t depth) {
    auto current = getCtx(ctx, depth, MAP_DEFAULT_CTX);

    string buf = string(current.beg);

    for (const auto& [key, val] : vec) {
        buf += stringify(key, ctx, depth+1);
        buf += current.pair;
        buf += stringify(val, ctx, depth+1);
        buf += current.sep;
    }

    if (buf.ends_with(current.sep)) {
        for (auto i = 0u; i < current.sep.size(); i++)
            buf.pop_back();
    }

    buf += current.end;

    return buf;
}


constexpr string stringifyVec(const VectorLike auto& vec, initializer_list<StringifyCtx> ctx, size_t depth) {
    auto current = getCtx(ctx, depth, VEC_DEFAULT_CTX);

    string buf = string(current.beg);

    for (const auto& valu : vec) {
        buf += stringify(valu, ctx, depth+1);
        buf += current.sep;
    }

    if (buf.ends_with(current.sep)) {
        for (auto i = 0u; i < current.sep.size(); i++)
            buf.pop_back();
    }

    buf += current.end;

    return buf;
}

template<typename T>
constexpr void strFormHelper(string& buffer, string_view str, size_t& i, const T& arg) {
    while (i < str.size()) {
        if (i+1 < str.size() && string_view(str.begin()+i, str.begin()+i+2) == "{}") {
            buffer += stringify(arg);
            i += 2;
            return;
        }
        if (i+3 < str.size() && string_view(str.begin()+i, str.begin()+i+4) == "{:x}") {
            auto ctx = VEC_DEFAULT_CTX;
            ctx.hexify = true;
            buffer += stringify(arg, {ctx});
            i += 4;
            return;
        }
        buffer += str[i];

        i++;
    }
}

template<typename ...Args>
struct StringChecker {
    string_view inner;

    template<typename T>
    requires convertible_to<const T&, string_view>
    consteval StringChecker(const T& s): inner(s) {
        validate();
    }

    auto invalidArgumentCount() const {}
    auto invalidFormatParameter() const {}

    consteval auto argCount() const {
        return sizeof...(Args);
    }

    consteval auto validate() const {
        if (inner.empty() && argCount() != 0) invalidArgumentCount();
        if (inner.empty() && argCount() == 0) return;

        char a, b = '\0';
        size_t counter = 0u;

        a = inner[0];

        for (auto i = 1u; i < inner.size(); i++) {
            b = inner[i];
            if (a == '{' && b == '}') counter++;
            else if (a == '{' && b == ':') {
                i++;

                size_t start = i;
                while (i < inner.size()) {
                    if (inner[i] == '}') break;
                    i++;
                }
                const auto atribute = string_view(inner.begin()+start, inner.begin()+i);
                if (atribute != "x") invalidFormatParameter();
                counter++;
            }
            a = b;
            if (counter > argCount()) invalidArgumentCount();
        }

        if (counter != argCount()) invalidArgumentCount();
    }
};

template<typename... Args>
constexpr auto stringify(StringChecker<type_identity_t<Args>...> strArg, Args&&... argz) {
    string buffer;
    size_t index = 0u;

    (strFormHelper(buffer, strArg.inner, index, std::forward<Args>(argz)), ...);

    while (index < strArg.inner.size()) {
        buffer += strArg.inner[index];
        index++;
    }

    return buffer;
}

template<typename... Args>
constexpr auto println(StringChecker<type_identity_t<Args>...> strArg, Args&&... argz) {
    cout << stringify(strArg, std::forward<Args>(argz)...) << endl;
}

template<typename... Args>
constexpr auto print(StringChecker<type_identity_t<Args>...> strArg, Args&&... argz) {
    cout << stringify(strArg, std::forward<Args>(argz)...);
}

constexpr auto println() {
    cout << endl;
}

template<typename T, typename SEP>
constexpr auto printHelper(string& buffer, SEP sep, T&& val) {
    buffer += stringify(std::forward<T>(val));
    buffer += sep;
}

template<StringLiteral SEP = ", ", typename... Args>
constexpr auto printl(Args&&... argz) {
    string buffer;

    (printHelper(buffer, SEP.asView(), argz),...);
    buffer = buffer.substr(0, buffer.size()-SEP.asView().size());
    cout << buffer << endl;
}

inline initializer_list<StringifyCtx> DotJoinCtx = {StringifyCtx{".", "", ""}};