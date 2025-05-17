#pragma once

#include <map>
#include <vector>

#include "../utils/utils.h"
#include "../utils/Errorable.h"

using namespace std;

/*
 * 3 types
 * jump - relative offset to some symbol
 * absolute - at this address i need value of this symbol
 * GOT - at this address i need offset to GOT
 */

struct Label {
    enum class Type {
        ABSOLUTE1,
        JUMP,
        RIP_REL_32_ADR,
        RIP_REL_32_VAL
    };

    // why is index long?
    i64 index{};
    // why is SIZE int?
    int size{};
    Type type = Type::ABSOLUTE1;
    size_t data = 0;

    string toString() const {
        return stringify("Label({:x}, {})", index, size);
    }

    size_t uns() const {
        return bit_cast<size_t>(index);
    }

    long sig() const {
        return index;
    }
};

class BinaryOutput {
public:
    vector<u8> bytes;
    map<string, Label> labels;

    map<string, Label> spaces;
    BinaryOutput(vector<u8> bytes, map<string, Label> labels, map<string, Label> spaces): bytes(std::move(bytes)), labels(std::move(labels)), spaces(std::move(spaces)) {

    }

    Result<void> link();
};