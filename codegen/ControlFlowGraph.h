#pragma once
#include <ranges>
#include <unordered_set>

#include "CodeBlock.h"
#include "../utils/Errorable.h"

template<typename CTX>
struct ControlFlowGraph {
    typedef CodeBlock<CTX> BaseBlock;
    typedef unordered_map<size_t, unordered_set<size_t>> DependencyMap;

    ControlFlowGraph() = default;

    BaseBlock& getBlock(size_t blockId) {
        assertInBounds(nodes, blockId);
        return *nodes[blockId];
    }

    [[nodiscard]] const BaseBlock& getBlockConst(size_t blockId) const {
        assertInBounds(nodes, blockId);
        return *nodes[blockId];
    }

    auto validNodes() {
        return views::filter(this->nodes, [](const auto& value) { return value != nullptr; });
    }

    [[nodiscard]] auto validNodesConst() const {
        return views::filter(this->nodes, [](const auto& value) { return value != nullptr; });
    }

    [[nodiscard]] size_t nodeCount() const {
        auto _nodes = validNodesConst();
        return std::distance(_nodes.begin(), _nodes.end());
    }

    void destroy(size_t blockId) {
        println("destroying block: {}", blockId);
        (void)nodes[blockId].uPtrRef().reset(nullptr);
    }

    void dumpSVG(string_view name) const {
        string buf = "digraph Test {\n";
        for (const auto& node : validNodesConst()) {
            for (auto tgt : node->getTargets())
                buf += stringify("{} -> {}\n", node->blockId, tgt);
        }
        buf += "}";

        auto res = system(stringify("echo \"{}\" | dot -Tsvg > {}", buf, name).c_str());
        if (res) println("[gen] dumping exited with {}", res);
    }

    /// tries to look up the original non-shadowed variable
    optional<SSARegisterHandle> lookupRootLocal(BaseBlock& c, string_view name) {
        // current reg, root reg

        optional<SSARegisterHandle> best;
        map<SSARegisterHandle, SSARegisterHandle> modernMap;
        BaseBlock* cur = &c;

        while (true) {
            auto reg = cur->getRegisterHandleByName(name);

            if (reg.has_value()) {
                auto root = getRoot(*reg).getHandle();

                if (not modernMap.contains(root)) {
                    modernMap[root] = *reg;
                }

                if (!best.has_value() || best != root) {
                    best = root;
                }
            }


            if (not cur->previousId().has_value()) break;
            cur = &getBlock(*cur->previousId());
        }

        if (best.has_value()) return modernMap[*best];

        return {};
    }

    pair<SSARegisterHandle, instructions::PhiFunction<CTX>*> makePhi(BaseBlock& current, span<pair<SSARegisterHandle, size_t>> regs) {
        auto temp = generateNewVersion(regs.begin()->first, current);

        auto& tempRec = getRecord(temp);
        assert(tempRec.getHandle() == temp);

        for (std::weakly_incrementable auto _i : views::iota(0u, regs.size())) {
            (void)_i;
            tempRec.incUseCount();
        }

        for (const auto& reg: regs) {
            getRecord(reg.first).incUseCount();
        }

        auto ptr = current.template pushInstruction<instructions::PhiFunction>(temp, regs);

        return {temp, ptr};
    }

    SSARegisterHandle generateNewVersion(SSARegisterHandle previousHandle, BaseBlock& block) {
        assert(previousHandle.isValid());

        auto& previous = getRecord(previousHandle);
        auto cpy = previous.copy();
        cpy.setPrevious(previousHandle);

        return block.pushRegister(make_unique<typename CTX::REG>(cpy));
    }

    CTX::REG& getRoot(SSARegisterHandle target) {
        assert(target.isValid());
        auto* current = &getBlock(target.graphId).getRecord(target.registerId);

        while (!current->isRoot()) {
            auto t1 = current->getPrevious();
            assert(t1.has_value());
            current = &getRecord(*t1);
        }

        return *current;
    }

