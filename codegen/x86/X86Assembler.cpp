#include <variant>
#include <iostream>
#include <fstream>
#include "X86Assembler.h"

#include "../../utils/utils.h"
#include "../../utils/code_gen.h"
#ifdef __unix__
#define LINUX
#endif

// TODO mov cache

typedef X64Register Reg;

void insertBytes(vector<u8>& target, const vector<u8>& source, size_t offset) {
    target.insert(target.begin()+offset, source.begin(), source.end());
}

void removeBytes(vector<u8>& target, size_t offset, size_t amount) {
    target.erase(target.begin()+offset, target.begin()+offset+amount);
}

struct CodeGroup {
    vector<u8>& bytes;
    map<string, Label>& labels;
    map<string, vector<Label>>& spaces;

    explicit CodeGroup(vector<u8>& bytes, map<string, Label>& labels, map<string, vector<Label>>& spaces) : bytes(
            bytes), labels(labels), spaces(spaces) {}

    void insertBytes(const vector<u8>& source, size_t offset) {
        ::insertBytes(bytes, source, offset);

        for (auto& label : labels | views::values) {
            if (label.index >= (long) offset) {
                label.index += source.size();
            }
        }
        for (auto& spacs : spaces | views::values) {
            for (auto& space: spacs) {
                if (space.index >= (long) offset) {
                    space.index += source.size();
                }
            }
        }
    }

    void removeBytes(size_t amout, size_t offset) {
        ::removeBytes(bytes, offset, amout);

        for (auto& label : labels | views::values) {
            if (label.index >= (long) offset) {
                label.index -= amout;
            }
        }
        for (auto& spacs : spaces | views::values) {
            for (auto& space: spacs) {
                if (space.index >= (long) offset) {
                    space.index -= amout;
                }
            }
        }
    }
};

u8 roundToNs(u8 n) {
    if (n <= 8) {
        return 1;
    }
    if (n <= 16) {
        return 2;
    }
    if (n <= 32) {
        return 4;
    }
    return 8;
}

template<typename T>
void rawWrite(u8* data, T value) {
    auto ptr = reinterpret_cast<u8*>(&value);

    for (auto i = 0u; i < sizeof(T); i++) {
        data[i] = ptr[i];
    }
}

Result<void> linkRelative(u8* data, const map<string , vector<Label>>& missing, const map<string, Label>& labels) {
    // println("SPACES {}", missing);
    // println("LABELS {}", labels);

    for (const auto& [name, spaces] : missing) {
        for (const auto& space : spaces) {
            if (space.type != Label::Type::GOT && space.type != Label::Type::JUMP) continue;
            if (!labels.contains(name)) {
                println("[WARNING] label \"{}\" not found (this can either be bug or its never referenced)", name);
                // return FAILF("label \"{}\" not found", mName);
                continue;
            }

            const auto& tag = labels.at(name);

            unsigned char convSize = roundToNs(static_cast<u8>(countl_zero(static_cast<u32>(tag.size)) - 24)); // converting u8 to u32 (24 additional zeros)
            if (convSize > space.size) return FAILF("symbol is dataSize of {} bytes, but space is dataSize of {} bytes tag: {}", convSize, space.size, name);
            long offset = static_cast<long>(tag.index)- (static_cast<long>(space.index) + static_cast<long>(space.size));

            if (space.type == Label::Type::GOT) {
                offset += space.data*8;
            }

            // println("[LINK] offset: {} {} {} {}", offset, space.index, space.size, tag.index);

            rawWrite(data+space.index, bit_cast<u32>(static_cast<int>(offset)));
        }
    }

    return {};
}


void X86Assembler::generateRet() {
    makeLabel(nextReturnLabel(), Label::Type::ABSOLUTE1, 0);
    mc.leave();
    mc.writeSimple(SimpleX64Instruction::ret);
}

void X86Assembler::generateRet(RegisterHandle value) {
#ifdef LINUX
    if (retSize <= REG_SIZE) {
        movHandleToReg(X64Register::Rax, value);
    } else if (retSize <= REG_SIZE*2) {
        assert(allocator.isStack(value));

        mc.readMem(X64Register::Rax, X64Register::Rsp, allocator.getStackOffset(value), REG_SIZE);
        mc.readMem(X64Register::Rdx, X64Register::Rsp, allocator.getStackOffset(value)+8, allocator.sizeOf(value)-REG_SIZE);
    } else {
        assert(allocator.isStack(value));

        // WHY ARE WE WRITING TO RDI ?????????
        // FIXME bcs we allocated the reg and never freed it OR didnt save it to stack to lower reg pressure
        // FIXME preserve it onto stack
        mc.movReg(X64Register::Rax, X64Register::Rdi);

        // rcx chosen just bcs its caller saved reg
        mc.garbageMemCpy(X64Register::Rax, X64Register::Rsp, allocator.stackSize(value), X64Register::Rcx, static_cast<int>(allocator.getStackOffset(value)));
    }
#else
    if (retSize <= REG_SIZE) {
        movHandleToReg(X64Register::Rax, value);
    } else {
        assert(allocator.isStack(value));

        // WHY ARE WE WRITING TO RDI ?????????
        // FIXME bcs we allocated the reg and never freed it OR didnt save it to stack to lower reg pressure
        // FIXME preserve it onto stack
        mc.movReg(X64Register::Rax, X64Register::Rcx);

        // rcx chosen just bcs its caller saved reg
        mc.garbageMemCpy(X64Register::Rax, X64Register::Rsp, allocator.stackSize(value), X64Register::Rcx, static_cast<int>(allocator.getStackOffset(value)));
    }
#endif

    generateRet();
}

