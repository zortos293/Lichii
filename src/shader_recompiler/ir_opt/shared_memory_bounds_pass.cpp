// SPDX-License-Identifier: GPL-3.0-or-later

#include <optional>

#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace Shader::Optimization {
namespace {
// Bytes a shared memory access touches, or nullopt for other instructions.
std::optional<u32> SharedAccessBytes(IR::Opcode opcode) {
    switch (opcode) {
    case IR::Opcode::LoadSharedU8:
    case IR::Opcode::LoadSharedS8:
    case IR::Opcode::WriteSharedU8:
        return 1;
    case IR::Opcode::LoadSharedU16:
    case IR::Opcode::LoadSharedS16:
    case IR::Opcode::WriteSharedU16:
        return 2;
    case IR::Opcode::LoadSharedU32:
    case IR::Opcode::WriteSharedU32:
    case IR::Opcode::SharedAtomicIAdd32:
    case IR::Opcode::SharedAtomicSMin32:
    case IR::Opcode::SharedAtomicUMin32:
    case IR::Opcode::SharedAtomicSMax32:
    case IR::Opcode::SharedAtomicUMax32:
    case IR::Opcode::SharedAtomicInc32:
    case IR::Opcode::SharedAtomicDec32:
    case IR::Opcode::SharedAtomicAnd32:
    case IR::Opcode::SharedAtomicOr32:
    case IR::Opcode::SharedAtomicXor32:
    case IR::Opcode::SharedAtomicExchange32:
        return 4;
    case IR::Opcode::LoadSharedU64:
    case IR::Opcode::WriteSharedU64:
    case IR::Opcode::SharedAtomicExchange64:
    case IR::Opcode::SharedAtomicExchange32x2:
        return 8;
    case IR::Opcode::LoadSharedU128:
    case IR::Opcode::WriteSharedU128:
        return 16;
    default:
        return std::nullopt;
    }
}
} // Anonymous namespace

void SharedMemoryBoundsPass(IR::Program& program, u32 shared_memory_size) {
    // Accesses that do not fit in the shared memory the shader got are sent to its last 16 bytes
    // instead, so they stay inside the allocation. Their results are wrong, and redirected stores
    // from different invocations may race on those bytes.
    if (shared_memory_size < 16) {
        return;
    }
    const u32 sink{(shared_memory_size - 16) & ~15U};
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            const std::optional<u32> bytes{SharedAccessBytes(inst.GetOpcode())};
            if (!bytes) {
                continue;
            }
            const u32 last{shared_memory_size - *bytes};
            const IR::Value offset{inst.Arg(0)};
            if (offset.IsImmediate()) {
                if (offset.U32() > last) {
                    inst.SetArg(0, IR::Value{sink});
                }
                continue;
            }
            IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
            const IR::U32 dynamic_offset{offset};
            const IR::U1 in_range{ir.ILessThanEqual(dynamic_offset, ir.Imm32(last), false)};
            inst.SetArg(0, ir.Select(in_range, dynamic_offset, ir.Imm32(sink)));
        }
    }
}

} // namespace Shader::Optimization
