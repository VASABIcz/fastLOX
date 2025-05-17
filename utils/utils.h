#pragma once

#include <cassert>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <set>
#include <span>
#include <functional>
#include <ranges>
#include <utility>

#include "stringify.h"
#include "Debuggable.h"
#include <cmath>

template<typename ...Args>
void consume(Args... argz) {

}

using namespace std;

struct Nothing {
public:
    Nothing() = default;
    ~Nothing() = default;
};

#define FAIL_FORMAT stringify("[{}] {}:{}\n-> ", __FUNCTION__, __FILE__, __LINE__)

template<typename T>
void _fail(T msg) {
    cout << msg << endl;
    exit(1);
}

// #define TODO(msg, ...) {assert(false); fail()}
#define TODO() {println("{} reached unimplement point", FAIL_FORMAT); std::terminate();}
#define TODOF(...) cout << FAIL_FORMAT << endl; assert(false); consume(__VA_ARGS__)
#define UNREACHABLEF(...) println("reached unreachable point at {} {}", FAIL_FORMAT, stringify(__VA_ARGS__)); std::unreachable()
#define UNREACHABLE() {println("{} reached unreachable point at", FAIL_FORMAT); PANIC();}

#define FIRST(lmao, ...) lmao
#define ARGS_OR_DEFAULT(DEFAULT, ...) FIRST(__VA_OPT__(__VA_ARGS__,) DEFAULT)
#define PANIC(msg, ...) {println("[{}] {}:{}\n-> " ARGS_OR_DEFAULT("reached invalid point", msg), __FUNCTION__, __FILE__, __LINE__, ##__VA_ARGS__); std::terminate();}

consteval auto getMessage() {
    return "reached invalid point";
}

consteval auto getMessage(auto pepa) {
    return pepa;
}

consteval auto pepa(string a, string b) {
    return a+b;
}

namespace std {
    template<typename T = void>
    void move() {

    }
}

#define DEBUG_INFO(name) public: [[nodiscard]] const string_view className() const override {return #name;} private:

#define TRY(...) ({auto _it = __VA_ARGS__; if (!_it.has_value()) return unexpected(std::move(_it.error())); std::move(*_it); })
#define TRYNULL(x) ({auto _it = x; if (!_it.has_value()) return nullptr; std::move(*_it); })
#define TRYA(x) ({auto _it = x; if (!_it.has_value()) return Err(std::move(_it.error())); *_it; })
#define TRYV(x) {auto _it = x; if (!_it.has_value()) return Err(std::move(_it.error()));}
#define TRY_MAP(x, y) ({auto _it = x; if (!_it.has_value()) { auto _err = std::move(_it.error()); return y; }; std::move(*_it); })
#define TRYV_MAP(x, y) ({auto _it = x; if (!_it.has_value()) { auto _err = std::move(_it.error()); return y; }; })

#define UNWRAPV(...) ({auto _it = __VA_ARGS__; if (!_it.has_value()) PANIC()})
#define UNWRAP(...) ({auto _it = __VA_ARGS__; if (!_it.has_value()) PANIC(); std::move(*_it); })

#define SHORT(x) ({ if (!x.has_value()) return; *x })
#define repeat1(n) for (auto it = (size_t)0; it < n; it++)
#define PRINTW(n) cout << string((n)*2, ' ')
// #define IFD if (DEBUG)
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

constexpr u8 operator ""_u8(unsigned long long value) {
    return static_cast<u8>(value);
}

constexpr u16 operator ""_u16(unsigned long long value) {
    return static_cast<u16>(value);
}

constexpr u32 operator ""_u32(unsigned long long value) {
    return static_cast<u32>(value);
}

constexpr u64 operator ""_u64(unsigned long long value) {
    return static_cast<u64>(value);
}

// #define val
// #define var

void writeBytesToFile(const string& name, auto data) {
    ofstream fajl;
    fajl.open(name, ios::binary | ios::out);

    fajl.write(reinterpret_cast<const char *>(data.data()), data.size()*sizeof(typename decltype(data)::value_type));

    fajl.close();
}

string writeBytesToTempFile(auto data, string_view plus = "") {
    auto r = rand();

    string fileName = stringify("/tmp/{}{}", r ^ (r >> 1), plus);
    writeBytesToFile(fileName, data);
    
    return fileName;
}

