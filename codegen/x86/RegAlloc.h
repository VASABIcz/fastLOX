#pragma once

#include <bitset>
#include <functional>
#include <map>
#include <set>

#include "../StackAllocator.h"
#include "../../gen64/definitions.h"

class RegAlloc {
    vector<bool> regs;
    vector<X64Register> regs64;
    // handle - size
    set<X64Register> mClobberedRegs;
    StackAllocator stack;
    
    static constexpr size_t MOST_SIGNIFICANT_BIT = ~(~static_cast<size_t>(0u) >> 1);
    static constexpr size_t IS_INT_BIT = ~(~static_cast<size_t>(0u) >> 1) >> 1;
    static constexpr size_t IS_SHORT_BIT = ~(~static_cast<size_t>(0u) >> 1) >> 2;
    static constexpr size_t IS_BYTE_BIT = ~(~static_cast<size_t>(0u) >> 1) >> 3;
    static constexpr size_t REG_SIZE = 8;
    static constexpr size_t GENERAL_PURPOUSE_REG_COUNT = 14;

    void freeStack(size_t handle) {
        stack.freeStack(handle);
    }

    void freeReg(size_t handle);
public:
    void dumpStack() {
        stack.dumpStack();
    }

    string debugString(size_t handle) const;

    bool isAcquired(const X64Register& reg) const;

    bool hasFreeReg() const;

    bool isAvailable(size_t n) {
        return !regs[n];
    }

    void forceReserveStack(size_t nBytes) {
        stack.forceReserve(nBytes);
    }

    vector<X64Register> availableRegs();

    size_t allocateReg(size_t size);

    size_t allocateAny(size_t sizeBytes);

    pair<size_t, X64Register> allocReg();

    X64Register acquireAny();

    pair<X64Register, bool> allocateEvenClobered(set<X64Register>& ignoreStackSpill);

    void freeReg(const X64Register& reg);

    size_t acquireSpecific(const X64Register& reg);

    size_t allocateStack(size_t amountBytes) {
        return stack.allocateStack(amountBytes);
    }

    static bool isStack(size_t handle) {
        return StackAllocator::isStack(handle);
    }

    X64Register getReg(size_t handle) const;

    size_t getStackOffset(size_t handle) const {
        return stack.getStackOffset(handle);
    }

    map<X64Register, size_t> saveCallRegs(span<const X64Register> exclude, const std::function<X64Register::SaveType(const X64Register&)>& convention);

    void restoreCallRegs(const map<X64Register, size_t>& state);

    void freeHandle(size_t handle);

    size_t stackSize(size_t handle) {
        return stack.stackSize(handle);
    }

    size_t regSize(size_t handle) {
        if ((handle & IS_BYTE_BIT) == IS_BYTE_BIT) {
            return 1;
        }
        if ((handle & IS_SHORT_BIT) == IS_SHORT_BIT) {
            return 2;
        }
        if ((handle & IS_INT_BIT) == IS_INT_BIT) {
            return 4;
        }
        return 8;
    }

    size_t sizeOf(size_t handle) {
        if (isStack(handle)) {
            return stackSize(handle);
        }
        else {
            return regSize(handle);
        }
    }

    RegAlloc();

    void clear();

    size_t stackSize() const {
        return stack.stackSize();
    }

    [[nodiscard]] const set<X64Register>& clobberedRegs() const;
};

enum class ValueType {
    Register,
    Stack,
    Immediate
};
