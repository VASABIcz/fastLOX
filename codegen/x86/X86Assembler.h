#pragma once

#include <memory>
#include <map>
#include <ranges>
#include <cstring>

#include "../Assembler.h"
#include "../../gen64/definitions.h"
#include "../../gen64/X86mc.h"
#include "RegAlloc.h"

using namespace std;

// TODO
// lazy register allocation
// writes will not be executed until required
// instruction can treat the value as immediate
// TODO stack allocation
// values larger than reg SIZE store on stack, if not said otherwise

Result<void> linkRelative(u8* data, const map<string , vector<Label>>& missing, const map<string, Label>& labels);


class X86Assembler: virtual public Assembler {
public:
    RegAlloc allocator;
    vector<u8> bytes;
    map<string, Label> labels;
    map<string, vector<Label>> spaces;
    vector<size_t> absoluteLabels;
    X86mc mc;
    vector<size_t> argSizes;
    size_t retSize;
    vector<size_t> argHandles;

    static constexpr size_t REG_SIZE = 8;
    static constexpr size_t STACK_ALIGNMENT = 16;
    static constexpr string STACK_LABEL = "_STACK_";
    static constexpr string STACK_SIZE_LABEL = "_STACK_SIZE_";

    X86Assembler(span<size_t> argSizes, size_t retSize);

    void initializeSYSV();

    void initializeFastCall();

    static unique_ptr<Assembler> create(span<size_t> argSizes, size_t retSize);

    vector<RegisterHandle> getArgHandles() override;
    void print() const override;

    // int operations
    void divInt(RegisterHandle dest, RegisterHandle left, RegisterHandle right) override;
    void modInt(RegisterHandle dest, RegisterHandle left, RegisterHandle right) override;
    void shlInt(Assembler::RegisterHandle dest, Assembler::RegisterHandle left, Assembler::RegisterHandle right) override;
    void shrInt(Assembler::RegisterHandle dest, Assembler::RegisterHandle left, Assembler::RegisterHandle right) override;
    void arithmeticInt(ArithmeticOp op, Assembler::RegisterHandle tgt, Assembler::RegisterHandle lhs, Assembler::RegisterHandle rhs) override;

    // float operations
    void arithmeticFloat(ArithmeticOp op, Assembler::FloatingPointType type, Assembler::RegisterHandle tgt, Assembler::RegisterHandle lhs, Assembler::RegisterHandle rhs) override;

    // float int conversion
    void f64ToInt(RegisterHandle dest, RegisterHandle value) override;
    void int2f64(RegisterHandle dest, RegisterHandle value) override;

    /*void numericCast(Assembler::RegisterHandle dst, Assembler::RegisterHandle src, NumericType dstType, NumericType srcType) {
        if (srcType.isSigned() && dstType.isFloating()) {
            auto srcSize = allocator.sizeOf(src);
            withRegs([&](const X64Register& srcReg, const X64Register& dstReg) {
                if (srcSize == 8) {
                    mc.movsx8(srcReg, srcReg);
                }
                if (srcSize == 16) {
                    mc.movsx16(srcReg, srcReg);
                }
                if (srcSize == 32) {
                    mc.movsx32(srcReg, srcReg);
                }

                if (dstType.type == NumericType::F64) {
                    mc.i64ToF64(dstReg, srcReg);
                } else {
                    mc.i64ToF32(dstReg, srcReg);
                }

                // write back result
                movRegToHandle(dst, dstReg);
            }, src, dst);

            return;
        }
        if (srcType.isUnsigned() && dstType.isFloating()) {
            TODO(); // FIXME this one looks kinda hard idk how to do it right (eg: godbolt)
        }
    }*/

    // memory primitives
    void readMem(RegisterHandle tgt, RegisterHandle obj, size_t offset, size_t amount) override;
    void writeMem(RegisterHandle obj, RegisterHandle value, size_t offset, size_t amount) override;
    void addressOf(Assembler::RegisterHandle tgt, Assembler::RegisterHandle obj) override;

    // register primitives
    void movInt(Assembler::RegisterHandle dest, u64 value, size_t offsetBytes = 0) override;
    void movReg(Assembler::RegisterHandle dest, Assembler::RegisterHandle src, size_t destOffsetBytes = 0, size_t srcOffsetBytes = 0, size_t amount = 8) override;

    // flow control functions
    void nop() override;
    void jmp(string_view label) override;
    void createLabel(string_view name) override;
    void jmpLabelTrue(RegisterHandle cond, string_view label) override;
    void jmpLabelFalse(RegisterHandle cond, string_view label) override;
    void generateRet() override;
    void generateRet(RegisterHandle value) override;

    // register allocation
    RegisterHandle allocateRegister(size_t size) override;
    void freeRegister(Assembler::RegisterHandle rt) override;

