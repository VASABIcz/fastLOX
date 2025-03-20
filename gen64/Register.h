#pragma once
#include <string>

using namespace std;

class Register {
public:
    [[nodiscard]] virtual string toString() const = 0;
    enum class SaveType {
        Caller,
        Callee,
        None
    };

    virtual ~Register() = default;

    virtual SaveType getSaveType() const {
        return SaveType::None;
    }
};