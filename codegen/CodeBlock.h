#pragma once

#include <memory>
#include <utility>
#include <vector>
#include <map>
#include <stack>
#include <ranges>

#include "IRInstruction.h"
#include "../utils/utils.h"
#include "../utils/CopyPtr.h"

using namespace std;

template<typename CTX>
class CodeBlock: VirtualCopy<CodeBlock<CTX>> {
public:
    CodeBlock* deepCopy() const override {
        auto n = new CodeBlock<CTX>(this->tag, this->blockId);
        n->instructions = this->instructions;
        n->previous = this->previous;
        n->registers = this->registers;

        return n;
    }

    CodeBlock(const CodeBlock&) = delete;

    CodeBlock(CodeBlock&&) = default;

    [[nodiscard]] const vector<CopyPtr<IRInstruction<CTX>>>& getInstructions() const { return instructions; }

    [[nodiscard]] vector<CopyPtr<IRInstruction<CTX>>>& getInstructionsMut() { return instructions; }

    [[nodiscard]] optional<size_t> previousId() const { return previous; }

    optional<SSARegisterHandle> getRegisterHandleByName(string_view name) {
        auto reg = getRegisterByName(name);
        if (reg.has_value()) return (*reg)->getHandle();

        return {};
    }

    bool isTerminated() {
        return !isEmpty() && getInstruction(-1)->isTerminal();
    }

    template<typename T, typename ...Args>
    T* pushInstruction(Args&&... args) {
        assert(this != nullptr);
        assert(isEmpty() || !getInstruction(-1)->isTerminal());
        auto instruction = createInstruction<T>(std::forward<Args>(args)...);
        auto ptr = instruction.get();
        push(std::move(instruction));

        return ptr;
    }

    size_t id() const {
        return blockId;
    }

    span<CopyPtr<typename CTX::REG>> getRegisters() {
        return registers;
    }

    static bool canPutInstruction(CodeBlock* block) {
        if (block == nullptr) return false;
        if (block->isEmpty()) return true;

        if (block->getInstruction(-1)->isTerminal()) return false;

        return true;
    }

    string tag;
    size_t blockId;
    vector<CopyPtr<IRInstruction<CTX>>> instructions{};

    CodeBlock clone() const {
        return *this;
    }

    CopyPtr<IRInstruction<CTX>>& getInstruction(int index) {
        if (index >= (int)instructions.size()) assert(false);
        if (index < 0 && (int)instructions.size()+index < 0) assert(false);

        return instructions[index < 0 ? instructions.size()+index : index];
    }

    const CopyPtr<IRInstruction<CTX>>& getInstructionConst(int index) const {
        if (index >= static_cast<int>(instructions.size())) assert(false);
        if (index < 0 && static_cast<int>(instructions.size())+index < 0) assert(false);

        return instructions[index < 0 ? instructions.size()+index : index];
    }

/// returns reg map in format:
/// ancestor to child
/// eg.: 0:3 to 0:0, 0:2 to 0:0

    map<SSARegisterHandle, instructions::PhiFunction<CTX>*> getPhiFunctions() const {
        map<SSARegisterHandle, instructions::PhiFunction<CTX>*> pis;

      vector<instructions::PhiFunction<CTX>*> phiSpan = getPhiSpan();
        for (size_t i = 0; i < phiSpan.size(); i++) {
          instructions::PhiFunction<CTX>* phi = phiSpan[i];
            for (SSARegisterHandle version : phi->getVersions()) {
                pis[version] = phi;
            }
        }

        return pis;
    }

    vector<size_t> getTargets() const {
        if (instructions.empty()) return {};

        return getInstructionConst(-1)->branchTargets();
    }

