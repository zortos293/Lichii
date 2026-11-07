// SPDX-License-Identifier: GPL-3.0-or-later

#include <string_view>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/GLSL.std.450.h>
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"

namespace {
using namespace Shader;

struct GlslInstruction {
    u32 instruction;
    u32 result;
    std::vector<u32> operands;
};

/// Every GLSL.std.450 extended instruction of a module, in order.
std::vector<GlslInstruction> GlslInstructions(const std::vector<u32>& code) {
    u32 glsl_set{};
    std::vector<GlslInstruction> found;
    for (size_t index = 5; index < code.size();) {
        const u32 count = code[index] >> 16;
        REQUIRE(count != 0);
        const auto opcode = static_cast<spv::Op>(code[index] & 0xffff);
        if (opcode == spv::Op::OpExtInstImport) {
            const auto* const name = reinterpret_cast<const char*>(&code[index + 2]);
            if (std::string_view{name} == "GLSL.std.450") {
                glsl_set = code[index + 1];
            }
        } else if (opcode == spv::Op::OpExtInst && code[index + 3] == glsl_set) {
            found.push_back({code[index + 4], code[index + 2],
                             {code.begin() + index + 5, code.begin() + index + count}});
        }
        index += count;
    }
    return found;
}

template <typename Body>
std::vector<GlslInstruction> Emit(bool broken_clamp, Body&& body) {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    program.stage = Stage::Compute;
    program.workgroup_size = {1, 1, 1};
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    IR::IREmitter ir{block};
    body(ir);
    Profile profile;
    profile.supported_spirv = 0x00010500;
    profile.has_broken_spirv_clamp = broken_clamp;
    return GlslInstructions(Backend::SPIRV::EmitSPIRV(profile, program));
}

size_t Count(const std::vector<GlslInstruction>& instructions, u32 instruction) {
    size_t count{};
    for (const auto& found : instructions) {
        count += found.instruction == instruction ? 1 : 0;
    }
    return count;
}

void RequireNoUndefinedNaN(const std::vector<GlslInstruction>& instructions) {
    CHECK(Count(instructions, GLSLstd450FMin) == 0);
    CHECK(Count(instructions, GLSLstd450FMax) == 0);
    CHECK(Count(instructions, GLSLstd450FClamp) == 0);
}
} // namespace

TEST_CASE("Float min and max return the operand that is not NaN", "[video_core][spirv][nan]") {
    for (const bool broken_clamp : {false, true}) {
        const auto instructions = Emit(broken_clamp, [](IR::IREmitter& ir) {
            (void)ir.FPMin(ir.Imm32(1.0f), ir.Imm32(2.0f));
            (void)ir.FPMax(ir.Imm32(1.0f), ir.Imm32(2.0f));
        });
        RequireNoUndefinedNaN(instructions);
        CHECK(Count(instructions, GLSLstd450NMin) == 1);
        CHECK(Count(instructions, GLSLstd450NMax) == 1);
    }
}

TEST_CASE("Float clamp and saturate send NaN to the lower bound", "[video_core][spirv][nan]") {
    for (const bool broken_clamp : {false, true}) {
        const auto instructions = Emit(broken_clamp, [](IR::IREmitter& ir) {
            (void)ir.FPSaturate(ir.Imm32(0.5f));
            (void)ir.FPClamp(ir.Imm32(0.5f), ir.Imm32(0.25f), ir.Imm32(0.75f));
        });
        RequireNoUndefinedNaN(instructions);
        REQUIRE(Count(instructions, GLSLstd450NMax) == 2);
        REQUIRE(Count(instructions, GLSLstd450NMin) == 2);
        // NMin(NMax(x, lo), hi): NMax turns a NaN into lo first, as the guest's clamp does.
        for (size_t index = 0; index < instructions.size(); ++index) {
            if (instructions[index].instruction != GLSLstd450NMin) {
                continue;
            }
            REQUIRE(index > 0);
            const auto& lower = instructions[index - 1];
            CHECK(lower.instruction == GLSLstd450NMax);
            REQUIRE(instructions[index].operands.size() == 2);
            CHECK(instructions[index].operands[0] == lower.result);
        }
    }
}