void X86Assembler::jmpLabelTrue(RegisterHandle cond, string_view label) {
    withRegs([&](X64Register condReg) {
        mc.makeIsTrue(condReg);
    }, cond);
    writeJmp(CmpType::Equal, label);
}

void X86Assembler::jmpLabelFalse(RegisterHandle cond, string_view label) {
    withRegs([&](X64Register condReg) {
        mc.makeIsTrue(condReg);
    }, cond);
    writeJmp(CmpType::NotEqual, label);
}

void X86Assembler::jmp(string_view label) {
    writeJmp(label);
}

void X86Assembler::createLabel(string_view mName) {
    makeLabel(mName, Label::Type::JUMP);
}

void X86Assembler::print() const {}

void X86Assembler::freeRegister(Assembler::RegisterHandle rt) {
    allocator.freeHandle(rt);
}

X86Assembler::Assembler::RegisterHandle X86Assembler::allocateRegister(size_t size) {
    return allocator.allocateAny(size);
}

void X86Assembler::readMem(Assembler::RegisterHandle tgt, Assembler::RegisterHandle obj, size_t offset, size_t amount) {
    // mov tgt, [obj+offset]
    withRegs([&](X64Register tgtReg, X64Register objReg) {
        if (amount > 8) {
            assert(allocator.isStack(tgt));

            mc.garbageMemCpy(X64Register::Rsp, objReg, amount, tgtReg, static_cast<int>(offset), static_cast<int>(allocator.getStackOffset(tgt)));
            return;
        }

        mc.readMem(tgtReg, objReg, offset, amount);

        // write back
        movRegToHandle(tgt, tgtReg);
    }, tgt, obj);
}

void X86Assembler::writeMem(Assembler::RegisterHandle obj, Assembler::RegisterHandle value, size_t offset, size_t amount) {
    // mov [obj+offset], value
    withRegs([&](X64Register objReg, X64Register valueReg) {
        if (amount > 8) {
            assert(allocator.isStack(value));

            mc.garbageMemCpy(objReg, X64Register::Rsp, amount, valueReg, static_cast<int>(allocator.getStackOffset(value)), static_cast<int>(offset));
            return;
        }

        mc.writeMem(objReg, valueReg, offset, amount);
    }, obj, value);
}

size_t X86Assembler::preserveCalleeRegs(const std::function<X64Register::SaveType(X64Register)>& save) {
    // println("labels before: {}", labels);
    vector<u8> temp;
    CodeGroup group(bytes, labels, spaces);
    X86mc tempAsm(temp);

    size_t stackSize = allocator.stackSize();

    assert(labels.contains(STACK_LABEL));
    auto managedStack = labels[STACK_LABEL].index;

    auto acumulatedStackSize = stackSize;
    auto clobberedRegs = allocator.clobberedRegs();

    // generate preservation code
    for (const auto& reg : clobberedRegs) {
        if (save(reg) != X64Register::SaveType::Callee) continue;

        tempAsm.writeStack(static_cast<int>(acumulatedStackSize), reg);
        acumulatedStackSize += REG_SIZE;
        allocator.forceReserveStack(REG_SIZE);
    }

    group.insertBytes(temp, managedStack);
    temp.clear();

    // generate restoration code
    for (const auto& [labelName, label] : labels) {
        if (!labelName.starts_with("return")) continue;
        temp.clear();

        auto acumulatedStackSize2 = stackSize;
        for (const auto& reg : clobberedRegs) {
            if (save(reg) != X64Register::SaveType::Callee) continue;

            tempAsm.readStack(static_cast<int>(acumulatedStackSize2), reg);

            acumulatedStackSize2 += REG_SIZE;
        }

        group.insertBytes(temp, label.index);
        temp.clear();
    }

    return acumulatedStackSize;
}

size_t X86Assembler::preserveCalleeRegs() {
#ifdef LINUX
    return  preserveCalleeRegs(sysVSave);
#else
    return  preserveCalleeRegs(fastCallSave);
#endif
}

void X86Assembler::movInt(Assembler::RegisterHandle dest, u64 value, size_t offsetBytes) {
    if (RegAlloc::isStack(dest)) {
        withTempReg([&](X64Register reg) -> void {
            mc.mov(reg, bit_cast<size_t>(value));

            movRegToStack(dest, static_cast<int>(offsetBytes), reg);
        }, {});
    }
    else {
        assert(offsetBytes == 0);
        mc.mov(allocator.getReg(dest), bit_cast<size_t>(value));
    }
}

void X86Assembler::movReg(Assembler::RegisterHandle dest, Assembler::RegisterHandle src, size_t destOffsetBytes, size_t srcOffsetBytes, size_t amount) {
    if (src == dest && destOffsetBytes == 0 && srcOffsetBytes == 0 && amount == REG_SIZE) {
        return;
    }
    if (amount == 0) {
        return;
    }

    if (!RegAlloc::isStack(dest) && !RegAlloc::isStack(src)) { // Reg2Reg
        auto srcReg = allocator.getReg(src);
        auto dstReg = allocator.getReg(dest);

        if (srcOffsetBytes == 0 && destOffsetBytes == 0) {
            movRegToReg(dstReg, srcReg, amount);
        } else {
            withTempReg([&](X64Register tmp) {
                // FIXME
                // copy value reg to tmp
                mc.movReg(tmp, srcReg);
                // strip of low bits
                mc.shiftRImm(tmp, srcOffsetBytes*8);
                // strip of high bits
                mc.shiftLImm(tmp, 64-amount*8);
                // move value to proper place
                mc.shiftRImm(tmp, (64-amount*8)-destOffsetBytes*8);
                // or with dst
                mc.writeRegInst(X64Instruction::Or, dstReg, tmp);
            }, array{srcReg, dstReg});
        }
        return;
    }
    if (RegAlloc::isStack(dest) && !RegAlloc::isStack(src)) { // Reg2Stack
        assert(srcOffsetBytes == 0);
        movRegToStack(dest, destOffsetBytes, allocator.getReg(src));
        return;
    }
    if (!RegAlloc::isStack(dest) && RegAlloc::isStack(src)) { // Stack2Reg
        assert(destOffsetBytes == 0);
        movStackToReg(allocator.getReg(dest), src, srcOffsetBytes);
        return;
    }

    assert(isAligned(amount, REG_SIZE));

    withTempReg([&](Reg a){
        mc.garbageMemCpy(X64Register::Rsp, X64Register::Rsp, amount, a, static_cast<int>(allocator.getStackOffset(src) + srcOffsetBytes), static_cast<int>(allocator.getStackOffset(dest) + destOffsetBytes));
    }, {});
}

