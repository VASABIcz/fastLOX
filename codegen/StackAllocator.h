#pragma once
#include "Assembler.h"

using namespace std;

struct StackAllocator {
    static constexpr size_t MOST_SIGNIFICANT_BIT = ~(~static_cast<size_t>(0u) >> 1);

    vector<bool> stack;
    // handle - size
    map<size_t, size_t> stackAllocations;

    size_t stackSize(size_t handle) {
        assert(isStack(handle));

        return stackAllocations[handle];
    }

    size_t stackSize() const {
        return stack.size();
    }

    void forceReserve(size_t nBytes) {
        for (auto i = 0u; i < nBytes; i++) {
            stack.push_back(false);
        }
    }

    size_t getStackOffset(size_t handle) const {
        assert(this->stackAllocations.contains(handle));
        return handle & ~MOST_SIGNIFICANT_BIT;
    }

    size_t allocateStack(size_t amountBytes) {
        const auto canAllocateStackRange = [&](auto start, auto size) {
            for (auto i: views::iota(start, start + size)) {
                if (stack[i]) return false;
            }
            return true;
        };
        const auto allocateStackRange = [&](auto start, auto size) {
            for (auto i: views::iota(start, start + size)) {
                stack[i] = true;
            }
        };
        const auto allocateStackHandle = [&](auto stackOffset) {
            auto handle = stackOffset | MOST_SIGNIFICANT_BIT;
            stackAllocations[handle] = amountBytes;
            return handle;
        };

        for (auto i: views::iota(0u, stack.size())) {
            auto ref = stack[i];
            if (!ref && (i+amountBytes <= stack.size()) && canAllocateStackRange(i, amountBytes)) {
                allocateStackRange(i, amountBytes);

                return allocateStackHandle(i);
            }
        }

        auto allocationStart = stack.size();
        for (auto i: views::iota(0u, amountBytes)) {
            (void) i;
            stack.push_back(true);
        }

        return allocateStackHandle(allocationStart);
    }

    void freeStack(size_t handle) {
        auto rawOffset = getStackOffset(handle);
        // println("[REG] freeing stack {}-{}", rawOffset, amountBytes);
        for (auto i: views::iota(rawOffset, rawOffset + stackSize(handle))) {
            stack[i] = false;
        }
    }

    void dumpStack() {
        println("=== alloc ===");
        println("{}", stackAllocations);
        println("{}", stack);
        println("=== end alloc ===");
    }

    static bool isStack(size_t handle) {
        // is the most significant bit set?
        return (handle & MOST_SIGNIFICANT_BIT) == MOST_SIGNIFICANT_BIT;
    }
};
