#include "RegAlloc.h"
#include "../../utils/utils.h"

void RegAlloc::freeReg(size_t handle) {
    // println("[REG] deallocated {}", regs64[handle]);
    assert(!isStack(handle));
    auto stripedBits = (handle << 4) >> 4;
    assert(stripedBits < regs64.size());
    regs[stripedBits] = false;
}

bool RegAlloc::isAcquired(const X64Register& reg) const {
    for (auto i : views::iota(0u, regs64.size())) {
        if (regs64[i] == reg) {
            return regs[i];
        }
    }
    return true;
}

bool RegAlloc::hasFreeReg() const {
    for (auto i : views::iota(0u, GENERAL_PURPOUSE_REG_COUNT)) {
        if (!regs[i]) return true;
    }

    return false;
}

size_t RegAlloc::allocateReg(size_t size) {
    for (auto i : views::iota(0u, GENERAL_PURPOUSE_REG_COUNT)) {
        if (!regs[i]) {
            regs[i] = true;

            mClobberedRegs.insert(regs64[i]);
            // println("[REG] allocated: {}", regs64[i]);

            if (size == 4) {
                return i | IS_INT_BIT;
            }
            if (size == 2) {
                return i | IS_SHORT_BIT;
            }
            if (size == 1) {
                return i | IS_BYTE_BIT;
            }
            // FIXME
            // assert(size == 8);
            return i;
        }
    }

    // stack alloc fallback
    // return allocateStack(8);
    TODO();
}

pair<size_t, X64Register> RegAlloc::allocReg() {
    auto regHandle = allocateReg(8);

    return {regHandle, regs64[regHandle]};
}

X64Register RegAlloc::acquireAny() {
    auto index = allocateReg(8);

    X64Register& reg = regs64[index];
    mClobberedRegs.insert(reg);

    return reg;
}

void RegAlloc::freeReg(const X64Register& reg) {
    for (auto i : views::iota(0u, regs64.size())) {
        if (regs64[i] == reg) {
            freeReg(i);
            return;
        }
    }
}

size_t RegAlloc::acquireSpecific(const X64Register& reg) {
    mClobberedRegs.insert(reg);

    auto res = ranges::find(regs64, reg);
    assert(res != regs64.end());
    size_t i = res - regs64.begin();

    assert(i < regs64.size());

    assert(!regs[i]);
    regs[i] = true;

    return i;
}

X64Register RegAlloc::getReg(size_t handle) const {
    if (isStack(handle)) PANIC();
    auto stripedBits = (handle << 4) >> 4;
    if (stripedBits >= regs64.size()) PANIC();
    return regs64[stripedBits];
}

map<X64Register, size_t> RegAlloc::saveCallRegs(span<const X64Register> exclude, const std::function<X64Register::SaveType(const X64Register&)>& convention) {
    map<X64Register, size_t> state;

    for (auto i : views::iota(0u, regs.size())) {
        const auto& reg = regs64[i];
        if (std::ranges::find(exclude, reg) != exclude.end()) continue;
        auto saveType = convention(reg);
        if (regs[i] && saveType == X64Register::SaveType::Caller) {
            auto stackHandle = allocateStack(REG_SIZE);
            state.emplace(reg, stackHandle);
        }
    }

    return state;
}

void RegAlloc::restoreCallRegs(const map<X64Register, size_t >& state) {
    for (const auto& item : state) {
        freeHandle(item.second);
    }
}

void RegAlloc::freeHandle(size_t handle) {
    if (isStack(handle)) {
        freeStack(handle);
    }
    else {
        freeReg(handle);
    }
}

RegAlloc::RegAlloc() {
#if 0
        regs64 = {
                X64Register::R15,
                X64Register::R13,
                X64Register::R14,
                X64Register::R8,
                X64Register::R9,
                X64Register::R10,
                X64Register::R11,
                X64Register::R12,
                X64Register::Rbx,
                X64Register::Rcx,
                X64Register::Rdx,
                X64Register::Rsi,
                X64Register::Rdi,
                X64Register::Rax
        };
#else
    // ignore arg regs Rdi, Rsi, Rdx, Rcx, R8, R9
    // note arg regs still need to be in regs64 bcs their handle is allocated
    regs64 = {
        X64Register::R10,
        X64Register::R11,
        X64Register::R12,
        X64Register::R13,
        X64Register::R14,
        X64Register::R15,
        X64Register::Rbx,
        X64Register::Rax,
        X64Register::R8,
        X64Register::R9,
        X64Register::Rcx,
        X64Register::Rdx,
        X64Register::Rsi,
        X64Register::Rdi,
    };
#endif
    for (const auto& _ : regs64) {
        (void)_;
        regs.push_back(false);
    }
}

void RegAlloc::clear() {
    for (auto reg : regs) {
        reg = false;
    }

    this->stack = {};
    this->mClobberedRegs = {};
}

const set<X64Register>& RegAlloc::clobberedRegs() const {
    return mClobberedRegs;
}

size_t RegAlloc::allocateAny(size_t sizeBytes) {
    if (hasFreeReg() && sizeBytes <= REG_SIZE)
        return allocateReg(sizeBytes);
    else
        return allocateStack(sizeBytes);
}

pair<X64Register, bool> RegAlloc::allocateEvenClobered(set<X64Register>& ignoreStackSpill) {
    auto available = availableRegs();

    for (auto av : available) {
        if (ignoreStackSpill.contains(av)) continue;
        acquireSpecific(av);
        return {available.front(), false};
    }

    for (auto reg : regs64) {
        if (ignoreStackSpill.contains(reg)) continue;

        return {reg, true};
    }

    std::terminate();
}

vector<X64Register> RegAlloc::availableRegs() {
    vector<X64Register> buf;

    for (auto i : views::iota(0u, GENERAL_PURPOUSE_REG_COUNT)) {
        if (isAvailable(i)) {
            buf.push_back(regs64[i]);
        }
    }

    return buf;
}

string RegAlloc::debugString(size_t handle) const {
    if (isStack(handle)) {
        return stringify("rsp+{}", getStackOffset(handle));
    }
    return getReg(handle).toString();
}