    // complex functions, which boil down to builtin function invocation
    // void objectGetter(Assembler::RegisterHandle target, Assembler::RegisterHandle object, size_t fieldId) override;

    void garbageMemCpy(X64Register ptrReg, size_t stackOffset, size_t stackSize);

    void garbageMemCpy(X64Register dst, size_t dstOffset, X64Register src, size_t srcOffset, size_t stackSize);

    // void invokeBuiltin(LinkSymbol symbol, span<const Assembler::RegisterHandle> args, optional<Assembler::RegisterHandle> ret) override;

    RegisterHandle allocateStack(size_t size) override {
        return allocator.allocateStack(size);
    }

    void signExtend(Assembler::RegisterHandle dst, Assembler::RegisterHandle src) override;

    void f32ToF64(Assembler::RegisterHandle dest, Assembler::RegisterHandle value) override;

    void f64ToF32(Assembler::RegisterHandle dest, Assembler::RegisterHandle value) override;

    // constexpr static string RETURN_LABEL = "return";
    size_t returnCounter = 0;

    string nextReturnLabel();

    void movLabel(const X64Register& dest, size_t value);

    void movLabel(int offset, size_t name);

    // beginning of a block (IF0, ELSE1, SWITCH3, FOR7)
    void makeLabel(string_view name, Label::Type type, int size = 8, size_t data = 0);

    void makeAbsolute(string_view name, int size) {
        makeLabel(name, Label::Type::ABSOLUTE1, size);
    }

    void putRelative(string_view name, ImmSpace space);

    void putAbsolute(string_view name, ImmSpace space);

    void putLabel(string_view name, ImmSpace space, Label::Type type, size_t data = 0);

    void putGOT(size_t index, ImmSpace space) {
        spaces["GOT-GOT"].push_back(Label{static_cast<long>(space.offset), static_cast<int>(space.size), Label::Type::GOT, index});
    }

    void withSavedCallRegs(span<const X64Register> exclude, const std::function<X64Register::SaveType(const X64Register&)>& convention, const std::function<void(const map<X64Register, size_t>&)>& callback);

    void callC(size_t label, span<const RegisterHandle> args, optional<RegisterHandle> ret);

    void callC(Arg label, span<const RegisterHandle> args, optional<RegisterHandle> ret) {
        vector<Arg> argz;

        for (auto [i, arg] : args | views::enumerate) {
            argz.push_back(handleToArg(arg));
        }
        optional<Arg> idk;
        if (ret.has_value()) {
            idk = handleToArg(*ret);
        }

        chadCall(label, argz, idk);
    }

    vector<X64Register> moveRegsToArgs(span<const RegisterHandle> handles, size_t argsOffset, const map<X64Register, size_t>& preserved, bool isFirstPtr = false);

    void moveToArgRegs(span<const X64Register> args);

    void writeJmp(CmpType jmp, string_view label);

    void writeJmp(string_view label);

    void withSpecificReg(const X64Register& reg, const function<void()>& callback);

    size_t preserveCalleeRegs(const std::function<X64Register::SaveType(X64Register)>& save);

    size_t preserveCalleeRegs() {
#ifdef LINUX
        return  preserveCalleeRegs(sysVSave);
#else
        return  preserveCalleeRegs(fastCallSave);
#endif
    }

    void patchStackSize(size_t stackSize);

    void instructionNumberHint(size_t id) override;

    void movToArgRegVIPLCallingConvention(const X64Register& dst, Assembler::RegisterHandle src, const map<X64Register, size_t>& preserved, bool moveAsIs = false);

    size_t writeStack(size_t offset, Assembler::RegisterHandle handle);

    size_t dumpToStack(X64Register reg);

    template<typename Ret, typename Class, typename... Args>
    constexpr void finallCallbackWrapper(Class obj, Ret (Class::* lambda)(Args...) const, Args... args);

    template<typename Class, typename... Args>
    constexpr void callWrapper(Class obj, void (Class::* lambda)(Args...) const, span<const X64Register> ignore1);

    template<typename T>
    void withTempReg(T callback, span<const X64Register> a);

    // function that will either get reg or
    // allocate temp reg
    // if no reg is available it will swap it to stack
    // and move original value to it
    X64Register idk(size_t handle, set<X64Register>& clobered, vector<pair<X64Register, optional<size_t>>>& toRestore);

    X64Register idk2(set<X64Register>& clobered, vector<pair<X64Register, optional<size_t>>>& toRestore);

    void restoreRegState(vector<pair<X64Register, optional<size_t>>>& regs);

    template<typename... Args, typename F>
    void withRegs(F fan, Args... args);

    void movRegToHandle(size_t dst, X64Register src);

    void movHandleToReg(const X64Register& dst, Assembler::RegisterHandle src);

    void threeWayWrapper(size_t tgt, size_t lhs, size_t rhs, auto fun);