    vector<instructions::PhiFunction<CTX>*> getPhiSpan() const {
        if (isEmpty())
            return {};

        vector<instructions::PhiFunction<CTX>*> phis;

        for (auto& inst : instructions) {
            if (inst->template is<instructions::PhiFunction<CTX>>()) {
                phis.push_back(inst->template cst<instructions::PhiFunction<CTX>>());
            } else if (inst->template is<instructions::NoOp<CTX>>()) {
                continue;
            }
            else {
                break;
            }
        }

        return phis;
    }

    map<SSARegisterHandle, SSARegisterHandle> getPhis(size_t source) const {
        map<SSARegisterHandle, SSARegisterHandle> pis;

        for (auto phi : getPhiSpan()) {
            auto value = phi->getBySource(source);
            if (value.has_value()) {
                pis[*value] = phi->target;
            }
        }

        return pis;
    }

    size_t phiCount() const {
        return getPhiSpan().size();
    }

    span<CopyPtr<IRInstruction<CTX>>> instructionsWithoutPhis() {
        return {instructions.data()+phiCount(), instructions.size()-phiCount()};
    }

    const CTX::REG& getRecordConst(size_t id) const {
        assertInBounds(registers, id);
        return *registers[id];
    }

    CTX::REG& getRecord(size_t id) {
        assertInBounds(registers, id);
        return *registers[id];
    }

    void removeInstruction(const CopyPtr<IRInstruction<CTX>>& inst) {
        erase_if(instructions, [&](auto& it) { return it.get() == inst.get(); });
    }

    const vector<CopyPtr<typename CTX::REG>>& getRegistersConst() const {
        return registers;
    }

    SSARegisterHandle getHandle(const CTX::REG& reg) const {
        return SSARegisterHandle::valid(blockId, reg.id);
    }

    bool isEmpty() const {
        return instructions.empty();
    }

    void setPrevious(size_t prev) {
        previous = prev;
    }

    CodeBlock(string tag, size_t blockId) : tag(std::move(tag)), blockId(blockId) {

    }

    SSARegisterHandle getTarget(size_t id) const { return SSARegisterHandle{blockId, id, true}; }

    size_t instructionCount() const {
        return instructions.size();
    }

    bool isLoopHeader() const {
        return mIsLoopHeader;
    }

    void setLoopHeader(bool isHeader) {
        mIsLoopHeader = isHeader;
    }

    optional<typename CTX::REG*> getRegisterByName(string_view name) {
        for (auto& reg : views::reverse(registers)) {
            if (reg->name == name) {
                return reg.get();
            }
        }

        return {};
    }

    vector<typename CTX::REG*> getArgRegisters() const {
        vector<typename CTX::REG*> buf;

        for (const auto& reg : registers) {
            if (!reg->isArgument()) continue;

            buf.push_back(reg.get());
        }

        return buf;
    }

/*    SSARegisterHandle pushRegister(string name, GenericType dataType, optional<SSARegisterHandle> previous, SSARegister::Type type) {
        auto record = make_unique<SSARegister>(blockId, std::move(name), dataType, type);

        const auto id = registers.size();
        record->id = id;
        if (previous.has_value()) record->setPrevious(*previous);

        assert(record->getBlockId() == blockId);

        registers.emplace_back(std::move(record));

        return SSARegisterHandle::valid(blockId, id);
    }*/

    optional<size_t> previous{}; // ment previous scope in the same level (used for var lookup)
    vector<CopyPtr<typename CTX::REG>> registers{};
    bool mIsLoopHeader = false;

    void push(unique_ptr<IRInstruction<CTX>> instruction) {
        assert(not instruction->target.isValid() || instruction->target.graphId == id());
        instructions.emplace_back(std::move(instruction));
    }

    SSARegisterHandle pushRegister(unique_ptr<typename CTX::REG> reg) {
        reg->id = registers.size();
        reg->setBlockId(id());

        auto hand = reg->getHandle();

        registers.push_back(std::move(reg));

        return hand;
    }

    template<typename T, typename ...Args>
    unique_ptr<T> createInstruction(Args... args) {
        return make_unique<T>(args...);
    }
};