void X86Assembler::divInt(Assembler::RegisterHandle dest, Assembler::RegisterHandle left, Assembler::RegisterHandle right) {
    withSpecificReg(X64Register::Rax, [&] {
        withSpecificReg(X64Register::Rdx, [&] {
            movHandleToReg(X64Register::Rax, left);
            mc.cqo();
            withRegs([&](X64Register rhsReg) {
                mc.signedDivide(rhsReg);
            }, right);
            movRegToHandle(dest, X64Register::Rax);
        });
    });
}

void X86Assembler::shlInt(Assembler::RegisterHandle dest, Assembler::RegisterHandle left, Assembler::RegisterHandle right) {
    movReg(dest, left);

    withSpecificReg(X64Register::Rcx, [&] {
        movHandleToReg(X64Register::Rcx, right);
        withRegs([&](X64Register dstReg) {
            mc.shiftLeftByCl(dstReg);
            movRegToHandle(dest, dstReg);
        }, dest);
    });
}

void X86Assembler::shrInt(Assembler::RegisterHandle dest, Assembler::RegisterHandle left, Assembler::RegisterHandle right) {
    movReg(dest, left);

    withSpecificReg(X64Register::Rcx, [&] {
        movHandleToReg(X64Register::Rcx, right);
        withRegs([&](X64Register dstReg) {
            mc.shiftRightByCl(dstReg);
            movRegToHandle(dest, dstReg);
        }, dest);
    });
}

void X86Assembler::modInt(RegisterHandle dest, RegisterHandle left, RegisterHandle right) {
    withSpecificReg(X64Register::Rax, [&] {
        withSpecificReg(X64Register::Rdx, [&] {
            movHandleToReg(X64Register::Rax, left);
            mc.cqo();
            withRegs([&](X64Register rhsReg) {
                mc.signedDivide(rhsReg);
            }, right);
            movRegToHandle(dest, X64Register::Rdx);
        });
    });
}

void X86Assembler::nop() {
    mc.nop();
}

void X86Assembler::garbageMemCpy(X64Register ptrReg, size_t stackOffset, size_t stackSize) {
    // FIXME assert(isAligned(stackSize, REG_SIZE));

    withTempReg([&](X64Register tmp) {
        mc.garbageMemCpy(ptrReg, X64Register::Rsp, stackSize, tmp, static_cast<int>(stackOffset));
    }, {&ptrReg, 1});
}

size_t X86Assembler::writeStack(size_t offset, Assembler::RegisterHandle handle) {
    if (not RegAlloc::isStack(handle)) {
        auto r = allocator.getReg(handle);

        mc.writeStack(offset, r);
        return X86Assembler::REG_SIZE;
    }

    auto amount = allocator.stackSize(handle);
    auto srcOffset = allocator.getStackOffset(handle);
    // garbageMemCpy(X64Register::Rsp, srcOffset, amount); // FIXME WTF is this
    garbageMemCpy(X64Register::Rsp, offset, X64Register::Rsp, srcOffset, amount);
    return amount;
}

X86Assembler::X86Assembler(span<size_t> argSizes, size_t retSize) : mc(bytes), argSizes(collectVec(argSizes)), retSize(retSize) {
    mc.nop();
    mc.nop();
    mc.nop();
    mc.nop();
    mc.nop();

#ifdef LINUX
    initializeSYSV();
#else
    initializeFastCall();
#endif
}

void X86Assembler::withSpecificReg(const X64Register& reg, const function<void()>& callback) {
    // GOOD we can aquire it
    if (!allocator.isAcquired(reg)) {
        allocator.acquireSpecific(reg);
        callback();
        allocator.freeReg(reg);
        return;
    }

    // still good we can move it to another reg
    if (allocator.hasFreeReg()) {
        auto saved = allocator.acquireAny();
        mc.movReg(saved, reg);
        callback();
        mc.movReg(reg, saved);
        allocator.freeReg(saved);
        return;
    }

    // bad :( we need to move it to stack
    // TODO maybe use allocate stack (check if free stack slot)
    mc.push(reg);
    callback();
    mc.pop(reg);
}

void X86Assembler::movHandleToReg(const X64Register& dst, Assembler::RegisterHandle src) {
    if (RegAlloc::isStack(src)) {
        movStackToReg(dst, src);
    }
    else {
        movRegToReg(dst, allocator.getReg(src), allocator.sizeOf(src));
    }
}

