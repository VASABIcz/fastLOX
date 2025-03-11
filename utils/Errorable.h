#pragma once

#include <complex>
#include <string>
#include <memory>
#include <expected>
#include <utility>

#include "Result.h"
#include "utils.h"

template<typename T>
struct Expected {
    expected<T, unique_ptr<Errorable>> inner;
};

using Err = std::unexpected<unique_ptr<Errorable>>;

#define FAILS [[nodisacrd("Result must be handled")]]

template<typename T>
using Ok = std::expected<T, unique_ptr<Errorable>>;

class MockError: public Errorable {
    DEBUG_INFO(MockError)
public:
    string message(ExceptionContext &) override {
        return "something wen wrong :(";
    }

    [[nodiscard]] string_view category() const override {
        return "mock";
    }

    [[nodiscard]] string_view type() const override {
        return "mock";
    }
public:
    MockError() = default;
};

class SimpleError: public Errorable {
    DEBUG_INFO(SimpleError)
public:
    SimpleError(string message): mMessage(std::move(message)) {
        cout << "mejking error " << this->mMessage << endl;
    }

    [[nodiscard]] string_view type() const override {
        return className();
    }

    [[nodiscard]] string_view category() const override {
        return "runtime";
    }

    string message(ExceptionContext &) override {
        return mMessage;
    }

private:
    string mMessage;
};

#define FAIL(msg) Err(make_unique<SimpleError>(FAIL_FORMAT+msg))
#define FAILF(msg, ...) Err(make_unique<SimpleError>(FAIL_FORMAT+stringify(msg, __VA_ARGS__)))


template<typename T>
auto makeExpected(T v) {
    return Ok<T>{v};
}

#define EXCEPT(x) ({auto _it = x; if (!_it.has_value()) { cerr << _it.error()->message({}); return; }; std::move(*_it); })
#define EXCEPTION(x, err) x.has_value() ? makeExpected(std::move(*x)) : FAIL(err)

#define EXCEPTNV(x) ({auto _err = ExceptionContext(); auto _it = x; if (!_it.has_value()) { cerr << "[ERR] " << _it.error()->message(_err); exit(0); }; })
#define EXCEPTN(x) ({auto _err = ExceptionContext(); auto _it = x; if (!_it.has_value()) { cerr << "[ERR] " << _it.error()->message(_err); exit(1); }; std::move(*_it); })

template<typename T, typename V>
Result<T*> cast(V i) {
    auto t  = dynamic_cast<T*>(i);
    if (t == nullptr)
        return FAIL("failed uniqueCast");

    return t;
}

template<typename T, typename V>
T* unsafeCast(V i) {
    auto t  = dynamic_cast<T*>(i);
    if (t == nullptr) std::terminate();

    return t;
}

template<typename T, typename V>
bool isType(V i) {
    auto t  = dynamic_cast<T*>(i);
    if (t == nullptr) return false;

    return true;
}

template<typename T, typename V>
Result<unique_ptr<T>> uniqueCast(unique_ptr<V> i) {
    T* t  = dynamic_cast<T*>(i.release());
    if (t == nullptr)
        return FAIL("failed uniqueCast");

    return unique_ptr<T>(t);
}