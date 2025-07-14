#pragma once
#include <string_view>

class Debuggable {
public:
    [[nodiscard]] virtual const std::string_view className() const = 0;
};