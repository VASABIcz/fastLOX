#include "SSARegisterHandle.h"
#include "../utils/stringify.h"
#include "../utils/utils.h"

SSARegisterHandle::SSARegisterHandle(size_t graphId, size_t regId, bool valid) : graphId(graphId), registerId(regId), mValid(valid) {}

SSARegisterHandle::SSARegisterHandle() : graphId(0), registerId(0), mValid(false) {}

bool SSARegisterHandle::operator<(const SSARegisterHandle& other) const {
    if (graphId != other.graphId) return graphId < other.graphId;

    return registerId < other.registerId;
}

bool SSARegisterHandle::operator==(const SSARegisterHandle& other) const {
    return other.registerId == registerId && other.graphId == graphId;
}

SSARegisterHandle SSARegisterHandle::invalid() {
    // NOTE
    // this should be enough to fix doble counting of registers
    // visit src passes even invalid reg handle the consumer of that doesn't check that, so it peers like it uses reg 0:0
    return {SIZET_MAX, SIZET_MAX, false};
}

SSARegisterHandle SSARegisterHandle::valid(size_t graphId, size_t regId) {
    return {graphId, regId, true};
}

bool SSARegisterHandle::isValid() const {
    return mValid && graphId != SIZET_MAX;
}

string SSARegisterHandle::toString() const {
    if (!isValid()) {
        return "_";
    }
    return stringify("{}:{}", graphId, registerId);
}

string SSARegisterHandle::toTextString() const {
    if (!isValid()) {
        return " _ ";
    }
    return stringify("{}:{}", graphId, registerId);
}
