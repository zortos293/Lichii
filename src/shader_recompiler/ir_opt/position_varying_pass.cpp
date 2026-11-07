// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2026 sudachi Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <optional>
#include <unordered_map>
#include <vector>

#include "common/logging.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/shader_info.h"

// Adapted from Eden PR #4575 (2724572c8ce82e8dd39c8c6776c52d63f5c92ae9).
// Games often pass the clip space position to the fragment shader through a generic varying and
// divide it by its w to get the screen position (e.g. to fetch the depth buffer at the current
// pixel). On the guest GPU the fragment shader interpolates the original vertex values and the
// division is exact. Some host GPUs (AMD) clip huge primitives crossing the w = 0 plane and
// interpolate the varyings of the clipped vertices with much less precision than the position
// used for rasterization: in Xenoblade Chronicles 2 the screen position computed this way is off
// by up to 1.5 pixels, so depth fetches hit neighbour texels.
//
// When the vertex shader stores the same value (or the same value plus a draw-uniform offset) in
// the x, y and w components of a generic varying and of the position, the fragment shader can
// rebuild those components exactly from gl_FragCoord and the host viewport. Uniform offsets are
// moved to a free flat varying.

namespace Shader::Optimization {
namespace {
constexpr std::array<size_t, 3> RECONSTRUCTED_COMPONENTS{0, 1, 3};

struct Store {
    IR::Block* block{};
    IR::Inst* inst{};
    IR::Value value;
    u32 count{};
};

IR::Value Resolve(const IR::Value& value) {
    return value.IsImmediate() ? value : value.Resolve();
}

bool SameValue(const IR::Value& lhs, const IR::Value& rhs) {
    const IR::Value a{Resolve(lhs)};
    const IR::Value b{Resolve(rhs)};
    if (a.IsImmediate() != b.IsImmediate()) {
        return false;
    }
    if (a.IsImmediate()) {
        return a == b;
    }
    return a.InstRecursive() == b.InstRecursive();
}

/// Values that are the same for every vertex of a draw (built from immediates and constant
/// buffers only)
class UniformChecker {
public:
    bool IsUniform(const IR::Value& value, u32 depth = 0) {
        if (value.IsEmpty()) {
            return false;
        }
        if (value.IsImmediate()) {
            return true;
        }
        IR::Inst* const inst{value.InstRecursive()};
        if (const auto it = cache.find(inst); it != cache.end()) {
            return it->second;
        }
        bool uniform = depth < 64 && IsAllowedOperation(inst->GetOpcode());
        for (size_t arg = 0; uniform && arg < inst->NumArgs(); ++arg) {
            const IR::Value operand{inst->Arg(arg)};
            if (operand.IsImmediate()) {
                continue;
            }
            if (operand.Type() == IR::Type::Attribute) {
                uniform = false;
                break;
            }
            uniform = IsUniform(operand, depth + 1);
        }
        cache.emplace(inst, uniform);
        return uniform;
    }

private:
    static bool IsAllowedOperation(IR::Opcode opcode) {
        // Only constant-buffer reads and explicitly pure arithmetic may produce an offset.
        // The recursive check also requires every constant-buffer address to be draw-uniform.
        // New getters, unresolved registers/predicates and Phi must fail closed.
        switch (opcode) {
        case IR::Opcode::GetCbufU8:
        case IR::Opcode::GetCbufS8:
        case IR::Opcode::GetCbufU16:
        case IR::Opcode::GetCbufS16:
        case IR::Opcode::GetCbufU32:
        case IR::Opcode::GetCbufF32:
        case IR::Opcode::GetCbufU32x2:
        case IR::Opcode::CompositeConstructU32x2:
        case IR::Opcode::CompositeConstructU32x3:
        case IR::Opcode::CompositeConstructU32x4:
        case IR::Opcode::CompositeExtractU32x2:
        case IR::Opcode::CompositeExtractU32x3:
        case IR::Opcode::CompositeExtractU32x4:
        case IR::Opcode::CompositeInsertU32x2:
        case IR::Opcode::CompositeInsertU32x3:
        case IR::Opcode::CompositeInsertU32x4:
        case IR::Opcode::CompositeConstructF32x2:
        case IR::Opcode::CompositeConstructF32x3:
        case IR::Opcode::CompositeConstructF32x4:
        case IR::Opcode::CompositeExtractF32x2:
        case IR::Opcode::CompositeExtractF32x3:
        case IR::Opcode::CompositeExtractF32x4:
        case IR::Opcode::CompositeInsertF32x2:
        case IR::Opcode::CompositeInsertF32x3:
        case IR::Opcode::CompositeInsertF32x4:
        case IR::Opcode::BitCastU32F32:
        case IR::Opcode::BitCastF32U32:
        case IR::Opcode::ConvertF32U32:
        case IR::Opcode::ConvertF32S32:
        case IR::Opcode::ConvertU32F32:
        case IR::Opcode::ConvertS32F32:
        case IR::Opcode::FPAbs32:
        case IR::Opcode::FPAdd32:
        case IR::Opcode::FPFma32:
        case IR::Opcode::FPMax32:
        case IR::Opcode::FPMin32:
        case IR::Opcode::FPMul32:
        case IR::Opcode::FPNeg32:
        case IR::Opcode::FPRecip32:
        case IR::Opcode::FPSaturate32:
        case IR::Opcode::FPRoundEven32:
        case IR::Opcode::FPFloor32:
        case IR::Opcode::FPCeil32:
        case IR::Opcode::FPTrunc32:
        case IR::Opcode::IAdd32:
        case IR::Opcode::ISub32:
        case IR::Opcode::IMul32:
        case IR::Opcode::SMin32:
        case IR::Opcode::SMax32:
        case IR::Opcode::INeg32:
        case IR::Opcode::ShiftLeftLogical32:
        case IR::Opcode::ShiftRightLogical32:
        case IR::Opcode::ShiftRightArithmetic32:
        case IR::Opcode::BitwiseAnd32:
        case IR::Opcode::BitwiseOr32:
        case IR::Opcode::BitwiseXor32:
        case IR::Opcode::BitwiseNot32:
        case IR::Opcode::IEqual:
        case IR::Opcode::INotEqual:
        case IR::Opcode::LogicalAnd:
        case IR::Opcode::LogicalOr:
        case IR::Opcode::LogicalXor:
        case IR::Opcode::LogicalNot:
        case IR::Opcode::SelectU1:
        case IR::Opcode::SelectU32:
        case IR::Opcode::SelectF32:
            return true;
        default:
            return false;
        }
    }

