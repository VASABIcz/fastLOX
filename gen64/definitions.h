#pragma once
#include <vector>
#include <span>
#include <cassert>
#include <iostream>
#include <utility>

#include "Register.h"
#include "../utils/utils.h"

using namespace std;

// RM encoding
enum class X64Instruction: u8 {
    mov = 0x8B, // 0x89
    add = 0x03, // 0x01
    sub = 0x2B, // 0x29
    xorI = 0x33, // 0x31
    cmp = 0x3B, // 0x39
    setCC = 0x0F, //
    idiv = 0xF7,
    baseJmp = 0x70,
    jmp = 0xE9,
    Or = 0x0B,
    And = 0x23,
    Test = 0x85,
    Call = 0xFF
};

// FIXME swapped endianness
// RM encoding
enum class SusX64Instruction {
    imul = 0xAF0F,
    movzx16 = 0xA70F,
    movzx8 = 0xB60F
};

enum class CmpType: u8 {
    Overflow = 0x0,
    NotOverflow = 0x1,
    Bellow = 0x2,
    NotBellow = 0x3,
    AboveEqual = NotBellow,
    Equal = 0x4,
    NotEqual = 0x5,
    BellowOrEqual = 0x6,
    Above = 0x7,
    Sing = 0x8,
    NotSing = 0x9,
    Parity = 0xA, // even
    NotParity = 0xB, // odd
    Less = 0xC,
    GreaterOrEqual = 0xD,
    LessOrEqual = 0xE,
    Greater = 0xF,
    JumpIfEcx0 = 0x64 // not valid for cmov
};

enum class SimpleX64Instruction {
    ret = 0xC3,
    nop = 0x90,
    cqo = 0x99,
    leave = 0xC9
};

enum class ArithmeticOp {
    Add,
    Sub = 5
};

enum class X64RegisterType: u8 {
    Rax,
    Zero = Rax,
    Rcx,
    One = Rcx,
    Rdx,
    Two = Rdx,
    Rbx,
    Three = Rbx,
    Rsp,
    Four = Rsp,
    Rbp,
    Five = Rbp,
    Rsi,
    Six = Rsi,
    Rdi,
    Seven = Rdi,
    R8,
    R9,
    R10,
    R11,
    R12,
    R13,
    R14,
    R15,
};

class X64Register {
public:
    enum class SaveType {
        Caller,
        Callee,
        None
    };

    constexpr X64Register(X64RegisterType v): value(v) {}

    // constexpr operator Value() const { return value; }

    explicit operator bool() const = delete;

    // TODO three way comp
    constexpr bool operator==(const X64Register& other) const {
        return value == other.value;
    }

    constexpr bool operator<(const X64Register& other) const {
        return value < other.value;
    }

    constexpr bool operator>(const X64Register& other) const {
        return value > other.value;
    }

    [[nodiscard]] string toString() const {
        using enum X64RegisterType;
        switch (value) {
            case Rax: return "rax";
            case Rbx: return "rbx";
            case Rcx: return "rcx";
            case Rdx: return "rdx";
            case Rsi: return "rsi";
            case Rdi: return "rdi";
            case Rsp: return "rsp";
            case Rbp: return "rbp";
            case R8: return  "r8";
            case R9: return  "r9";
            case R10: return "r10";
            case R11: return "r11";
            case R12: return "r12";
            case R13: return "r13";
            case R14: return "r14";
            case R15: return "r15";
        }
        unreachable();
    }

    [[nodiscard]] u8 getEncoding() const {
        constexpr u8 offset = 0x8;

        if (isExt()) {
            return static_cast<u8>(this->value) - offset;
        }

        return static_cast<u8>(this->value);
    }

    X64RegisterType getValue() const {
        return value;
    }

    [[nodiscard]] constexpr bool isExt() const {
        using enum X64RegisterType;
        return value >= R8;
    }

    static constexpr span<X64Register> argRegs() {
        return mArgRegs;
    }

#define REEG(nam) const static X64Register nam;
    REEG(Zero)
    REEG(One)
    REEG(Two)
    REEG(Three)
    REEG(Four)
    REEG(Five)
    REEG(Six)
    REEG(Seven)
    REEG(Rax)
    REEG(Rbx)
    REEG(Rcx)
    REEG(Rdx)
    REEG(Rsi)
    REEG(Rdi)
    REEG(Rbp)
    REEG(Rsp)
    REEG(R8)
    REEG(R9)
    REEG(R10)
    REEG(R11)
    REEG(R12)
    REEG(R13)
    REEG(R14)
    REEG(R15)
#undef REEG
    static array<X64Register, 16> ALL_REGS;
private:
    X64RegisterType value;
    static array<X64Register, 6> mArgRegs;// = {{Rdi}, {Rsi}, Rdx, Rcx, R8, R9};
};

enum SibScale: u8 {
    One = 0,
    Two = 1,
    Four = 2,
    Eight = 3
};

inline X64Register::SaveType fastCallSave(const X64Register& reg) {
    using enum X64RegisterType;
    switch (reg.getValue()) {
        case Rax:
        case Rcx:
        case Rdx:
        case R8:
        case R9:
        case R10:
        case R11:
            return X64Register::SaveType::Caller;
        case Rsi:
        case Rdi:
        case Rbx:
        case R12:
        case R13:
        case R14:
        case R15:
            return X64Register::SaveType::Callee;
        case Rbp:
        case Rsp:
            return X64Register::SaveType::None;
    }
    unreachable();
}

inline X64Register::SaveType sysVSave(const X64Register& reg) {
    using enum X64RegisterType;
    switch (reg.getValue()) {
        case Rax:
        case Rdx:
        case Rcx:
        case Rsi:
        case Rdi:
        case R8:
        case R9:
        case R10:
        case R11:
            return X64Register::SaveType::Caller;
        case Rbx:
        case R12:
        case R13:
        case R14:
        case R15:
            return X64Register::SaveType::Callee;
        case Rbp:
        case Rsp:
            return X64Register::SaveType::None;
    }
    unreachable();
}