void assertInBounds(const auto& vec, size_t index) {
    if (index > vec.size()) PANIC();
}

bool allOf(const auto& vec, auto fn) {
    if (vec.empty()) return false;

    for (const auto& item : vec) {
        if (!fn(item)) return false;
    }

    return true;
}

template<typename ...T>
constexpr auto haveValues(const optional<T>&... options) {
    auto idk = {options...};
    return std::ranges::all_of(idk.begin(), idk.end(), [](const auto& it) { return it.has_value(); });
}

auto collectVec(auto iter) -> vector<std::remove_cv_t<std::remove_reference_t<decltype(*iter.begin())>>> {
    return vector(iter.begin(), iter.end());
}

template<size_t size>
struct Dummy {
    u8 data[size];
};

auto escape(auto str) {
    string escaped;

    for (auto c : str) {
        switch (c) {
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            case '\e':
                escaped += "\\e";
                break;
            default:
                escaped += c;
        }
    }

    return escaped;
}


inline optional<string> unescape(string_view value) {
    string unescaped;

    for (auto i = 0u; i < value.size(); i++) {
        auto c = value[i];

        if (c == '\\') {
            if (i+1 >= value.size()) return {};
            auto next = value[++i];

            if (next == 'n') {
                unescaped += '\n';
            }
            else if (next == 't') {
                unescaped += '\t';
            }
            else if (next == 'r') {
                unescaped += '\r';
            }
            else if (next == 'e') {
                unescaped += '\e';
            }
            else if (next == '\\') {
                unescaped += '\\';
            }
            else if (next == '0') {
                unescaped += (char)0;
            }
            else {
                unescaped += next;
            }
        }
        else {
            unescaped += c;
        }
    }

    return unescaped;
}

template<typename T, typename... Args>
auto getFirType() -> T;

template<int N, typename... Ts> using nThType = typename std::tuple_element<N, std::tuple<Ts...>>::type;

template<typename T>
bool isTheSame(const vector<T>& items) {
    if (items.empty()) return false;

    const auto& firstItem = items[0];

    return std::ranges::all_of(items.begin(), items.end(), [&](const auto& it){ return it == firstItem; });
}

template<typename T>
bool isTheSameOr(const vector<T>& items, T other) {
    if (items.empty()) return false;

    const auto& firstItem = items[0];

    return std::ranges::all_of(items.begin(), items.end(), [&](const auto& it){ return it == firstItem || it == other; });
}

template<typename T>
const T* findNotMatch(const vector<T>& items, const T& other) {
    for (const auto& item : items) {
        if (item != other) {
            return &item;
        }
    }
    return nullptr;
}

bool compare(auto a, auto b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end());
}

template<typename T>
struct Destroyable {
    optional<T> inner{};
    std::function<void(T)> dest{};

    Destroyable(const Destroyable&) = delete;

    Destroyable() = default;

    Destroyable& operator=(Destroyable&& other) {
        inner = std::move(other.inner);
        dest = std::move(other.dest);

        return *this;
    }

    Destroyable& operator=(const Destroyable& other) = delete;

    Destroyable(optional<T> inner, std::function<void(T)> fun): inner(std::move(inner)), dest(std::move(fun)) {}

    ~Destroyable() {
        if (dest) {
            dest(*inner);
        }
    }
};

template<typename T>
auto makeDestroyable(T value, auto fn) {
    return Destroyable<T>{{value}, fn};
}

template<typename T>
using Predicate = std::function<bool(T)>;


enum class Flow {
    CONTINUE,
    BREAK
};

template<typename T>
using Visitor = std::function<Flow(T)>;

bool fuckingTrue(auto v) {
    return v;
}

bool all(const auto v) {
    return std::ranges::all_of(v.begin(), v.end());
}

bool cmp(const auto& as, const auto& bs) {
    // FIXME this is fucking retarded but still better than stl :)
    auto aa = collectVec(as);
    auto bb = collectVec(bs);

    if (aa.size() != bb.size()) return false;

    for (auto i = 0u; i < aa.size(); i++) {
        if (aa[i] != bb[i]) return false;
    }

    return true;
}

template<typename T>
struct Cleanup {
    T fn;

