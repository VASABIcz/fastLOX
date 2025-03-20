#pragma once
#include <functional>

#include "forward.h"
#include "SSARegisterHandle.h"
#include "../utils/StringLiteral.h"
#include "../utils/VirtualCopy.h"
#include "../utils/stringify.h"

template<typename CTX>
struct IRInstruction: public VirtualCopy<IRInstruction<CTX>> {
    SSARegisterHandle target;
    string name;

    typedef CodeBlock<CTX> BaseGen;

    virtual ~IRInstruction() = default;

    IRInstruction(SSARegisterHandle target, string name): target(target), name(std::move(name)) {}

    virtual void print(CTX::IRGEN& gen) = 0;

    virtual void visitSrc(function<void(SSARegisterHandle&)> fn) {}

    virtual void generate(CTX::GEN& gen) = 0;

    [[nodiscard]] virtual vector<size_t> branchTargets() const {
        return {};
    }

    [[nodiscard]] virtual bool isTerminal() const {
        return false;
    }

    template<class T>
    [[nodiscard]] bool is() const {
        bool b = dynamic_cast<const T *>(this) != nullptr;
        return b;
    }

    template<class T>
    T* cst() {
        return dynamic_cast<T*>(this);
    }

    template<typename T>
    bool ifIs(std::function<void(T&)> fn) {
        if (is<T>()) {
            fn(*cst<T>());
            return true;
        }
        return false;
    }

    virtual bool isPure() const {
        return true;
    }

    template<typename... Args>
    constexpr void basePrint(StringChecker<type_identity_t<Args>...> strArg, Args&&... argz) {
        println("{} := {} {}", target, name, stringify(strArg, std::forward<Args>(argz)...));
    }
};

template<StringLiteral V, typename CTX>
struct NamedIrInstruction: public IRInstruction<CTX> {
    NamedIrInstruction(SSARegisterHandle tgt): IRInstruction<CTX>(tgt, string(V.asView())) {}
};

template<StringLiteral V, typename CTX>
struct IR2Instruction: public IRInstruction<CTX> {
    SSARegisterHandle lhs;
    SSARegisterHandle rhs;

    using BaseGen = CTX::GEN;

    IR2Instruction(SSARegisterHandle tgt, SSARegisterHandle lhs, SSARegisterHandle rhs):
    IRInstruction<CTX>(tgt, string(V.asView())), lhs(lhs), rhs(rhs) {}

    void visitSrc(std::function<void (SSARegisterHandle &)> fn) override {
        fn(rhs);
        fn(lhs);
    }

    void print(CTX::IRGEN &gen) override {
        println("{} := {} {}, {}", this->target.toTextString(), V.value, lhs.toString(), rhs.toString());
    }

    void generate(BaseGen& gen) override = 0;
};

template<StringLiteral V, typename CTX>
struct IR1Instruction: public IRInstruction<CTX> {
    SSARegisterHandle value;

    IR1Instruction(SSARegisterHandle tgt, SSARegisterHandle value): IRInstruction<CTX>(tgt, string(V.asView())), value(value) {

    }

    void visitSrc(std::function<void (SSARegisterHandle &)> fn) override {
        fn(value);
    }

    void print(CTX::IRGEN &gen) override {
        println("{} := {} {}", this->target.toTextString(), V.value, value.toString());
    }
};

template<StringLiteral V, typename CTX>
struct IR0Instruction: public IRInstruction<CTX> {
    IR0Instruction(): IRInstruction<CTX>(SSARegisterHandle::invalid(), string(V.asView())) {}
    explicit IR0Instruction(SSARegisterHandle h): IRInstruction<CTX>(h, string(V.asView())) {}

    void visitSrc(std::function<void (SSARegisterHandle &)> fn) override {}

    void print(CTX::IRGEN &gen) override {
        println("{} := {}", this->target.toTextString(), V.value);
    }
};

template<typename T, StringLiteral V, typename CTX>
struct IRBaseInstruction: public NamedIrInstruction<V, CTX> {
    T value;
    using BaseGen = CTX::GEN;

    IRBaseInstruction(SSARegisterHandle tgt, T value): NamedIrInstruction<V, CTX>(tgt), value(value) {

    }

    void visitSrc(std::function<void (SSARegisterHandle &)> fn) override {}

    void print(CTX::IRGEN& gen) override {
        this->basePrint("{}", stringify(value));
    }

    void generate(BaseGen& gen) override = 0;
};