    CTX::REG& getRecord(SSARegisterHandle target) {
        assert(target.isValid());

        auto& block = getBlock(target.graphId);

        auto& reg = block.getRecord(target.registerId);

        assert(reg.getHandle() == target);

        return reg;
    }

    BaseBlock& createBlock(string tag) {
        nodes.emplace_back(make_unique<BaseBlock>(std::move(tag), nodes.size()));

        return *nodes.back().get();
    }

    void printRegisters() {
        for (const auto& block : validNodes()) {
            for (const auto& reg : block->getRegistersConst()) {
                auto root = getRoot(reg->getHandle()).getHandle();
                println("id: {} - mName: {} - uses: {} - parent: {} - type: {} - size: {}", reg->getHandle().toString(), reg->name, reg->useCount, root == reg->getHandle() ? "_" : root.toString(), reg->typeString(), reg->sizeBytes());
            }
        }
    }

    const CTX::REG& getRecordConst(SSARegisterHandle target) const {
        assert(target.isValid());
        const auto& block = getBlockConst(target.graphId);

        return block.getRecordConst(target.registerId);
    }

    const CTX::REG& getRootConst(SSARegisterHandle target) const {
        assert(target.isValid());
        const auto* current = &getBlockConst(target.graphId).getRecordConst(target.registerId);

        while (!current->isRoot()) {
            auto t1 = current->getPrevious();
            assert(t1.has_value());
            current = &getBlockConst(t1->graphId).getRecordConst(t1->registerId);
        }

        return *current;
    }

    Result<void> validate() {
        for (auto& block: validNodesConst()) {
            bool endOfPhis = false;
            bool encounteredTerminator = false;

            for (auto& inst: block->getInstructions()) {
                if (inst->template is<instructions::PhiFunction<CTX>>() && !endOfPhis) {
                    continue;
                }
                if (inst->isTerminal() && encounteredTerminator) {
                    return FAIL("multiple terminal instructions");
                }
                if (inst->isTerminal() && !encounteredTerminator) {
                    encounteredTerminator = true;
                    continue;
                }
                if (encounteredTerminator) {
                    return FAIL("instructions after terminator");
                }
                endOfPhis = true;
            }
        }

        return {};
    }

    void fixupUseCount() {
        for (auto& block : validNodes()) {
            for (auto& inst : block->getInstructionsMut()) {
                if (not inst->target.isValid()) continue;
                auto& reg = getRecord(inst->target);

                auto* phiPtr = dynamic_cast<instructions::PhiFunction<CTX>*>(inst.get());
                if (phiPtr != nullptr) {
                    reg.useCount = (calculateUseCount(inst->target) + phiPtr->versionsCount())-1;
                }
                else {
                    reg.useCount = calculateUseCount(inst->target);
                }
            }
        }
    }

    ControlFlowGraph::DependencyMap buildDependencyMap() const {
        DependencyMap buf;

        for (auto& block : validNodesConst()) {
            for (auto target : block->getTargets()) {
                buf[target].insert(block->blockId);
            }
        }

        return buf;
    }

    bool canReach(size_t source, size_t target) const {
        unordered_set<size_t> visited;
        vector<size_t> toVisit;

        toVisit.push_back(source);

        while (not toVisit.empty()) {
            auto current = toVisit.back(); toVisit.pop_back();

            if (visited.contains(current)) continue;
            visited.insert(current);

            if (current == target) return true;

            for (auto curTarget : getBlockConst(current).getTargets()) {
                toVisit.push_back(curTarget);
            }
        }

        return false;
    }

    ControlFlowGraph::DependencyMap buildDependencyMapDeep() const {
        DependencyMap buf;

        for (auto& block : validNodesConst()) {
            for (auto target : block->getTargets()) {
                buf[target].insert(block->blockId);
                auto& src = buf[block->blockId];
                auto& wtf = buf[target];
                wtf.insert(src.begin(), src.end());
            }
        }

        return buf;
    }

