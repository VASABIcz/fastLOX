#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <expected>

#include "Debuggable.h"

class ExceptionContext {
public:
    ExceptionContext() = default;
};

class Errorable: public Debuggable {
public:
    virtual std::string message(ExceptionContext&) = 0;
    virtual std::string_view category() const = 0;
    virtual std::string_view type() const = 0;

    virtual ~Errorable() = default;
};

template<typename T>
using Result = std::expected<T, std::unique_ptr<Errorable>>;