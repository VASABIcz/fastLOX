#pragma once

class SSARegisterHandle;

class Assembler;

template<typename CTX>
class CodeGen;

template<typename CTX>
class IRGen;

template<typename CTX>
class CodeBlock;

namespace instructions {
    template<typename CTX>
    struct PhiFunction;
    template<typename CTX>
    class NoOp;
    template<typename CTX>
    class Branch;
    template<typename CTX>
    class JumpTrue;
    template<typename CTX>
    class JumpFalse;
    template<typename CTX>
    class Jump;
    template<typename CTX>
    class FallTrough;
    template<typename CTX>
    class Assign;
    template<typename CTX>
    class AddressOf;
    template<typename CTX>
    struct IntEquals;
    template<typename CTX>
    struct IntGt;
    template<typename CTX>
    struct IntGe;
    template<typename CTX>
    struct IntSub;
    template<typename CTX>
    struct IntLe;
    template<typename CTX>
    struct IntLess;
    template<typename CTX>
    class BranchCond;
    template<typename CTX>
    class JumpCond;
}