void X86Assembler::movToArgRegVIPLCallingConvention(const X64Register& dst, Assembler::RegisterHandle src, const map<X64Register, size_t>& preserved, bool moveAsIs) {
    if (not RegAlloc::isStack(src)) {
        auto r = allocator.getReg(src);

        // is reg in stack?
        if (preserved.contains(r)) {
            mc.readStack(allocator.getStackOffset(preserved.at(r)), dst);
        }
        else {
            mc.movReg(dst, r);
        }
        return;
    }

    // move ptr to
    if (moveAsIs) {
        mc.getStackPtr(dst, allocator.getStackOffset(src));
    }

    if (allocator.stackSize(src) > 8) {
        mc.getStackPtr(dst, allocator.getStackOffset(src));
    } else {
        movStackToReg(dst, src);
    }
}

void X86Assembler::patchStackSize(size_t stackSize) {
    for (const auto& stackLabel : spaces[STACK_SIZE_LABEL]) {
        patchLabel<i32>(stackLabel, stackSize);
    }
}

void X86Assembler::instructionNumberHint(size_t id) {
/*    bool err = false;
    auto str = to_string(id);
    auto num = stringToLong(str, err, 16);

    // TODO use one line instruction like mov [Rsp+40], 0x21
    mc.push(X64Register::Rax);
    mc.mov(X64Register::Rax, num);
    mc.pop(X64Register::Rax);*/
}

size_t X86Assembler::dumpToStack(X64Register reg) {
    auto handle = allocator.allocateStack(REG_SIZE);
    mc.writeStack(allocator.getStackOffset(handle), reg);

    return handle;
}

X64Register X86Assembler::idk(size_t handle, set<X64Register>& clobered, vector<pair<X64Register, optional<size_t>>>& toRestore) {
    if (!RegAlloc::isStack(handle)) {
        auto tmp = allocator.getReg(handle);
        clobered.insert(tmp);

        return tmp;
    }

    auto [reg, isClobered] = allocator.allocateEvenClobered(clobered);

    // ignore newly allocated reg from being allocated again
    clobered.insert(reg);

    if (isClobered) {
        // if we allocated used register persist it onto stack
        auto hand = dumpToStack(reg);
        toRestore.emplace_back(reg, hand);
    } else {
        toRestore.emplace_back(reg, nullopt);
    }

    // move stack variable to reg
    movHandleToReg(reg, handle);

    return reg;
}

X64Register X86Assembler::idk2(set<X64Register>& clobered, vector<pair<X64Register, optional<size_t>>>& toRestore) {
    auto [reg, isClobered] = allocator.allocateEvenClobered(clobered);

    if (isClobered) {
        auto hand = dumpToStack(reg);
        toRestore.emplace_back(reg, hand);
    } else {
        toRestore.emplace_back(reg, nullopt);
    }

    return reg;
}

void X86Assembler::restoreRegState(vector<pair<X64Register, optional<size_t>>>& regs) {
    for (const auto& [originalReg, stackHandle] : regs) {
        if (not stackHandle.has_value()) {
            allocator.freeReg(originalReg);
        }
        else {
            mc.readStack(allocator.getStackOffset(*stackHandle), originalReg);
            allocator.freeHandle(*stackHandle);
        }
    }
}

void X86Assembler::movRegToHandle(size_t dst, X64Register src) {
    if (RegAlloc::isStack(dst)) {
        movRegToStack(dst, 0, src);
    }
    else {
        auto dstReg = allocator.getReg(dst);
        auto size = allocator.sizeOf(dst);
        movRegToReg(dstReg, src, size);
    }
}

void X86Assembler::threeWayWrapper(size_t tgt, size_t lhs, size_t rhs, auto fun) {
    withRegs([&](X64Register tReg, X64Register lReg, X64Register rReg) {
        fun(tReg, lReg, rReg);

        // flush result to original destination
        movRegToHandle(tgt, tReg);
    }, tgt, lhs, rhs);
}

void X86Assembler::twoWayWrapper(size_t tgt, size_t lhs, size_t rhs, auto fun) {
    threeWayWrapper(tgt, lhs, rhs, [&](X64Register tReg, X64Register lReg, X64Register rReg) {
        mc.movReg(tReg, lReg);
        fun(tReg, rReg);
    });
}


void X86Assembler::withSavedCallRegs(span<const X64Register> exclude, span<const X64Register> excluceRestore, const std::function<X64Register::SaveType(const X64Register&)>& convention, const std::function<void(const map<X64Register, size_t>&)>& callback) {
    auto saved = allocator.saveCallRegs(exclude, convention);

    for (const auto& [reg, stackHandle]: saved) {
        mc.writeStack(allocator.getStackOffset(stackHandle), reg);
    }

    callback(saved);

    for (const auto& [reg, stackHandle]: saved) {
        if (std::ranges::find(excluceRestore, reg) != excluceRestore.end()) continue;
        mc.readStack(allocator.getStackOffset(stackHandle), reg);
    }

    // free allocated stack
    allocator.restoreCallRegs(saved);
}

unique_ptr<X86Assembler::Assembler> X86Assembler::create(span<size_t> argSizes, size_t retSize) {
    return make_unique<X86Assembler>(argSizes, retSize);
}

vector<X86Assembler::Assembler::RegisterHandle> X86Assembler::getArgHandles() {
    return argHandles;
}

void X86Assembler::f64ToInt(RegisterHandle dest, RegisterHandle value) {
    withRegs([&](X64Register dst, X64Register src) {
        mc.f64ToI64(dst, src);

        // flush result to original destination
        movRegToHandle(dest, dst);
    }, dest, value);
}

void X86Assembler::writeJmp(string_view label) {
    auto space = mc.writeJmp((u32)0);
    putRelative(label, space);
}

void X86Assembler::writeJmp(CmpType jmp, string_view label) {
    auto space = mc.writeJmp(jmp, (u32)0);
    putRelative(label, space);
}

void X86Assembler::putAbsolute(string_view name, ImmSpace space) {
    putLabel(name, space, Label::Type::ABSOLUTE1);
}