    size_t calculateUseCount(SSARegisterHandle reg) {
        size_t useCount = 0;

        for (auto& block : validNodes()) {
            for (auto& inst : block->getInstructionsMut()) {
                inst->visitSrc([&](auto usedReg) {
                    if (usedReg == reg) {
                        useCount++;
                    }
                });
            }
        }

        return useCount;
    }

    bool canReach(size_t source, size_t target, unordered_set<size_t>& ignore1) const {
        unordered_set<size_t> visited{ignore1.begin(), ignore1.end()};
        vector<size_t> toVisit;

        toVisit.push_back(source);

        while (not toVisit.empty()) {
            auto current = toVisit.back(); toVisit.pop_back();

            if (current == target) return true;

            if (visited.contains(current)) continue;
            visited.insert(current);

            for (auto curTarget : getBlockConst(current).getTargets()) {
                toVisit.push_back(curTarget);
            }
        }

        return false;
    }

    vector<vector<size_t>> inverseTreeP(const ControlFlowGraph<CTX>& graph) const {
        vector<vector<size_t>> res;
        for (auto _i : views::iota(0u, graph.nodeCount())) {
            (void)_i;
            res.emplace_back();
        }


        for (const auto& node : graph.validNodesConst()) {
            for (auto tgt : node.get()->getTargets()) {
                res[tgt].push_back(node.get()->blockId);
            }
        }

        return res;
    }

    optional<size_t> indexOfLast(const vector<size_t>& pepa, const auto& pred) const {
        // we could iterate in reverse :)
        optional<size_t> n;
        for (const auto& [i, item] : pepa | views::enumerate) {
            if (pred(item)) n = i;
        }
        return n;
    }

    vector<size_t> flattenBlocks() const {
        vector<size_t> toProcessStack;
        vector<size_t> result;
        set<size_t> resolved;
        auto inverseTree = inverseTreeP(*this);
        vector<size_t> priorityStack;

        toProcessStack.push_back(0);

        const auto visit = [&](size_t n) {
            if (resolved.contains(n)) return;
            toProcessStack.push_back(n);
        };
        auto graph = *this;

        const auto canReach = [&](size_t src, size_t dst) -> bool {
            stack<size_t> toVisit;
            set<size_t> visited;
            toVisit.push(src);

            while (not toVisit.empty()) {
                auto cur = toVisit.top(); toVisit.pop();
                if (cur == dst) return true;
                if (resolved.contains(cur) || visited.contains(cur)) continue;
                visited.insert(cur);
                for (auto child : graph.getBlock(cur).getTargets()) {
                    toVisit.push(child);
                }
            }
            return false;
        };

        auto unresolvedIncomingEdges = [&](size_t node) -> vector<size_t> { return inverseTree[node] | views::filter([&](const auto& it) { return !resolved.contains(it); }) | vi::toVec; };
        auto isAllIncomingResolved = [&](size_t node) -> bool { return unresolvedIncomingEdges(node).empty(); };

        const auto removePriorityIfResolve = [&]() {
            if (priorityStack.empty()) return;
            priorityStack.erase(std::remove_if(priorityStack.begin(), priorityStack.end(), [&](const auto& it) { return isAllIncomingResolved(it); }), priorityStack.end());
        };

        const auto markResolved = [&](size_t n) {
            result.push_back(n);
            resolved.insert(n);
            removePriorityIfResolve();
        };

        while (not toProcessStack.empty()) {
            optional<size_t> res;
            if (not priorityStack.empty()) {
                res = indexOfLast(toProcessStack, [&](const auto& it) { return canReach(it, priorityStack.back()) && isAllIncomingResolved(it); });
            }
            if (not res.has_value()) {
                res = indexOfLast(toProcessStack, [&](const auto& it) { return !isAllIncomingResolved(it) && canReach(it, unresolvedIncomingEdges(it)[0]); });
            }
            if (not res.has_value()) {
                res = indexOfLast(toProcessStack, [&](const auto& it) { return isAllIncomingResolved(it); });
            }

            size_t top;
            if (res.has_value()) {
                top = toProcessStack[*res];
                toProcessStack.erase(toProcessStack.begin()+*res);
            }
            else {
                top = toProcessStack.back(); toProcessStack.pop_back();
            }

            if (resolved.contains(top)) continue;
            markResolved(top);

            auto children = graph.getBlock(top).getTargets();

            for (auto child : children) {
                visit(child);
            }

            if (!isAllIncomingResolved(top)) {
                if (children.empty()) TODO();
                priorityStack.push_back(top);
            }
        }

        return result;
    }

