// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <unordered_set>
#include <vector>

#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace Shader::Optimization {
namespace {
using NodeType = IR::AbstractSyntaxNode::Type;

// Values that can differ between the invocations of a workgroup whatever their operands are.
bool IsDivergentSource(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::LocalInvocationId:
    case IR::Opcode::InvocationId:
    case IR::Opcode::SampleId:
    case IR::Opcode::IsHelperInvocation:
    case IR::Opcode::GetAttribute:
    case IR::Opcode::GetAttributeU32:
    case IR::Opcode::GetAttributeIndexed:
    case IR::Opcode::IsbeRead:
    case IR::Opcode::LoadLocal:
    case IR::Opcode::LaneId:
    case IR::Opcode::SubgroupBallot:
    case IR::Opcode::SubgroupEqMask:
    case IR::Opcode::SubgroupLtMask:
    case IR::Opcode::SubgroupLeMask:
    case IR::Opcode::SubgroupGtMask:
    case IR::Opcode::SubgroupGeMask:
    case IR::Opcode::FSwizzleAdd:
    case IR::Opcode::DPdxFine:
    case IR::Opcode::DPdyFine:
    case IR::Opcode::DPdxCoarse:
    case IR::Opcode::DPdyCoarse:
    case IR::Opcode::GetInBoundsFromOp:
        return true;
    case IR::Opcode::ConditionRef:
        return false;
    default:
        // Atomics return a different value to each invocation.
        return inst.MayHaveSideEffects() && inst.Type() != IR::Type::Void;
    }
}

// A conservative divergence analysis over the structured program: a value is uniform when every
// invocation of the workgroup that computes it gets the same result, and control flow is uniform
// when every invocation that reaches a construct takes the same path through it.
class Divergence {
public:
    explicit Divergence(IR::Program& program_)
        : program{program_}, divergent_loops(program.syntax_list.size()) {
        while (Walk(false)) {
        }
    }

    void DemoteBarriers() {
        Walk(true);
    }

private:
    struct Construct {
        size_t node;
        bool divergent;
    };

    bool IsDivergent(const IR::Value& value) const {
        return !value.IsImmediate() && values.contains(value.InstRecursive());
    }

    bool Mark(const IR::Inst& inst, bool divergent_loop, bool divergent_merge) {
        if (values.contains(&inst)) {
            return false;
        }
        bool divergent{divergent_loop || IsDivergentSource(inst) ||
                       (divergent_merge && inst.GetOpcode() == IR::Opcode::Phi)};
        for (size_t arg = 0; !divergent && arg < inst.NumArgs(); ++arg) {
            divergent = IsDivergent(inst.Arg(arg));
        }
        return divergent && values.insert(&inst).second;
    }

    bool MarkLoop(size_t node) {
        if (divergent_loops[node]) {
            return false;
        }
        divergent_loops[node] = true;
        return true;
    }

    // One pass in program order. Returns true when it found new divergence, so loop back edges
    // and divergence found after a construct's start reach a fixed point.
    bool Walk(bool demote) {
        std::vector<Construct> stack;
        bool exited{};
        bool changed{};
        auto& syntax_list{program.syntax_list};
        for (size_t index = 0; index < syntax_list.size(); ++index) {
            const IR::AbstractSyntaxNode& node{syntax_list[index]};
            // Once part of the workgroup has returned, the rest runs without it.
            const bool divergent_flow{
                exited || std::ranges::any_of(stack, &Construct::divergent)};
            switch (node.type) {
            case NodeType::Block: {
                // Values computed in a loop that invocations leave on different iterations differ
                // after it, even when every iteration computes them uniformly.
                const bool divergent_loop{std::ranges::any_of(stack, [&](const Construct& c) {
                    return syntax_list[c.node].type == NodeType::Loop && c.divergent;
                })};
                const bool divergent_merge{divergent_merges.contains(node.data.block)};
                for (IR::Inst& inst : node.data.block->Instructions()) {
                    if (demote && divergent_flow && inst.GetOpcode() == IR::Opcode::Barrier) {
                        LOG_WARNING(Shader, "Barrier in divergent control flow keeps only its "
                                            "memory ordering");
                        inst.ReplaceOpcode(IR::Opcode::WorkgroupMemoryBarrier);
                        program.info.demoted_conditional_barrier = true;
                        continue;
                    }
                    changed |= Mark(inst, divergent_loop, divergent_merge);
                }
                break;
            }
            case NodeType::If: {
                const bool divergent{IsDivergent(node.data.if_node.cond)};
                if (divergent) {
                    changed |= divergent_merges.insert(node.data.if_node.merge).second;
                }
                stack.push_back({index, divergent});
                break;
            }
            case NodeType::Loop:
                stack.push_back({index, divergent_loops[index]});
                break;
            case NodeType::Break: {
                if (!divergent_flow && !IsDivergent(node.data.break_node.cond)) {
                    break;
                }
                const auto loop{std::ranges::find_if(stack.rbegin(), stack.rend(),
                                                     [&](const Construct& c) {
                                                         return syntax_list[c.node].type ==
                                                                NodeType::Loop;
                                                     })};
                if (loop != stack.rend()) {
                    changed |= MarkLoop(loop->node);
                }
                break;
            }
            case NodeType::EndIf:
                stack.pop_back();
                break;
            case NodeType::Repeat: {
                const size_t loop{stack.back().node};
                if (IsDivergent(node.data.repeat.cond)) {
                    changed |= MarkLoop(loop);
                }
                if (divergent_loops[loop]) {
                    changed |= divergent_merges.insert(node.data.repeat.merge).second;
                }
                stack.pop_back();
                break;
            }
            case NodeType::Return:
                if (!divergent_flow) {
                    break;
                }
                // The invocations still in an enclosing loop run its next iterations without the
                // ones that returned.
                exited = true;
                for (const Construct& construct : stack) {
                    if (syntax_list[construct.node].type == NodeType::Loop) {
                        changed |= MarkLoop(construct.node);
                    }
                }
                break;
            case NodeType::Unreachable:
                break;
            }
        }
        ASSERT(stack.empty());
        return changed;
    }

    IR::Program& program;
    std::unordered_set<const IR::Inst*> values;
    std::unordered_set<const IR::Block*> divergent_merges;
    std::vector<bool> divergent_loops;
};
} // Anonymous namespace

// Some drivers hang when only part of a workgroup reaches a barrier, which the guest allows: the
// invocations that skip it or have exited count as arrived. Barriers under divergent control flow
// keep their memory ordering without the execution barrier; the rest stay as they are.
void ConditionalBarrierPass(IR::Program& program) {
    Divergence{program}.DemoteBarriers();
}

} // namespace Shader::Optimization