void X86Assembler::putLabel(string_view name, ImmSpace space, Label::Type type, size_t data) {
    spaces[string(name)].emplace_back(static_cast<long>(space.offset), static_cast<int>(space.size), type, data);
}

void X86Assembler::putRelative(string_view name, ImmSpace space) {
    putLabel(name, space, Label::Type::JUMP);
}

void X86Assembler::makeLabel(string_view name, Label::Type type, int size, size_t data) {
    // println("LABEL {} : {}", name, bytes.size());
    labels[string(name)] = Label{static_cast<int>(bytes.size()), size, type, data};
}

void X86Assembler::movLabel(const X64Register& dest, size_t value) {
    auto labelId = getLabelId(value);
    auto imm = mc.relativeRead(dest, 0);
    putGOT(labelId, imm);
}

void X86Assembler::movLabel(int offset, size_t name) {
    withTempReg([&](X64Register reg) {
        movLabel(reg, name);
        mc.writeStack(offset, reg);
    }, span<X64Register>{});
}

string X86Assembler::nextReturnLabel() {
    return stringify("return-{}", returnCounter++);
}

void X86Assembler::int2f64(RegisterHandle dest, RegisterHandle value) {
    withRegs([&](X64Register dst, X64Register src) {
        auto srcSize = allocator.sizeOf(value);

        if (srcSize == 1) {
            mc.movsx8(src, src);
        }
        else if (srcSize == 2) {
            mc.movsx16(src, src);
        }
        else if (srcSize == 4) {
            mc.movsx32(src, src);
        }
        else {
            assert(srcSize == 8);
        }

        mc.i64ToF64(dst, src);

        // flush result to original destination
        movRegToHandle(dest, dst);
    }, dest, value);
}

void X86Assembler::callC(size_t label, span<const RegisterHandle> args, optional<RegisterHandle> ret) {
    vector<Arg> argz;

    for (auto [i, arg] : args | views::enumerate) {
        argz.push_back(handleToArg(arg));
    }
    optional<Arg> idk;
    if (ret.has_value()) {
        idk = handleToArg(*ret);
    }

    chadCall(Arg::Symbol(label), argz, idk);
}

void X86Assembler::movSymbol(Assembler::RegisterHandle handle, size_t name) {
    if (RegAlloc::isStack(handle)) {
        movLabel(allocator.getStackOffset(handle), name);
    }
    else {
        movLabel(allocator.getReg(handle), name);
    }
}

void X86Assembler::arithmeticInt(ArithmeticOp op, Assembler::RegisterHandle tgt, Assembler::RegisterHandle lhs, Assembler::RegisterHandle rhs) {
    if (op == ArithmeticOp::DIV) {
        return divInt(tgt, lhs, rhs);
    }
    if (op == ArithmeticOp::MOD) {
        return modInt(tgt, lhs, rhs);
    }
    if (op == ArithmeticOp::SHL) {
        return shlInt(tgt, lhs, rhs);
    }
    if (op == ArithmeticOp::SHR) {
        return shrInt(tgt, lhs, rhs);
    }
    if (op == ArithmeticOp::EQ || op == ArithmeticOp::GT || op == ArithmeticOp::LS || op == ArithmeticOp::GE || op == ArithmeticOp::LE) {
        threeWayWrapper(tgt, lhs, rhs, [&](X64Register tReg, X64Register lReg, X64Register rReg) {
            switch (op) {
                case ArithmeticOp::EQ:
                    mc.cmpI(CmpType::Equal, tReg, lReg, rReg);
                    break;
                case ArithmeticOp::GT:
                    mc.cmpI(CmpType::Less, tReg, lReg, rReg);
                    break;
                case ArithmeticOp::LS:
                    mc.cmpI(CmpType::Greater, tReg, lReg, rReg);
                    break;
                case ArithmeticOp::GE:
                    mc.cmpI(CmpType::LessOrEqual, tReg, lReg, rReg);
                    break;
                case ArithmeticOp::LE:
                    mc.cmpI(CmpType::GreaterOrEqual, tReg, lReg, rReg);
                    break;
                default:
                    PANIC();
            }
        });
        return;
    }

    twoWayWrapper(tgt, lhs, rhs, [&](X64Register lhs, X64Register rhs) {
        switch (op) {
            case ArithmeticOp::ADD:
                mc.writeRegInst(X64Instruction::add, lhs, rhs);
                break;
            case ArithmeticOp::SUB:
                mc.writeRegInst(X64Instruction::sub, lhs, rhs);
                break;
            case ArithmeticOp::MUL:
                mc.writeRegInst(SusX64Instruction::imul, lhs, rhs);
                break;
            case ArithmeticOp::OR:
                mc.writeRegInst(X64Instruction::Or, lhs, rhs);
                break;
            case ArithmeticOp::AND:
                mc.writeRegInst(X64Instruction::And, lhs, rhs);
                break;
            case ArithmeticOp::XOR:
                mc.writeRegInst(X64Instruction::xorI, lhs, rhs);
                break;
            case ArithmeticOp::NEGATE:
                mc.writeRegInst(X64Instruction::xorI, lhs, rhs);
                break;
            case ArithmeticOp::DIV:
            case ArithmeticOp::MOD:
            case ArithmeticOp::SHR:
            case ArithmeticOp::SHL:
            case ArithmeticOp::EQ:
            case ArithmeticOp::GT:
            case ArithmeticOp::LS:
            case ArithmeticOp::GE:
            case ArithmeticOp::LE:
                PANIC();
        }
    });
}

