#pragma once
#include <iostream>
#include <set>
#include <unordered_set>

#include "CodeBlock.h"
#include "ControlFlowGraph.h"
#include "SSARegisterHandle.h"
#include "../utils/utils.h"
#include "../utils/Errorable.h"

using namespace std;

template<typename CTX>
class IRGen {
    typedef CodeBlock<CTX> BaseBlock;
public:
    ControlFlowGraph<CTX>& graph;
    size_t loopIdCounter = 0;
    friend CodeGen<CTX>;
public:
    IRGen(ControlFlowGraph<CTX>& graph): graph(graph) {}
    IRGen(const IRGen&) = delete;

    size_t nextConstructId() {
        return loopIdCounter++;
    }

    BaseBlock& getBlock(size_t blockId) { return graph.getBlock(blockId); }

    [[nodiscard]] const BaseBlock& getBlockConst(size_t blockId) const { return graph.getBlockConst(blockId); }

    // void stackPush(SSARegisterHandle target) { registerStack.push(target); }

    CTX::REG& getRecord(SSARegisterHandle target) { return graph.getRecord(target); }

    [[nodiscard]] const CTX::REG& getRecordConst(SSARegisterHandle target) const { return graph.getRecordConst(target); }
    // Result<SSARegisterHandle> stackPop();

    BaseBlock& createBlock(string_view tag) { return graph.createBlock(stringify("{}::{}", tag, graph.nodeCount())); }

    BaseBlock& createBlock(string_view tag, BaseBlock& previous) { auto& v = graph.createBlock(stringify("{}::{}", tag, graph.nodeCount())); v.setPrevious(previous.blockId); return v;}

    CTX::REG& getRoot(SSARegisterHandle target) { return graph.getRoot(target); }

    [[nodiscard]] const CTX::REG& getRootConst(SSARegisterHandle target) const { return graph.getRootConst(target); }

    auto& root() { return graph.getBlock(0); }

    auto nodes() { return graph.validNodes(); }

    void print() {
        graph.printRegisters();
        for (auto& block : nodes()) {
            const auto & instructions = block->getInstructions();
            println("{} - {}", block->tag, block->previousId().has_value() ? to_string(*block->previousId()) : "null");
            if (instructions.empty()) {
                println("  _ := TERMINAL");
            }
            for (auto& instruction : instructions) {
                cout << "  ";
                instruction->print(*static_cast<CTX::IRGEN*>(this));
            }
        }
    }

    SSARegisterHandle generateNewVersion(SSARegisterHandle previousHandle, BaseBlock& block) {
        return graph.generateNewVersion(previousHandle, block);
    }

    Result<SSARegisterHandle> lookupLocal(string_view name, BaseBlock& _block) {
        auto res = lookupOptionalLocal(name, _block);
        if (not res.has_value())
            return FAILF("could not find variable: {}", name);
        return *res;
        // return EXCEPTION(lookupOptionalLocal(name, _block), stringify("could not find variable: {}", name));
    }

    optional<SSARegisterHandle> lookupOptionalLocal(string_view name, BaseBlock& _block) {
        return graph.lookupOptionalLocal(name, _block);
    }

    void markUse(SSARegisterHandle handle) {
        getRecord(handle).incUseCount();
    }

    vector<size_t> lookupIngress(size_t id) {
        return this->graph.lookupIngress(id);
    }
};