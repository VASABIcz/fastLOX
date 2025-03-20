#include "definitions.h"
#include "Register.h"

array<X64Register, 6> X64Register::mArgRegs = {Rdi, Rsi, Rdx, Rcx, R8, R9};
array<X64Register, 16> X64Register::ALL_REGS = {Rax, Rbx, Rcx, Rdx, Rsi, Rdi, Rbp, Rsp, R8, R9, R10, R11, R12, R13, R14, R15};

#define REEG(nam) const X64Register X64Register::nam = X64RegisterType::nam;
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