void X86Assembler::arithmeticFloat(Assembler::ArithmeticOp op, Assembler::FloatingPointType type, Assembler::RegisterHandle tgt, Assembler::RegisterHandle lhs, Assembler::RegisterHandle rhs) {
    auto is64Bit = type == Assembler::FloatingPointType::Double;

    threeWayWrapper(tgt, lhs, rhs, [&](X64Register dst, X64Register lhs, X64Register rhs) {
        switch (op) {
            case ArithmeticOp::ADD:
                mc.addFloat(dst, lhs, rhs, is64Bit);
                break;
            case ArithmeticOp::SUB:
                mc.subFloat(dst, lhs, rhs, is64Bit);
                break;
            case ArithmeticOp::MUL:
                mc.mulFloat(dst, lhs, rhs, is64Bit);
                break;
            case ArithmeticOp::DIV:
                mc.divFloat(dst, lhs, rhs, is64Bit);
                break;
            case ArithmeticOp::EQ:
                mc.floatCompare(CmpType::Equal, dst, lhs, rhs, is64Bit);
                break;
            case ArithmeticOp::LS:
                mc.floatCompare(CmpType::Above, dst, lhs, rhs, is64Bit);
                break;
            case ArithmeticOp::GT:
                mc.floatCompare(CmpType::Above, dst, rhs, lhs, is64Bit);
                break;
            case ArithmeticOp::GE:
                mc.floatCompare(CmpType::NotBellow, dst, rhs, lhs, is64Bit);
                break;
            case ArithmeticOp::LE:
                mc.floatCompare(CmpType::NotBellow, dst, lhs, rhs, is64Bit);
                break;
            case ArithmeticOp::OR:
            case ArithmeticOp::AND:
            case ArithmeticOp::XOR:
            case ArithmeticOp::NEGATE:
            case ArithmeticOp::MOD:
            case ArithmeticOp::SHR:
            case ArithmeticOp::SHL:
                PANIC("unsuported arithmetic op");
        }
    });
}

void X86Assembler::garbageMemCpy(X64Register dst, size_t dstOffset, X64Register src, size_t srcOffset, size_t stackSize) {
    // FIXME assert(isAligned(stackSize, REG_SIZE));

    array<X64Register, 2> ignore{dst, src};

    withTempReg([&](X64Register tmp) {
        mc.garbageMemCpy(dst, src, stackSize, tmp, srcOffset, dstOffset);
    }, ignore);
}

template<typename T>
void X86Assembler::withTempReg(T callback, span<const X64Register> ign) {
    callWrapper(callback, &T::operator(), ign);
}

template<typename Ret, typename Class, typename... Args>
constexpr void X86Assembler::finallCallbackWrapper(Class obj, Ret (Class::*lambda)(Args...) const, Args... args) {
    (obj.*lambda)(args...);
}

template<typename Class, typename... Args>
constexpr void X86Assembler::callWrapper(Class obj, void (Class::*lambda)(Args...) const, span<const X64Register> ignore1) {
    vector<pair<X64Register, optional<size_t>>> restorCtx;
    set<X64Register> clobbered;

    for (auto ing : ignore1) {
        clobbered.insert(ing);
    }

    finallCallbackWrapper(obj, lambda, std::forward<Args>(idk2(clobbered, restorCtx))...);

    restoreRegState(restorCtx);
}

template<typename... Args, typename F>
void X86Assembler::withRegs(F fan, Args... args) {
    set<X64Register> clobered;
    vector<pair<X64Register, optional<size_t >>> toRestore;

    // put all reg args to clobbered to prevent them from being allocated
    for (auto handle : {args...}) {
        if (RegAlloc::isStack(handle)) continue;
        clobered.insert(allocator.getReg(handle));
    }

    fan(idk(args, clobered, toRestore)...);

    restoreRegState(toRestore);
}

void X86Assembler::addressOf(Assembler::RegisterHandle tgt, Assembler::RegisterHandle obj) {
    assert(allocator.isStack(obj));
    auto offset = allocator.getStackOffset(obj);

    withRegs([&](auto reg) {
        mc.getStackPtr(reg, offset);
        movRegToHandle(tgt, reg);
    }, tgt);
}

void X86Assembler::movRegToReg(const X64Register& dst, const X64Register& src, size_t size) {
    mc.movReg(dst, src);
    if (size == 1) {
        mc.writeMovZX8(dst, dst);
    }
    else if (size == 2) {
        mc.writeMovZX16(dst, dst);
    }
    // FIXME maybe even zx 32bit
}

void X86Assembler::movRegToStack(size_t handle, int _offset, const X64Register& src) {
    auto size = allocator.stackSize(handle);
    auto offset = allocator.getStackOffset(handle)+_offset;

    mc.writeStackAmount(offset, src, size);
}

void X86Assembler::movStackToReg(const X64Register& dst, size_t adrHandle, int srcOffset) {
    assert(allocator.isStack(adrHandle));
    auto size = allocator.stackSize(adrHandle);
    auto offset = allocator.getStackOffset(adrHandle)+srcOffset;

    if (size == 1) {
        mc.read1(dst, X64Register::Rsp, offset);
        mc.writeMovZX8(dst, dst);
    }
    else if (size == 2) {
        mc.read2(dst, X64Register::Rsp, offset);
        mc.writeMovZX16(dst, dst);
    }
    else if (size == 4) {
        mc.read4(dst, X64Register::Rsp, offset);
    }
    else if (size == 8) {
        mc.readStack(offset, dst);
    }
    else {
        // println("[WARN] trying to move {} from stack to reg -- moving only REG_SIZE", size);
        mc.readStack(offset, dst);
    }
}

void X86Assembler::freeHandles(span<const size_t> handles) {
    for (auto handle : handles) {
        allocator.freeHandle(handle);
    }
}