    vector<size_t> lookupIngress(size_t id) const {
        vector<size_t> buf;

        for (const auto& block : validNodesConst()) {
            if (block->isEmpty()) continue;

            auto& inst = block->getInstruction(-1);

            if (inst->template is<instructions::Jump<CTX>>()) {
                if (inst->template cst<instructions::Jump<CTX>>()->value == id) {
                    buf.push_back(block->blockId);
                }
            }
            else if (inst->template is<instructions::Branch<CTX>>()) {
                auto branch = inst->template cst<instructions::Branch<CTX>>();
                if (branch->scopeT == id || branch->scopeF == id) {
                    buf.push_back(block->blockId);
                }
            }
            else if (inst->template is<instructions::FallTrough<CTX>>()) {
                if (inst->template cst<instructions::FallTrough<CTX>>()->value == id) {
                    buf.push_back(block->blockId);
                }
            }
            else if (inst->template is<instructions::JumpTrue<CTX>>()) {
                auto branch = inst->template cst<instructions::JumpTrue<CTX>>();
                if (branch->scopeT == id || branch->scopeF == id) {
                    buf.push_back(block->blockId);
                }
            }
            else if (inst->template is<instructions::JumpFalse<CTX>>()) {
                auto branch = inst->template cst<instructions::JumpTrue<CTX>>();
                if (branch->scopeT == id || branch->scopeF == id) {
                    buf.push_back(block->blockId);
                }
            }
        }

        return buf;
    }

