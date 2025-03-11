#pragma once
#include <string_view>
#include "Position.h"

using namespace std;

template<typename T>
class Token {
public:
    string_view content;
    Position position;
    T type;

    explicit Token(Position position, string_view content, T type) : content(content), position(position), type(type) {
    }

    string toString() const {
        return stringify("Token{ content: {}, type: {}, pos: {} }", content, type, position.index);
    }
};