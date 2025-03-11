#pragma once

#include <cstdio>
#include <string>
#include "../utils/stringify.h"

using namespace std;

struct Position {
    size_t row;
    size_t column;
    size_t index;

    explicit Position(size_t row, size_t column, size_t index) : row(row), column(column), index(index) {

    }

    Position() = default;

    string toString() const {
        return stringify("{}:{} - {}", row, column, index);
    }
};