    ~Cleanup() {
        fn();
    }
};

template<typename T>
auto makeCleanup(T&& fn) {
    return Cleanup<T>(std::forward<T>(fn));
}

template<typename T>
auto byteSpan(T& v) -> span<u8, sizeof(T)> {
    return span<u8, sizeof(T)>{reinterpret_cast<u8*>(&v), sizeof(T)};
}

static size_t quickHash(span<u8> data) {
    size_t hash = 0;

    for (u8 byte: data) {
        hash += (byte ^ data.size()) * 31;
    }

    return hash;
}

static bool quickCmp(span<u8> a, span<u8> b) {
    for (auto [c1, c2]: views::zip(a, b)) {
        if (c1 != c2) return false;
    }

    return true;
}

inline constexpr size_t align(size_t value, size_t amount) {
    if (value % amount == 0) return value;
    return ((value / amount)*amount) + amount;
}

inline constexpr size_t alignFix(size_t value, size_t amount) {
    return value % amount;
}

inline constexpr bool isAligned(size_t value, size_t amount) {
    return (value % amount) == 0;
}

inline string randomString(string base) {
    return stringify("{}{}", base, to_string(rand()));
}

inline string randomString() {
    return to_string(rand());
}

#define KB 1024
#define MB (1024*KB)
#define GB (1024*MB)

namespace vipl {
    auto max(auto a, auto b) {
        return a > b ? a : b;
    }

    auto min(auto a, auto b) {
        return a > b ? a : b;
    }
}

using namespace std::literals;

struct string_hash {
    using hash_type = std::hash<std::string_view>;
    using is_transparent = void;

    std::size_t operator()(const char* str) const        { return hash_type{}(str); }
    std::size_t operator()(std::string_view str) const   { return hash_type{}(str); }
    std::size_t operator()(std::string const& str) const { return hash_type{}(str); }
};

template<typename T>
using HashMap = unordered_map<string, T, string_hash, std::equal_to<>>;

template<typename T>
using HashMapView = unordered_map<string_view, T>;

inline long stringToLong(const char* str, bool& fail, int base = 10) {
    char* pEnd = nullptr;
    auto d = strtol(str, &pEnd, base);

    if (*pEnd) fail = true;

    return d;
}

const constexpr size_t SIZET_MAX = ~static_cast<size_t>(0);


inline size_t indexOf(auto& cont, auto value) {
    for (auto i : views::iota(0u, cont.size())) {
        if (cont[i] == value) return i;
    }
    PANIC();
}

inline size_t difference(size_t a, size_t b) {
    auto ma = max(a, b);
    auto mi = min(a, b);

    return ma-mi;
}

inline bool contains(auto& cont, auto v) {
    return std::find(cont.begin(), cont.end(), v) != cont.end();
}

template<typename FN>
struct Find : std::ranges::range_adaptor_closure<Find<FN>> {
    FN func;

    template<typename IN1>
    constexpr auto operator()(IN1&& input) const -> optional<std::remove_reference_t<decltype(*input.begin())>> {
        for (const auto& v : input) {
            if (func(v)) return v;
        }
        return nullopt;
    }
};

template<typename FN>
struct FindIndex : std::ranges::range_adaptor_closure<Find<FN>> {
    FN func;

    template<typename IN1>
    constexpr auto operator()(IN1&& input) const -> optional<size_t> {
        for (const auto& [index, v] : input | views::enumerate) {
            if (func(v)) return index;
        }
        return nullopt;
    }
};

template<typename FN>
struct Any : std::ranges::range_adaptor_closure<Any<FN>> {
    FN func;

    template<typename IN1>
    constexpr auto operator()(IN1&& input) const {
        for (const auto& v : input) {
            if (func(v)) return true;
        }
        return false;
    }
};

template<typename FN>
struct All : std::ranges::range_adaptor_closure<All<FN>> {
    FN func;

    template<typename IN1>
    constexpr auto operator()(IN1&& input) const {
        for (const auto& v : input) {
            if (!func(v)) return false;
        }
        return true;
    }
};

struct ToVector: std::ranges::range_adaptor_closure<ToVector> {
    template<typename IN1>
    constexpr auto operator()(IN1&& input) const {
        vector<std::remove_cvref_t<decltype(*input.begin())>> res;
        for (const auto& v : input) {
            res.push_back(v);
        }
        return res;
    }
};