    std::unordered_map<IR::Inst*, bool> cache;
};

/// Returns the offset of position over attribute (position = attribute + offset) when it is draw
/// uniform. std::nullopt when the position isn't derived from the attribute that way, and an
/// empty value when both are the same value.
std::optional<IR::Value> PositionOffset(UniformChecker& checker, const IR::Value& attribute,
                                        const IR::Value& position) {
    if (SameValue(attribute, position)) {
        return IR::Value{};
    }
    const IR::Value resolved{Resolve(position)};
    if (resolved.IsImmediate()) {
        return std::nullopt;
    }
    IR::Inst* const inst{resolved.InstRecursive()};
    switch (inst->GetOpcode()) {
    case IR::Opcode::FPAdd32:
        for (size_t arg = 0; arg < 2; ++arg) {
            if (SameValue(inst->Arg(arg), attribute) && checker.IsUniform(inst->Arg(1 - arg))) {
                return inst->Arg(1 - arg);
            }
        }
        break;
    case IR::Opcode::FPFma32:
        if (SameValue(inst->Arg(2), attribute) && checker.IsUniform(inst->Arg(0)) &&
            checker.IsUniform(inst->Arg(1))) {
            return resolved;
        }
        break;
    default:
        break;
    }
    return std::nullopt;
}

IR::Attribute GenericAttribute(size_t index, size_t component) {
    return IR::Attribute::Generic0X + (index * 4 + component);
}

IR::Attribute PositionAttribute(size_t component) {
    return IR::Attribute::PositionX + component;
}

struct Candidate {
    size_t index{};
    std::array<std::optional<IR::Value>, 4> offsets;
};
} // Anonymous namespace

void PositionVaryingPass(IR::Program& vertex, IR::Program& fragment) {
    Info& vs_info{vertex.info};
    Info& fs_info{fragment.info};
    if (vs_info.stores_indexed_attributes || fs_info.loads_indexed_attributes ||
        vs_info.stores[IR::Attribute::ViewportIndex] ||
        vs_info.stores[IR::Attribute::ViewportMask] || vs_info.uses_render_area ||
        fs_info.uses_render_area) {
        return;
    }
    // Collect the stores of the vertex shader
    std::unordered_map<IR::Attribute, Store> stores;
    for (IR::Block* const block : vertex.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::SetAttribute) {
                continue;
            }
            Store& store{stores[inst.Arg(0).Attribute()]};
            store.block = block;
            store.inst = &inst;
            store.value = inst.Arg(1);
            ++store.count;
        }
    }
    const auto unique_store = [&](IR::Attribute attribute) -> const Store* {
        const auto it = stores.find(attribute);
        return it != stores.end() && it->second.count == 1 ? &it->second : nullptr;
    };
    UniformChecker checker;
    std::vector<Candidate> candidates;
    for (size_t index = 0; index < 32; ++index) {
        if (fs_info.interpolation[index] != Interpolation::Smooth) {
            continue;
        }
        bool any_load = false;
        for (const size_t component : RECONSTRUCTED_COMPONENTS) {
            any_load |= fs_info.loads.Generic(index, component);
        }
        if (!any_load) {
            continue;
        }
        Candidate candidate{.index = index};
        bool matches = true;
        for (const size_t component : RECONSTRUCTED_COMPONENTS) {
            const Store* const attribute{unique_store(GenericAttribute(index, component))};
            const Store* const position{unique_store(PositionAttribute(component))};
            if (!attribute || !position || attribute->block != position->block) {
                matches = false;
                break;
            }
            const std::optional<IR::Value> offset{
                PositionOffset(checker, attribute->value, position->value)};
            if (!offset) {
                matches = false;
                break;
            }
            candidate.offsets[component] = offset;
        }
        if (matches) {
            candidates.push_back(candidate);
        }
    }
    if (candidates.empty()) {
        return;
    }
    // Offsets are transported in a free flat varying, one per candidate needing it
    std::array<std::optional<size_t>, 32> offset_slot{};
    size_t next_free = 0;
    const auto find_free_slot = [&]() -> std::optional<size_t> {
        for (; next_free < 32; ++next_free) {
            if (!vs_info.stores.Generic(next_free) && !fs_info.loads.Generic(next_free)) {
                return next_free++;
            }
        }
        return std::nullopt;
    };
    std::erase_if(candidates, [&](const Candidate& candidate) {
        const bool needs_offset = std::ranges::any_of(
            candidate.offsets, [](const auto& v) { return v && !v->IsEmpty(); });
        if (!needs_offset) {
            return false;
        }
        const std::optional<size_t> slot{find_free_slot()};
        if (!slot) {
            return true;
        }
        offset_slot[candidate.index] = *slot;
        // Store position - attribute in the free slot, right before the last of the stores
        for (const size_t component : RECONSTRUCTED_COMPONENTS) {
            const Store& attribute{*unique_store(GenericAttribute(candidate.index, component))};
            const Store& position{*unique_store(PositionAttribute(component))};
            IR::Inst* const last{[&] {
                for (IR::Inst& inst : attribute.block->Instructions()) {
                    if (&inst == attribute.inst) {
                        return position.inst;
                    }
                    if (&inst == position.inst) {
                        return attribute.inst;
                    }
                }
                return position.inst;
            }()};
            IR::IREmitter ir{*attribute.block, IR::Block::InstructionList::s_iterator_to(*last)};
            IR::F32 offset{ir.Imm32(0.0f)};
            const IR::Value expression{*candidate.offsets[component]};
            if (!expression.IsEmpty()) {
                const IR::Inst* source = expression.TryInstRecursive();
                if (source && source == position.value.TryInstRecursive() &&
                    source->GetOpcode() == IR::Opcode::FPFma32) {
                    offset = IR::F32{ir.FPMul(IR::F32{source->Arg(0)}, IR::F32{source->Arg(1)})};
                } else {
                    offset = IR::F32{expression};
                }
            }
            ir.SetAttribute(GenericAttribute(*slot, component), offset, ir.Imm32(0));
            vs_info.stores.Set(GenericAttribute(*slot, component));
            fs_info.loads.Set(GenericAttribute(*slot, component));
        }
        fs_info.interpolation[*slot] = Interpolation::Flat;
        return false;
    });
    if (candidates.empty()) {
        return;
    }
    // Rebuild the fragment shader reads from gl_FragCoord and the host viewport
    std::array<std::array<bool, 4>, 32> rebuild{};
    for (const Candidate& candidate : candidates) {
        for (const size_t component : RECONSTRUCTED_COMPONENTS) {
            rebuild[candidate.index][component] = true;
        }
    }
    std::vector<std::pair<IR::Block*, IR::Inst*>> reads;
    for (IR::Block* const block : fragment.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::GetAttribute) {
                continue;
            }
            const IR::Attribute attribute{inst.Arg(0).Attribute()};
            if (!IR::IsGeneric(attribute)) {
                continue;
            }
            const size_t index{IR::GenericAttributeIndex(attribute)};
            const size_t component{IR::GenericAttributeElement(attribute)};
            if (rebuild[index][component]) {
                reads.emplace_back(block, &inst);
            }
        }
    }
    for (const auto& [block, inst] : reads) {
        const IR::Attribute attribute{inst->Arg(0).Attribute()};
        const size_t index{IR::GenericAttributeIndex(attribute)};
        const size_t component{IR::GenericAttributeElement(attribute)};
        IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(*inst)};
        const IR::F32 w{ir.FPRecip(ir.GetAttribute(IR::Attribute::PositionW))};
        IR::F32 value{w};
        if (component != 3) {
            // ndc = (frag_coord - viewport.offset) / viewport.extent * 2 - 1
            const IR::F32 frag_coord{ir.GetAttribute(PositionAttribute(component))};
            const IR::F32 origin{ir.HostViewport(component)};
            const IR::F32 extent{ir.HostViewport(component + 2)};
            const IR::F32 relative{
                ir.FPMul(ir.FPAdd(frag_coord, ir.FPNeg(origin)), ir.FPRecip(extent))};
            const IR::F32 ndc{ir.FPFma(relative, ir.Imm32(2.0f), ir.Imm32(-1.0f))};
            value = IR::F32{ir.FPMul(ndc, w)};
        }
        if (const std::optional<size_t> slot{offset_slot[index]}) {
            const IR::F32 offset{ir.GetAttribute(GenericAttribute(*slot, component))};
            value = IR::F32{ir.FPAdd(value, ir.FPNeg(offset))};
        }
        inst->ReplaceUsesWith(value);
    }
    fs_info.loads.Set(IR::Attribute::PositionX);
    fs_info.loads.Set(IR::Attribute::PositionY);
    fs_info.loads.Set(IR::Attribute::PositionW);
    fs_info.uses_host_viewport = true;
    LOG_DEBUG(Shader, "Rebuilding {} position varyings from the fragment coordinate",
              candidates.size());
}

} // namespace Shader::Optimization
