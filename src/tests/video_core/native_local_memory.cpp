// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <map>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <spirv/unified1/GLSL.std.450.h>
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
namespace IR = Shader::IR;

class LocalEnvironment final : public Shader::Environment {
public:
    LocalEnvironment(bool native_, Shader::Stage stage_, u32 size_)
        : native{native_}, size{size_} {
        stage = stage_;
    }
    bool IsNativeProgram() const override { return native; }
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { return {}; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return size; }
    u32 SharedMemorySize() const override { return 256; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    bool native;
    u32 size;
};

u64 MemoryInstruction(u32 address_register, u32 offset, u32 size = 4) {
    return u64{4} | (u64{address_register} << 8) | (u64{offset & 0xffffff} << 20) |
           (u64{size} << 48);
}

u32 EvaluateAddress(IR::Value value, u32 register_value) {
    if (value.IsImmediate()) {
        return value.U32();
    }
    const auto& inst = *value.Inst();
    const auto arg = [&](size_t index) { return EvaluateAddress(inst.Arg(index), register_value); };
    switch (inst.GetOpcode()) {
    case IR::Opcode::Identity: return arg(0);
    case IR::Opcode::GetRegister: return register_value;
    case IR::Opcode::IAdd32: return arg(0) + arg(1);
    case IR::Opcode::BitwiseAnd32: return arg(0) & arg(1);
    case IR::Opcode::ShiftRightArithmetic32:
        return static_cast<u32>(static_cast<s32>(arg(0)) >> arg(1));
    default: throw std::logic_error("Unexpected local address operation");
    }
}

std::vector<u32> AccessOffsets(bool native, Shader::Stage stage, bool shared, bool store,
                               u32 address_register, u32 register_value, u32 offset,
                               u32 size = 4) {
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    LocalEnvironment env{native, stage, 0x180};
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    const u64 instruction = MemoryInstruction(address_register, offset, size);
    if (shared) {
        if (store) visitor.STS(instruction);
        else visitor.LDS(instruction);
    } else {
        if (store) visitor.STL(instruction);
        else visitor.LDL(instruction);
    }
    const auto wanted = shared ? (store ? IR::Opcode::WriteSharedU32 : IR::Opcode::LoadSharedU32)
                               : (store ? IR::Opcode::WriteLocal : IR::Opcode::LoadLocal);
    std::vector<u32> offsets;
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == wanted) {
            offsets.push_back(EvaluateAddress(inst.Arg(0), register_value));
        }
    }
    return offsets;
}

std::vector<std::vector<u32>> Instructions(const std::vector<u32>& code, spv::Op opcode) {
    std::vector<std::vector<u32>> result;
    for (size_t offset = 5; offset < code.size();) {
        const u32 count = code[offset] >> 16;
        REQUIRE(count > 0);
        REQUIRE(offset + count <= code.size());
        if ((code[offset] & 0xffff) == static_cast<u32>(opcode)) {
            result.emplace_back(code.begin() + offset + 1, code.begin() + offset + count);
        }
        offset += count;
    }
    return result;
}
} // namespace

TEST_CASE("Native compute local accesses address the top of the 24-bit space",
          "[video_core][shader][native_local_memory]") {
    for (const bool store : {false, true}) {
        CAPTURE(store);
        // A 0x180-byte allocation, with a stack pointer 0x80 bytes below the top.
        CHECK(AccessOffsets(true, Shader::Stage::Compute, false, store, 2, 0xffff80,
                            0xffff00) == std::vector<u32>{0});
        CHECK(AccessOffsets(true, Shader::Stage::Compute, false, store, 2, 0xffff80,
                            0xffff04) == std::vector<u32>{1});
        CHECK(AccessOffsets(true, Shader::Stage::Compute, false, store, 255, 0,
                            0xfffffc) == std::vector<u32>{0x5f});
        CHECK(AccessOffsets(true, Shader::Stage::Compute, false, store, 2, 0xffff80,
                            0xffff00, 6) == std::vector<u32>{0, 1, 2, 3});
    }
}

TEST_CASE("Local relocation leaves other stages and shared memory addresses unchanged",
          "[video_core][shader][native_local_memory]") {
    for (const bool store : {false, true}) {
        CAPTURE(store);
        CHECK(AccessOffsets(false, Shader::Stage::Compute, false, store, 255, 0, 0x40) ==
              std::vector<u32>{0x10});
        for (const auto stage : {Shader::Stage::VertexB, Shader::Stage::Fragment}) {
            CHECK(AccessOffsets(true, stage, false, store, 2, 0x80, 0xffffe0) ==
                  std::vector<u32>{0x18});
        }
        CHECK(AccessOffsets(true, Shader::Stage::Compute, true, store, 2, 0x80, 0xffffe0) ==
              std::vector<u32>{0x60});
        CHECK(AccessOffsets(true, Shader::Stage::Compute, true, store, 255, 0, 0x40) ==
              std::vector<u32>{0x40});
    }
}

TEST_CASE("SPIR-V local reads and writes cannot address beyond their private allocation",
          "[video_core][spirv][native_local_memory]") {
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    Shader::IR::Program program;
    program.stage = Shader::Stage::Compute;
    program.local_memory_size = 64;
    program.workgroup_size = {1, 1, 1};
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    for (const auto offset : {ir.Imm32(0), ir.Imm32(15), ir.Imm32(16), ir.Imm32(~0U),
                              ir.LocalInvocationIdX()}) {
        ir.WriteLocal(offset, ir.Imm32(0x12345678));
        ir.WriteLocal(ir.Imm32(0), ir.LoadLocal(offset));
    }
    LocalEnvironment env{true, Shader::Stage::Compute, 64};
    Shader::Optimization::CollectShaderInfoPass(env, program);
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    const auto code = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
    std::map<u32, u32> constants;
    for (const auto& words : Instructions(code, spv::Op::OpConstant)) {
        if (words.size() == 3) constants.emplace(words[1], words[2]);
    }
    const auto arrays = Instructions(code, spv::Op::OpTypeArray);
    REQUIRE(arrays.size() == 1);
    CHECK(constants.at(arrays.front()[2]) == 17);
    std::map<u32, u32> clamp_limits;
    for (const auto& words : Instructions(code, spv::Op::OpExtInst)) {
        if (words[3] == GLSLstd450UMin) {
            clamp_limits.emplace(words[1], constants.at(words[5]));
        }
    }
    u32 local_variable = 0;
    for (const auto& words : Instructions(code, spv::Op::OpVariable)) {
        if (words[2] == static_cast<u32>(spv::StorageClass::Private)) {
            REQUIRE(local_variable == 0);
            local_variable = words[1];
        }
    }
    REQUIRE(local_variable != 0);
    u32 checked_accesses = 0;
    for (const auto& words : Instructions(code, spv::Op::OpAccessChain)) {
        if (words[2] == local_variable) {
            REQUIRE(clamp_limits.contains(words[3]));
            CHECK(clamp_limits.at(words[3]) == 16);
            ++checked_accesses;
        }
    }
    CHECK(checked_accesses == 15);
}
