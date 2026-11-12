// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Ported from Hookshot (ir_opt/oot_visual_workaround.h, GuardFragmentOutputs).

#pragma once

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"

namespace Shader::Optimization {

/// Replaces NaN with 0 and clamps to +-1e30 in every float colour output of a fragment program.
/// One NaN pixel in a scene target spreads through the bloom's chain of averages to the whole
/// picture (the one-frame white flash in Ocarina of Time, with the HUD over it).
/// float_targets: bit i is set when colour target i is a float format.
/// Constant values are sanitized in place; other values get a clamp and a NaN select in front.
/// Returns the number of float colour writes guarded.
inline size_t GuardFragmentOutputs(IR::Program& program, u32 float_targets) {
    if (program.stage != Stage::Fragment) {
        return 0;
    }
    std::vector<std::pair<IR::Block*, IR::Inst*>> writes;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::SetFragColor && inst.Arg(0).IsImmediate() &&
                inst.Arg(0).U32() < 8 && ((float_targets >> inst.Arg(0).U32()) & 1) != 0) {
                writes.emplace_back(block, &inst);
            }
        }
    }
    for (const auto& [block, inst] : writes) {
        if (inst->Arg(2).IsImmediate()) {
            // A constant needs no instructions: sanitize it here. Immediate also covers constants
            // reached through Identity instructions, so this does not depend on constant folding.
            const f32 value{inst->Arg(2).F32()};
            const f32 safe{std::isnan(value) ? 0.0f : std::clamp(value, -1e30f, 1e30f)};
            if (std::isnan(value) || safe != value) {
                inst->SetArg(2, IR::Value{safe});
            }
            continue;
        }
        IR::IREmitter ir{*block, block->Instructions().iterator_to(*inst)};
        const IR::F32 value{inst->Arg(2)};
        const IR::F32 limited{ir.FPClamp(value, ir.Imm32(-1e30f), ir.Imm32(1e30f))};
        const IR::F32 safe{ir.Select(ir.FPIsNan(value), ir.Imm32(0.0f), limited)};
        inst->SetArg(2, safe);
    }
    return writes.size();
}

} // namespace Shader::Optimization
