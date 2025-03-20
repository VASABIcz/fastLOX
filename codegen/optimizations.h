#pragma once

#include "forward.h"
#include "IRInstructions.h"

/*bool optimizeSimpleReturns(IRGen& gen) {
    bool didOptimize = false;

    for (const auto& block: gen.nodes()) {
        if (block->getInstructions().size() != 1 || gen.root().blockId == block->blockId) continue;

        if (block->getInstruction(0)->is<instructions::Return>()) {
            auto ingress = gen.lookupIngress(block->blockId);

            for (auto srcId: ingress) {
                gen.getBlock(srcId).getInstruction(-1) = make_unique<instructions::Return>(SSARegisterHandle::invalid(), SSARegisterHandle::invalid());
            }
        }

        gen.graph.destroy(block->blockId);
        didOptimize = true;
    }

    return didOptimize;
}*/

template<typename CTX>
bool optimizePhis(typename CTX::IRGEN& gen) {
    bool didOptimize = false;

    for (auto& block: gen.nodes()) {
        for (const auto& instruction: block->getInstructions() | views::reverse) {
            if (!instruction->template is<instructions::PhiFunction<CTX>>()) continue;

            auto* phi = instruction->template cst<instructions::PhiFunction<CTX>>();
            if (phi->getAllVersions().size() > 1) continue;
            auto target = phi->target;
            auto valu = *phi->getVersions().begin();

            for (const auto& otherBlock: gen.nodes()) {
                for (const auto& instruction1: otherBlock->getInstructions()) {
                    if (instruction1->template is<instructions::PhiFunction<CTX>>()) {
                        auto phaj = instruction1->template cst<instructions::PhiFunction<CTX>>();
                        if (phaj->target == valu) {
                            phaj->remove(target);
                            continue;
                        }
                    }
                    instruction1->visitSrc([&](auto& reg) {
                        if (reg == target) {
                            reg = valu;
                        }
                    });
                }
            }

            block->removeInstruction(instruction);
            didOptimize = true;
        }
    }

    return didOptimize;
}

template<typename CTX>
bool optimizeAssign(typename CTX::IRGEN& gen) {
    bool didOptimize = false;

    for (auto& block: gen.nodes()) {
        for (const auto& instruction: block->getInstructions() | views::reverse) {
            if (!instruction->template is<instructions::Assign<CTX>>()) continue;

            auto* assign = instruction->template cst<instructions::Assign<CTX>>();
            auto& value = gen.getRecord(assign->value);
            // auto& target = gen.getRecord(assign->target);
            value.decUseCount();

            for (auto& otherBlock: gen.nodes()) {
                for (const auto& instruction1: otherBlock->getInstructions()) {
                    instruction1->visitSrc([&](auto& reg) {
                        if (reg == assign->target) {
                            reg = assign->value;
                            gen.markUse(assign->value);
                        }
                    });
                }
            }

            block->removeInstruction(instruction);
            didOptimize = true;
        }
    }

    return didOptimize;
}

template<typename CTX>
void forEachInstruction(ControlFlowGraph<CTX>& g, auto&& fn) {
    for (auto& node : g.validNodes()) {
        for (auto& inst : node->instructions) {
            if (!fn(inst)) return;
        }
    }
}

template<typename CTX>
CopyPtr<IRInstruction<CTX>>* findInstructionByTGT(ControlFlowGraph<CTX>& g, SSARegisterHandle reg) {
    CopyPtr<IRInstruction<CTX>>* res = nullptr;
    forEachInstruction(g, [&](auto&& it) -> bool {
        if (it->target == reg) {
           res = &it;
           return false;
        }
        return true;
    });

    return res;
}

template<typename IRGEN>
bool removeUnusedInstructions(IRGEN& gen) {
    bool didOptimize = false;

    for (auto& block: gen.nodes()) {
        for (auto& instruction: block->getInstructionsMut() | views::reverse) {
            if (!instruction->target.isValid()) continue;

            if (gen.graph.getRecord(instruction->target).useCount == 0 && instruction->isPure()) {
                instruction->visitSrc([&](auto& reg) {
                    gen.getRecord(reg).decUseCount();
                });

                block->removeInstruction(instruction);
                didOptimize = true;
            }
        }
    }

    return didOptimize;
}

template<typename CTX>
bool mergeLiveRanges(typename CTX::IRGEN& gen, map<SSARegisterHandle, vector<bool>>& liveRanges) {
    bool didOptimize = false;

    for (auto& block: gen.nodes()) {
        for (auto& instruction: block->getInstructionsMut() | views::reverse) {
            if (!instruction->template is<instructions::PhiFunction<CTX>>()) continue;

            auto* phi = instruction->template cst<instructions::PhiFunction<CTX>>();

            vector<bool> merged(liveRanges.begin()->second.size());

            const auto apply = [&](SSARegisterHandle idk) {
                size_t overlaps = 0;
                for (auto [i, b] : liveRanges[idk] | views::enumerate) {
                    if (merged[i] && b) overlaps += 1;
                    if (b) merged[i] = true;
                }
                return overlaps <= 1;
            };

            auto toApply = phi->getVersions();
            toApply.insert(phi->target);
            auto sources = phi->getVersions();
            sources.erase(phi->target);

            bool wasSucess = true;
            for (auto& idk : toApply) {
                if (!apply(idk)) {
                    wasSucess = false;
                    break;
                }
            }

            if (wasSucess) {
                didOptimize = true;

                // patch nodes
                forEachInstruction(gen.graph, [&](auto& inst) -> bool {
                    // patch writes
                    if (sources.contains(inst->target)) {
                        inst->target = phi->target;
                    }
                    // patch reads
                    inst->visitSrc([&](auto& src) {
                        if (sources.contains(src)) {
                            src = phi->target;
                        }
                    });

                    return true;
                });

                // set new live range
                liveRanges[phi->target] = merged;

                // remove source registers
                for (auto& source : sources) {
                    liveRanges.erase(source);
                }

                // replace phi instruction with noop, this makes sure we dont break live ranges
                instruction.uPtrRef() = std::make_unique<instructions::NoOp<CTX>>();
            }
        }
    }

    return didOptimize;
}

