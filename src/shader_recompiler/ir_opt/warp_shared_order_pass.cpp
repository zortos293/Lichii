// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <string_view>
#include <unordered_map>

#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace Shader::Optimization {
namespace {
// Shared memory accesses that may still be unordered, from least to most
enum class Pending : u8 {
    None,
    Atomic,
    Store,
};

bool IsSharedAtomic(IR::Opcode opcode) {
    return std::string_view{IR::NameOf(opcode)}.starts_with("SharedAtomic");
}

bool WritesShared(IR::Opcode opcode) {
    switch (opcode) {
    case IR::Opcode::WriteSharedU8:
    case IR::Opcode::WriteSharedU16:
    case IR::Opcode::WriteSharedU32:
    case IR::Opcode::WriteSharedU64:
    case IR::Opcode::WriteSharedU128:
        return true;
    default:
        return false;
    }
}

bool LoadsShared(IR::Opcode opcode) {
    switch (opcode) {
    case IR::Opcode::LoadSharedU8:
    case IR::Opcode::LoadSharedS8:
    case IR::Opcode::LoadSharedU16:
    case IR::Opcode::LoadSharedS16:
    case IR::Opcode::LoadSharedU32:
    case IR::Opcode::LoadSharedU64:
    case IR::Opcode::LoadSharedU128:
        return true;
    default:
        return false;
    }
}

bool OrdersShared(IR::Opcode opcode) {
    switch (opcode) {
    case IR::Opcode::Barrier:
    case IR::Opcode::WorkgroupMemoryBarrier:
    case IR::Opcode::DeviceMemoryBarrier:
    case IR::Opcode::VotedBarrier:
    case IR::Opcode::WarpSharedMemoryBarrier:
        return true;
    default:
        return false;
    }
}

// Updates `pending` past an instruction, and returns whether it needs a barrier before it.
// Atomics read the word they modify, so they wait for plain stores; atomics on the same word are
// already ordered among themselves.
bool Step(IR::Opcode opcode, Pending& pending) {
    if (OrdersShared(opcode)) {
        pending = Pending::None;
        return false;
    }
    const bool atomic{IsSharedAtomic(opcode)};
    const bool needs_barrier{(LoadsShared(opcode) && pending != Pending::None) ||
                             (atomic && pending == Pending::Store)};
    if (needs_barrier) {
        pending = Pending::None;
    }
    if (WritesShared(opcode)) {
        pending = Pending::Store;
    } else if (atomic) {
        pending = std::max(pending, Pending::Atomic);
    }
    return needs_barrier;
}

Pending Transfer(const IR::Block& block, Pending pending) {
    for (const IR::Inst& inst : block) {
        Step(inst.GetOpcode(), pending);
    }
    return pending;
}
} // Anonymous namespace

// NVIDIA guest code reads words other threads of its warp stored in shared memory with no barrier
// in between (warp-synchronous scans and reductions): the warp runs in step. Vulkan orders such
// accesses only through a barrier. A shared load that a store can reach with no barrier on the way
// gets a subgroup memory barrier before it, and the program runs whole guest warps.
// This is a memory barrier, not an execution barrier: it relies on the host running the subgroup
// in step, as AMD does. The Vulkan memory model does not guarantee it, and NVIDIA hosts, whose
// threads can drift apart within a subgroup, may still read the old value.
void WarpSharedOrderPass(IR::Program& program) {
    std::unordered_map<const IR::Block*, Pending> pending_out;
    const auto pending_in = [&](const IR::Block& block) {
        Pending pending{Pending::None};
        for (const IR::Block* const pred : block.ImmPredecessors()) {
            const auto it{pending_out.find(pred)};
            if (it != pending_out.end()) {
                pending = std::max(pending, it->second);
            }
        }
        return pending;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (const IR::Block* const block : program.blocks) {
            const Pending out{Transfer(*block, pending_in(*block))};
            auto [it, inserted]{pending_out.try_emplace(block, out)};
            if (inserted || it->second != out) {
                it->second = out;
                changed = true;
            }
        }
    }
    for (IR::Block* const block : program.blocks) {
        Pending pending{pending_in(*block)};
        for (auto it = block->begin(); it != block->end(); ++it) {
            if (Step(it->GetOpcode(), pending)) {
                block->PrependNewInst(it, IR::Opcode::WarpSharedMemoryBarrier);
                program.info.orders_warp_shared_memory = true;
            }
        }
    }
}

} // namespace Shader::Optimization