    map<SSARegisterHandle, vector<bool>> liveRanges(vector<size_t> blocks) const {
        map<SSARegisterHandle, vector<bool>> ranges;
        map<SSARegisterHandle, size_t> registerStarts;

        size_t instructionCount = 0;
        map<size_t, size_t> instructionRangeOffset;

        map<size_t, set<SSARegisterHandle>> liveIns;

        for (auto block : blocks) {
            instructionRangeOffset[block] = instructionCount;

            auto instCount = getBlockConst(block).instructions.size();
            instructionCount += instCount;
        }

        // NOTE bypasssStart ignores start index of register needed for correct loop handling
        auto addRange = [&](SSARegisterHandle handle, size_t s, size_t e, bool bypassStart = false) {
            assert(handle.isValid());
            auto& range = ranges[handle];

            if (range.empty()) {
                range.insert(range.end(), instructionCount, false);
            }

            auto start = (registerStarts.contains(handle) && !bypassStart) ? max(min(s, e), registerStarts[handle]) : min(s, e);
            auto end = min(max(s, e)+1, range.size());

            if (end < start) return;

            for (auto id : views::iota(start, end)) {
                range[id] = true;
            }
        };

        auto getLiveIns = [&](size_t blockId) -> set<SSARegisterHandle> {
            return liveIns[blockId];
        };

        auto setLiveIns = [&](size_t blockId, set<SSARegisterHandle> regs1) {
            liveIns[blockId] = std::move(regs1);
        };

        auto getBlockPhis = [&](size_t blockId) {
            return getBlockConst(blockId).getPhiSpan();
        };

        auto getBlockRanges = [&](size_t blockId) -> pair<size_t, size_t> {
            auto start = instructionRangeOffset[blockId];
            auto instructionCount = getBlockConst(blockId).instructionCount();
            assert(instructionCount > 0);
            // it theoretically should make sense bcs views::iota iterates by (start...<end) and we add +1 at the place
            // but start + instructionCount gives index 1 larger than possible
            // so we should output (start..end) not (start..end+1)
            auto end = start+(instructionCount-1); // FIXME not sure about this -1

            return {start, end};
        };

        auto getBlock = [&](size_t blockId) -> const BaseBlock& {
            return getBlockConst(blockId);
        };

        // NOTE this indeed screws handling for loops, but it's fixed by forced fixup by is bypassStart
        auto setFrom = [&](SSARegisterHandle reg, size_t instId) {
            registerStarts[reg] = instId;
            if (not ranges.contains(reg)) return;
            auto& range = ranges[reg];
            for (auto id : views::iota(0u, instId)) {
                range[id] = false;
            }
        };

        auto isLoopHeader = [&](size_t blockId) -> bool {
            return getBlock(blockId).isLoopHeader();
        };

        auto getLastBlockOfLoop = [&](size_t blockId) -> size_t {
            // get incoming blocks to header
            // find the farthest block in liner representation
            // TODO i think we know the information in previous pass so we can just pass it back?
            auto ingress = lookupIngress(blockId) | views::filter([&](auto it) { return contains(blocks, it); }) | views::transform([&](auto it) { return make_pair(it, indexOf(blocks, it)); });
            assert(not ingress.empty());
            auto srcIndex = indexOf(blocks, blockId);

            auto max = pair{ingress.front().first, difference(ingress.front().second, srcIndex)};
            for (auto [ingId, ingIndex] : ingress) {
                auto diff = difference(srcIndex, ingIndex);
                if (diff >= max.second) max = {ingId, diff}; // it can happend that this returns the inbound block that isnt actualy part of the loop
            }

            return max.first;
        };

        auto getBlockSuccesors = [&](size_t blockId) -> vector<size_t> {
            return getBlock(blockId).getTargets();
        };

        // for each block b in reverse order do
        for (auto blockId : blocks | views::reverse) {
            auto currentSuccesors = getBlockSuccesors(blockId);
            auto blockRange = getBlockRanges(blockId);

            // live = union of successor.liveIn for each successor of b
            auto live = set<SSARegisterHandle>();
            for (auto succesorId: currentSuccesors) {
                auto liveIns1 = getLiveIns(succesorId);
                live.insert(liveIns1.begin(), liveIns1.end());
            }

            // for each phi function phi of successors of b do
            //     live.add(phi.inputOf(b))
            for (auto succesorId: currentSuccesors) {
                for (auto* phi: getBlockPhis(succesorId)) {
                    auto res = phi->getBySource(blockId);
                    if (res.has_value()) {
                        live.insert(*res);
                    }
                }
            }

            // for each opd in live do
            //    intervals[opd].addRange(b.from, b.to)
            for (auto reg: live) {
                addRange(reg, blockRange.first, blockRange.second);
            }

            // for each operation op of b in reverse order do
            for (auto [index, operation] : getBlock(blockId).instructions | views::enumerate | views::reverse) {
                // "Phi functions are not processed during this iteration of operations, instead they are iterated separately"
                if (operation->template is<instructions::PhiFunction<CTX>>()) continue;

                //    for each output operand opd of op do
                //       intervals[opd].setFrom(op.id)
                //       live.remove(opd)
                auto tgt = operation->target;
                if (tgt.isValid()) {
                    setFrom(tgt, blockRange.first+index);
                }
                live.erase(tgt);

                //    for each input operand opd of op do
                //       intervals[opd].addRange(b.from, op.id)
                //       live.add(opd)
                operation->visitSrc([&](auto& source) {
                    if (!source.isValid()) return; // return can have invalid src
                    addRange(source, blockRange.first, blockRange.first+index);
                    live.insert(source);
                });

                // for each phi function phi of b do
                //    live.remove(phi.output)
                for (auto* phi: getBlockPhis(blockId)) {
                    live.erase(phi->target);
                }

                // if b is loop header then
                if (isLoopHeader(blockId)) {
                    //      loopEnd = last block of the loop starting at b
                    //      for each opd in live do
                    //          intervals[opd].addRange(b.from, loopEnd.to)
                    auto loopEnd = getLastBlockOfLoop(blockId);
                    auto loopEndEnd = getBlockRanges(loopEnd).second;
                    for (auto reg : live) {
                        addRange(reg, blockRange.first, loopEndEnd, true);
                    }
                }

                // b.liveIn = live
                setLiveIns(blockId, live);
            }
        }

        return ranges;
    }