template<typename CTX>
bool removeFallJumps(typename CTX::IRGEN& gen, span<size_t> linearized) {
    bool didOptimize = false;

    for (auto i: views::iota(0u, linearized.size()-1)) {
        auto& inst = gen.getBlock(linearized[i]).getInstruction(-1);
        if (inst->template is<instructions::Jump<CTX>>()) {
            auto next = inst->template cst<instructions::Jump<CTX>>()->value;
            if (next == linearized[i+1]) {
                inst.uPtrRef() = make_unique<instructions::FallTrough<CTX>>(next);
                didOptimize = true;
            }
        }
        else if (inst->template is<instructions::Branch<CTX>>()) {
            auto* branch = inst->template cst<instructions::Branch<CTX>>();
            if (linearized[i+1] == branch->scopeT) {
                inst.uPtrRef() = make_unique<instructions::JumpFalse<CTX>>(branch->condition, branch->scopeT, branch->scopeF);
                didOptimize = true;
            }
            else if (linearized[i+1] == branch->scopeF) {
                inst.uPtrRef() = make_unique<instructions::JumpTrue<CTX>>(branch->condition, branch->scopeT, branch->scopeF);
                didOptimize = true;
            }
        }
    }

    return didOptimize;
}

template<typename CTX>
bool forceStackAlloc(typename CTX::IRGEN& gen) {
    bool didOptimize = false;

    for (auto& block: gen.nodes()) {
        for (auto& instruction: block->getInstructionsMut() | views::reverse) {
            if (not instruction->template is<instructions::AddressOf<CTX>>()) continue;
            auto* inst = instruction->template cst<instructions::AddressOf<CTX>>();

            gen.getRecord(inst->obj).forceStack();
            didOptimize = true;
        }
    }

    return didOptimize;
}


template<typename CTX>
bool optimizeJmpCond(typename CTX::IRGEN& gen) {
    bool didOptimize = false;

    enum class Type {
        BRANCH,
        JUMP_TRUE,
        JUMP_FALSE
    };

    for (auto& block: gen.nodes()) {
        for (auto& instruction: block->getInstructionsMut()) {
            if (not instruction->template is<instructions::Branch<CTX>>() && not instruction->template is<instructions::JumpFalse<CTX>>() && not instruction->template is<instructions::JumpTrue<CTX>>()) continue;

            Type type;
            size_t scopeT;
            size_t scopeF;
            SSARegisterHandle subject;

            instruction->template ifIs<instructions::Branch<CTX>>([&](auto& it) {
                type = Type::BRANCH;
                scopeT = it.scopeT;
                scopeF = it.scopeF;
                subject = it.condition;
            }) || instruction->template ifIs<instructions::JumpTrue<CTX>>([&](auto& it) {
                type = Type::JUMP_TRUE;
                scopeT = it.scopeT;
                scopeF = it.scopeF;
                subject = it.condition;
            }) || instruction->template ifIs<instructions::JumpFalse<CTX>>([&](auto& it) {
                type = Type::JUMP_FALSE;
                scopeT = it.scopeT;
                scopeF = it.scopeF;
                subject = it.condition;
            });

            auto cnd1 = findInstructionByTGT(gen.graph, subject);

            if (cnd1 == nullptr) continue;
            auto& cnd = *cnd1;

            optional<JumpCondType> tajp;
            optional<SSARegisterHandle> rhs;
            optional<SSARegisterHandle> lhs;

            cnd->template ifIs<instructions::IntEquals<CTX>>([&](auto& it) {
                tajp = JumpCondType::EQUALS;
                rhs = it.rhs;
                lhs = it.lhs;
            });
            cnd->template ifIs<instructions::IntGe<CTX>>([&](auto& it) {
                tajp = JumpCondType::GREATER_OR_EQUAL;
                rhs = it.rhs;
                lhs = it.lhs;
            });
            cnd->template ifIs<instructions::IntGt<CTX>>([&](auto& it) {
                tajp = JumpCondType::GREATER;
                rhs = it.rhs;
                lhs = it.lhs;
            });
            cnd->template ifIs<instructions::IntLess<CTX>>([&](auto& it) {
                tajp = JumpCondType::LESS;
                rhs = it.rhs;
                lhs = it.lhs;
            });
            cnd->template ifIs<instructions::IntLe<CTX>>([&](auto& it) {
                tajp = JumpCondType::LESS_OR_EQUAL;
                rhs = it.rhs;
                lhs = it.lhs;
            });

            if (not tajp.has_value()) continue;

            switch (type) {
                case Type::BRANCH:
                    instruction = CopyPtr<IRInstruction<CTX>>(make_unique<instructions::BranchCond<CTX>>(*tajp, *lhs, *rhs, scopeT, scopeF));
                    break;
                case Type::JUMP_TRUE:
                    instruction = CopyPtr<IRInstruction<CTX>>(make_unique<instructions::JumpCond<CTX>>(*tajp, *lhs, *rhs, scopeT, scopeF));
                    break;
                case Type::JUMP_FALSE:
                    break;
                    instruction = CopyPtr<IRInstruction<CTX>>(make_unique<instructions::JumpCond<CTX>>(negateType(*tajp), *lhs, *rhs, scopeT, scopeF));
            }

            didOptimize = true;
        }
    }
    return didOptimize;
}