void X86Assembler::freeHandles(initializer_list<size_t> handles) {
    freeHandles({handles.begin(), handles.size()});
}

size_t X86Assembler::addressOf(size_t stackHandle) {
    auto dst = allocator.allocateAny(8);
    addressOf(dst, stackHandle);

    return dst;
}

void X86Assembler::signExtend(Assembler::RegisterHandle dst, Assembler::RegisterHandle src) {
    auto srcSize = allocator.sizeOf(src);
    auto dstSize = allocator.sizeOf(dst);
    (void)dstSize;
    assert(dstSize >= srcSize);

    withRegs([&](X64Register dstReg, X64Register srcReg) {
        // FIXME
        if (srcSize == 1) {
            mc.movsx8(dstReg, srcReg);
        } else if (srcSize == 2) {
            mc.movsx16(dstReg, srcReg);
        } else if (srcSize == 4) {
            mc.movsx32(dstReg, srcReg);
        } else {
        }

        if (allocator.isStack(dst)) {
            mc.writeStackAmount(allocator.getStackOffset(dst), dstReg, srcSize);
        }
    }, dst, src);
}

void X86Assembler::initializeSYSV() {
    const std::array<X64Register, 6> SYSV_REGS{X64Register::Rdi, X64Register::Rsi, X64Register::Rdx, X64Register::Rcx, X64Register::R8, X64Register::R9};
    const X64Register TMP_REG = X64Register::Rax;
    const auto STACK_ARGS_BASE = 16;

    mc.push(X64Register::Rbp);
    mc.movReg(X64Register::Rbp, X64Register::Rsp);

    auto label = mc.subImm32(X64Register::Rsp, 0x0);
    putAbsolute(STACK_SIZE_LABEL, label);

    createLabel(STACK_LABEL);

    size_t allocatedArgs = 0;
    size_t stackOffset = 0;

    // return cant fit into reg
    if (retSize > REG_SIZE*2) {
        assert(isAligned(retSize, REG_SIZE));
        // reserve Rdi for return ptr
        allocator.acquireSpecific(SYSV_REGS[allocatedArgs]);
        allocatedArgs += 1;
    }

    for (auto argSize : argSizes) {
        if (argSize <= REG_SIZE && allocatedArgs < SYSV_REGS.size()) {
            argHandles.push_back(allocator.acquireSpecific(SYSV_REGS[allocatedArgs]));
            allocatedArgs += 1;
        } else if (argSize <= REG_SIZE*2 && allocatedArgs <= SYSV_REGS.size()-2) {
            auto stack = allocator.allocateStack(REG_SIZE*2);

            mc.writeStackAmount(allocator.getStackOffset(stack), SYSV_REGS[allocatedArgs], REG_SIZE);
            mc.writeStackAmount(allocator.getStackOffset(stack)+REG_SIZE, SYSV_REGS[allocatedArgs+1], argSize-REG_SIZE);

            argHandles.push_back(stack);

            allocatedArgs += 2;
        } else {
            auto aligned = align(argSize, REG_SIZE);
            auto stack = allocateStack(aligned);

            mc.garbageMemCpy(X64Register::Rsp, X64Register::Rbp, aligned, TMP_REG, STACK_ARGS_BASE+stackOffset, allocator.getStackOffset(stack));

            argHandles.push_back(stack);

            stackOffset += aligned;
        }
    }
}

void X86Assembler::initializeFastCall() {
    const std::array<X64Register, 4> FAST_REGS{X64Register::Rcx, X64Register::Rdx, X64Register::R8, X64Register::R9};
    const X64Register TMP_REG = X64Register::Rax;
    const auto STACK_ARGS_BASE = 16;
    const auto STACK_RESERVED = REG_SIZE*FAST_REGS.size();

    mc.frameInit();

    auto label = mc.subImm32(X64Register::Rsp, 0x0);
    putAbsolute(STACK_SIZE_LABEL, label);

    createLabel(STACK_LABEL);

    size_t allocatedArgs = 0;
    size_t stackOffset = 0;

    // return cant fit into reg
    if (retSize > REG_SIZE) {
        assert(isAligned(retSize, REG_SIZE));
        // reserve Rdi for return ptr
        allocator.acquireSpecific(FAST_REGS[allocatedArgs]);
        allocatedArgs += 1;
    }

    for (const auto [argId, argSize] : argSizes | views::enumerate) {
        if (argSize <= REG_SIZE && allocatedArgs < FAST_REGS.size()) { // value is pased trough reg
            argHandles.push_back(allocator.acquireSpecific(FAST_REGS[allocatedArgs]));
            allocatedArgs += 1;
        } else if (allocatedArgs < FAST_REGS.size()) { // value passed by reference
            // FIXME for now just copy the value onto our stack
            auto reg = FAST_REGS[allocatedArgs];
            assert(isAligned(argSize, 8));

            auto stackHandle = allocator.allocateStack(argSize);
            argHandles.push_back(stackHandle);

            mc.garbageMemCpy(X64Register::Rsp, reg, argSize, TMP_REG, 0, allocator.getStackOffset(stackHandle));

            allocatedArgs += 1;
        } else { // value is passed through stack
            auto aligned = align(argSize, REG_SIZE);

            auto stack = allocateStack(aligned);
            argHandles.push_back(stack);


            // if somehow 0..3rd argument is passed over stack, its locations is special spot
            size_t dynamicOffset = STACK_RESERVED;
            if ((size_t)argId < FAST_REGS.size() && argSize <= 8) {
                dynamicOffset = argId*REG_SIZE;
            }

            mc.garbageMemCpy(X64Register::Rsp, X64Register::Rbp, aligned, TMP_REG, STACK_ARGS_BASE+dynamicOffset+stackOffset, allocator.getStackOffset(stack));

            stackOffset += aligned;
        }
    }
}

