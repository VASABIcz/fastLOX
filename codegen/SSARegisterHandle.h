#pragma once

#include <string>

using namespace std;

class SSARegisterHandle {
public:
    SSARegisterHandle(size_t graphId, size_t regId, bool valid);

    explicit SSARegisterHandle();

    bool operator<(SSARegisterHandle const& other) const;

    bool operator==(SSARegisterHandle const& other) const;

    static SSARegisterHandle invalid();

    static SSARegisterHandle valid(size_t graphId, size_t regId);

    [[nodiscard]] bool isValid() const;

    [[nodiscard]] string toString() const;

    [[nodiscard]] string toTextString() const;

    size_t graphId;
    size_t registerId;
private:
    bool mValid;
};