    void twoWayWrapper(size_t tgt, size_t lhs, size_t rhs, auto fun);

    void movSymbol(RegisterHandle dst, size_t value) override;

    void movRegToReg(const X64Register& dst, const X64Register& src, size_t size);

    void movRegToStack(size_t handle, int _offset, const X64Register& src);

    size_t addressOf(size_t stackHandle);

    void movStackToReg(const X64Register& dst, size_t adrHandle, int srcOffset = 0);

    void freeHandles(span<const size_t> handles);

    void freeHandles(initializer_list<size_t> handles);

    size_t getLabelId(size_t name);

    size_t toHandleStupid(const X64Register& reg) {
        return allocator.toHandleStupid(reg);
    }

    Arg handleToArg(size_t handle);

    Arg handleToArgAssume8(size_t handle);

    size_t calculateStackSizeFastCall(span<const RegisterHandle> args);

    /*    void fastCall(LinkSymbol label, span<const RegisterHandle> args, optional<RegisterHandle> ret) {
        const array<X64Register, 4> FAST_CALL_REG_ARGS{X64Register::Rcx, X64Register::Rdx, X64Register::R8, X64Register::R9};
        const size_t STACK_BYTES_USED = calculateStackSizeFastCall(args);
        const size_t STACK_BYTES_RESERVED = REG_SIZE * FAST_CALL_REG_ARGS.size(); // 40B

        vector<X64Register> ignoredRegs;
        if (ret.has_value() && !RegAlloc::isStack(*ret)) {
            ignoredRegs.push_back(allocator.getReg(*ret));
        }

        size_t currentStackOffset = 0;

        withSavedCallRegs(ignoredRegs, fastCallSave, [&](const auto& savedRegs) {
            // prepare arg stack
            auto stackAllocated = align(STACK_BYTES_USED + STACK_BYTES_RESERVED, 16);
            mc.subImm32(X64Register::Rsp, stackAllocated);

            // FIXME the arguments are pused from right to left IDK if we are doing that fix if wrong :*
            for (auto [index, arg] : args | views::enumerate) {
                // try to pass first 4 args trough regs
                if (allocator.sizeOf(arg) <= 8 && index < 4) {
                    auto reeg = allocator.getReg(arg);
                    if (not allocator.isStack(arg) && savedRegs.contains(reeg)) {
                        mc.readStack(savedRegs.at(reeg)+stackAllocated, FAST_CALL_REG_ARGS[index]);
                    }
                    else {
                        movHandleToReg(FAST_CALL_REG_ARGS[index], arg);
                    }
                }
                // move onto stack
                // lower args go to lower addresses
                else {
                    auto bytesUsed = writeStack(STACK_BYTES_USED + currentStackOffset, arg);
                    // we assume alignment if it breaks (it will) fix it ._.
                    assert(bytesUsed % 8 == 0); // aligned to 8B
                    currentStackOffset += bytesUsed;
                }
            }

            // FIXME not sure about the return stuff

            // move address to rax
            movLabel(X64Register::Rax, label);

            // call ret reg
            mc.call(X64Register::Rax);

            // move rax to ret
            if (ret.has_value()) {
                movRegToHandle(*ret, X64Register::Rax);
            }

            // restore arg stack
            mc.addImm32(X64Register::Rsp, stackAllocated);
        });
    }*/

    // PRE: 16bit aligned stack, SAVED CALLER SAVED REGS
    // REQUIRED INFO/SERVICESS: clobbered regs
    // 8 < value <= 16 will be passed over 2 registers
    void invokeScuffedSYSV(Arg func, span<Arg> args, optional<Arg> ret);

    void generateArgMove(X64Register ret, Arg arg) {
        ImmSpace space;
        switch (arg.type) {
            case Arg::SYMBOL:
                movLabel(ret, arg.symbol);
                break;
            case Arg::SYMBOL_RIP_OFF_32:
                space = mc.leaRip(ret, 0);
                putLabel(stringify("__{}", arg.symbol), space, Label::Type::RIP_REL_32_ADR, arg.offset);
                break;
            case Arg::SYMBOL_RIP_VALUE_32:
                space = mc.relativeRead(ret, arg.offset);
                putLabel(stringify("__{}", arg.symbol), space, Label::Type::RIP_REL_32_VAL, arg.offset);
                break;
            default:
                PANIC()
        }
    }

    void invokeScuffedFastCall(Arg func, span<Arg> args, optional<Arg> ret);

    void chadCall(Arg fun, span<Arg> args, optional<Arg> ret);

    void jmpCond(string_view label, JumpCondType type, RegisterHandle lhs, RegisterHandle rhs) override;

    template<typename T>
    void patchLabel(Label l, T value) {
        std::memcpy(bytes.data()+l.index, &value, sizeof value);
    }
};