void X86Assembler::f32ToF64(Assembler::RegisterHandle dest, Assembler::RegisterHandle value) {
    withRegs([&](X64Register dst, X64Register src) {
        mc.floatToDouble(dst, src);

        // flush result to original destination
        movRegToHandle(dest, dst);
    }, dest, value);
}

void X86Assembler::f64ToF32(Assembler::RegisterHandle dest, Assembler::RegisterHandle value) {
    withRegs([&](X64Register dst, X64Register src) {
        mc.doubleToFloat(dst, src);

        // flush result to original destination
        movRegToHandle(dest, dst);
    }, dest, value);
}

size_t X86Assembler::getLabelId(size_t name) {
    for (const auto& [n, label] : absoluteLabels | views::enumerate) {
        if (label == name) return n;
    }
    absoluteLabels.push_back(name);
    return absoluteLabels.size()-1;
}

Arg X86Assembler::handleToArg(size_t handle) {
    if (RegAlloc::isStack(handle)) {
        return Arg::StackValue(allocator.getStackOffset(handle), allocator.sizeOf(handle));
    } else {
        return Arg::Reg(allocator.getReg(handle));
    }
}

Arg X86Assembler::handleToArgAssume8(size_t handle) {
    if (RegAlloc::isStack(handle)) {
        assert(allocator.sizeOf(handle) >= 8);
        return Arg::StackValue(allocator.getStackOffset(handle), 8);
    } else {
        return Arg::Reg(allocator.getReg(handle));
    }
}

size_t X86Assembler::calculateStackSizeFastCall(span<const RegisterHandle> args) {
    size_t stackSize = 0;
    size_t regsUsed = 0;

    for (auto arg : args) {
        if (allocator.sizeOf(arg) <= 8 && regsUsed < 4) {
            continue;
        }
        regsUsed++; // well this is windows for ya if you organice ur args badly u will lose regs :))
        stackSize += align(allocator.sizeOf(arg), 8);
    }

    return stackSize;
}

void X86Assembler::invokeScuffedSYSV(Arg func, span<Arg> args, optional<Arg> ret) {
    vector<X64Register> exclude;
    vector<X64Register> excludeRestore;

    if (ret.has_value() && ret->type == Arg::REGISTER) {
        bool contains = false;
        for (auto arg : args) {
            // if ret reg is the same as any arg reg, preserve it but dont restore it
            if (arg.type == Arg::REGISTER && arg.reg == ret->reg) {
                contains = true;
                break;
            }
        }
        if (contains) {
            excludeRestore.push_back(ret->reg);
        } else {
            exclude.push_back(ret->reg);
        }
    }

    withSavedCallRegs(exclude, excludeRestore, sysVSave, [&](const auto& saved){
        mc.invokeScuffedSYSV2(func, args, ret, saved, [&](auto dst, auto sym) {
            generateArgMove(dst, sym);
        });
    });
}

void X86Assembler::invokeScuffedFastCall(Arg func, span<Arg> args, optional<Arg> ret) {
    vector<X64Register> exclude;
    vector<X64Register> excludeRestore;

    if (ret.has_value() && ret->type == Arg::REGISTER) {
        bool contains = false;
        for (auto arg : args) {
            // if ret reg is the same as any arg reg, preserve it but dont restore it
            if (arg.type == Arg::REGISTER && arg.reg == ret->reg) {
                contains = true;
                break;
            }
        }
        if (contains) {
            excludeRestore.push_back(ret->reg);
        } else {
            exclude.push_back(ret->reg);
        }
    }

    withSavedCallRegs(exclude, excludeRestore, fastCallSave, [&](const auto& saved){
        mc.invokeScuffedFastCall(func, args, ret, saved, [&](auto dst, auto sym) {
                                     generateArgMove(dst, sym);
                                 }, [&](auto amount){ return allocator.getStackOffset(allocator.allocateStack(amount)); });
    });
}

void X86Assembler::chadCall(Arg fun, span<Arg> args, optional<Arg> ret) {
#ifdef LINUX
    invokeScuffedSYSV(fun, args, ret);
#else
    invokeScuffedFastCall(fun, args, ret);
#endif
}

CmpType X86Assembler::toCmpType(JumpCondType it) {
    switch (it) {
        case JumpCondType::EQUALS: return CmpType::Equal;
        case JumpCondType::NOT_EQUALS: return CmpType::NotEqual;
        case JumpCondType::GREATER: return CmpType::Greater;
        case JumpCondType::GREATER_OR_EQUAL: return CmpType::GreaterOrEqual;
        case JumpCondType::LESS: return CmpType::Less;
        case JumpCondType::LESS_OR_EQUAL: return CmpType::LessOrEqual;
    }
    PANIC()
}

CmpType X86Assembler::toCmpType2(JumpCondType it) {
    switch (it) {
        case JumpCondType::EQUALS: return CmpType::Equal;
        case JumpCondType::NOT_EQUALS: return CmpType::NotEqual;
        case JumpCondType::GREATER: return CmpType::Above;
        case JumpCondType::GREATER_OR_EQUAL: return CmpType::AboveEqual;
        case JumpCondType::LESS: return CmpType::Bellow;
        case JumpCondType::LESS_OR_EQUAL: return CmpType::BellowOrEqual;
    }
    PANIC()
}

void X86Assembler::jmpCond(string_view label, JumpCondType type, RegisterHandle lhs, RegisterHandle rhs) {
    withRegs([&](X64Register a, X64Register b) {
        mc.writeRegInst(X64Instruction::cmp, a, b);
    }, lhs, rhs);
    writeJmp(toCmpType(type), label);
}