    void commitReachableDefs(map<SSARegisterHandle, instructions::PhiFunction<CTX>*> phis, const BaseBlock& endBlock) const {
        auto end = reachableDefinitions(endBlock);

        for (const auto& [fst, snd]: end) {
            if (phis.contains(fst)) {
                auto* phiRef = phis[fst];
                // if (phiRef->target == snd) continue;
                // if (gen.getRecord(phiRef->getVersions()[0]).version > gen.getRecord(fst).version) return;

                phiRef->pushVersion(snd, endBlock.blockId);
            }
        }
    }

    map<SSARegisterHandle, SSARegisterHandle> reachableDefinitions(const BaseBlock& block) const {
        map<SSARegisterHandle, SSARegisterHandle> oldes;

        const BaseBlock* currentBlock = &block;
        while (currentBlock != nullptr) {
            // reverse used bcs we want to register the newest register version witch is at the end, THIS ASSUMPTION DOESNT HAVE TO HOLD TRUE
            // 1. FIXME this doesnt correctly represent the version and we should iterate over the instructions in the block and get their targets
            // 2. this whole function depends on block.previous which is kinda outdating and propably inacurate representation
            // we should do block.getPredecessors and iterate until we get to root ignoring already visited
            // if we encounter IF/ELSE it wont actualy affect us bcs if they modify regs then phi function will be generated after them
            for (const auto& regHandle : currentBlock->getInstructions() | views::transform([&](const CopyPtr<IRInstruction<CTX>>& it) { return it->target; }) | views::filter([](const SSARegisterHandle& it) { return it.isValid(); }) | views::reverse) {
                assert(regHandle.graphId == currentBlock->id());
                auto& reg = currentBlock->getRecordConst(regHandle.registerId);
                if (reg.isTemp()) continue;

                if (reg.getPrevious().has_value()) {
                    const auto root = getRootConst(*reg.getPrevious());

                    // NOTE: we are iterating from the youngest to the oldest if we already encountered younger version we shouldn't replace it with older none
                    // + reg.version doesn't seem to be working
                    if (oldes.contains(root.getHandle())) continue;

                    oldes[root.getHandle()] = regHandle;
                }
                else {
                    if (!oldes.contains(regHandle)) {
                        oldes[regHandle] = regHandle;
                    }
                }
            }

            currentBlock = currentBlock->previousId().has_value() ? &getBlockConst(*currentBlock->previousId()) : nullptr;
        }

        return oldes;
    }

    optional<SSARegisterHandle> lookupOptionalLocal(string_view name, BaseBlock& _block) {
        BaseBlock* block = &_block;

        while (!block->getRegisterByName(name).has_value()) {
            if (!block->previousId().has_value()) {
                return {};
            }

            block = &getBlock(*block->previousId());
        }

        return block->getRegisterHandleByName(name);
    }


private:
    vector<CopyPtr<BaseBlock>> nodes;
};