struct ToSet: std::ranges::range_adaptor_closure<ToSet> {
    template<typename IN1>
    constexpr auto operator()(IN1&& input) const {
        set<std::remove_cvref_t<decltype(*input.begin())>> res;
        for (const auto& v : input) {
            res.insert(v);
        }
        return res;
    }
};

template<typename FN>
struct None : std::ranges::range_adaptor_closure<None<FN>> {
    FN func;

    template<typename IN1>
    constexpr auto operator()(IN1&& input) const {
        for (const auto& v : input) {
            if (func(v)) return false;
        }
        return true;
    }
};

template<typename FN>
struct _Map : std::ranges::range_adaptor_closure<_Map<FN>> {
    FN func;

    template<typename IN1>
    constexpr auto operator()(IN1&& input) const -> optional<std::remove_reference_t<decltype(*input)>> {
        if (input.has_value()) {
            return {func(*input)};
        }
        return nullopt;
    }
};

template<typename FN>
struct Elvis : std::ranges::range_adaptor_closure<Elvis<FN>> {
    FN func;

    template<typename IN1>
    constexpr auto operator()(IN1&& input) const -> std::remove_reference_t<decltype(*input)> {
        if (input.has_value()) {
            return *input;
        }
        return func();
    }
};

namespace vi {
    template<typename T>
    auto makeFind(T fn) {
        return Find<T>{.func=fn};
    }

    template<template<typename C> typename A, typename T>
    auto maker(T&& fn) {
        return A<T>{.func=fn};
    }

    template<typename T>
    auto find(T&& v) {
        return Find<T>{.func=v};
    }

    inline ToVector toVec = ToVector{};
    inline ToSet toSet = ToSet{};

    template<typename T>
    auto findIndex(T&& v) {
        return FindIndex<T>{.func=v};
    }

    template<typename T>
    auto any(T&& v) {
        return Any<T>{.func=v};
    }
    template<typename T>
    auto all(T&& v) {
        return All<T>{.func=std::forward<T>(v)};
    }
    template<typename T>
    auto none(T&& v) {
        return None<T&&>{.func=std::forward<T>(v)};
    }
    template<typename T>
    auto map(T&& v) {
        return _Map<T>{.func=v};
    }
    template<typename T>
    auto map_null(T&& v) {
        return Elvis<T>{.func=v};
    }
}

template<typename T, typename... Other>
struct First {
    using TYPE = T;
};

template<typename... Args>
auto make_vector(Args&&... args) {
    vector<std::remove_cvref_t<typename First<Args...>::TYPE>> vec;

    (vec.emplace_back(std::forward<Args>(args)), ...);

    return vec;
}

template<typename T>
bool canFit(T value, u64 bits) {
    // maybe we can use log2?
    if constexpr (std::is_signed_v<T>) {
        u64 zero = 0;
        auto idk = static_cast<i64>(1) << 63;

        i64 min = idk >> (64-bits);
        i64 max = bit_cast<i64>(~zero >> (65-bits));

        // cout << "WTF: " << min << " " << max << endl;

        return value >= min && value <= max;
    }
    else {
        u64 max = 0;
        max = ~max;
        max = max >> (64-bits);
        return value <= max;
    }
}

inline string readFile(const string& filePath) {
    stringstream data;

    ifstream file;
    file.open(filePath, ios::in);
    data << file.rdbuf();

    return data.str();
}


#define vi_map(pepa) vi::map([](auto&& it) pepa)
#define vi_find(pepa) vi::find([](auto&& it) pepa)
#define vi_map_none(pepa) vi::map_null([]() pepa)
#define vi_transform(pepa) views::transform([](auto&& it) pepa)

// https://stackoverflow.com/questions/664014/what-integer-hash-function-are-good-that-accepts-an-integer-hash-key
inline unsigned int hashInt(u64 x) {
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    x = x ^ (x >> 31);
    return x;
}

constexpr static size_t numberOfOnes(size_t c) {
    size_t ones = 0;

    for (auto i = 0u; i < c; i++) {
        ones = ones << 1;
        ones |= 1;
    }

    return ones;
}

constexpr size_t SIZE_T_BITS = sizeof(size_t)*8;