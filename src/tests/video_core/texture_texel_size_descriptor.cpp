// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
namespace IR = Shader::IR;

// Handles 0, 4, 8 and 12 at constant buffer 0, offsets 0, 4, 8 and 12; 12 is a rect texture.
class TexelEnvironment final : public Shader::Environment {
public:
    u64 ReadInstruction(u32) override { throw std::logic_error("Unexpected instruction read"); }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        REQUIRE(index == 0);
        return offset;
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        return handle == 12 ? Shader::TextureType::Color2DRect : Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

const Shader::TextureDescriptor& DescriptorAt(const Shader::Info& info, u32 offset) {
    for (const Shader::TextureDescriptor& desc : info.texture_descriptors) {
        if (desc.cbuf_index == 0 && desc.cbuf_offset == offset) {
            return desc;
        }
    }
    FAIL("No texture descriptor at offset " << offset);
    throw std::logic_error("unreachable");
}
} // namespace

TEST_CASE("Texture descriptors note texel-unit reads", "[shader][texel_size]") {
    Shader::ObjectPool<IR::Inst> pool{256};
    IR::Block block{pool};
    IR::Program program;
    program.stage = Shader::Stage::Fragment;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    TexelEnvironment env;
    IR::IREmitter ir{block};
    IR::TextureInstInfo flags{};
    flags.type.Assign(Shader::TextureType::Color2D);
    const auto coords{ir.CompositeConstruct(ir.Imm32(0.5f), ir.Imm32(0.5f))};
    const auto texel{ir.CompositeConstruct(ir.Imm32(3U), ir.Imm32(5U))};
    // Offset 0: normalized sampling only.
    static_cast<void>(ir.ImageSampleImplicitLod(ir.GetCbuf(ir.Imm32(0), ir.Imm32(0)), coords,
                                               ir.Imm32(0.0f), {}, {}, flags));
    // Offset 4: sampled, then fetched (the flags of one descriptor merge).
    static_cast<void>(ir.ImageSampleImplicitLod(ir.GetCbuf(ir.Imm32(0), ir.Imm32(4)), coords,
                                               ir.Imm32(0.0f), {}, {}, flags));
    static_cast<void>(ir.ImageFetch(ir.GetCbuf(ir.Imm32(0), ir.Imm32(4)), texel, {},
                                    ir.Imm32(0U), {}, flags));
    // Offset 8: size query, as the frontend emits it (TXQ: bindless, typed by the pass).
    static_cast<void>(ir.ImageQueryDimension(ir.GetCbuf(ir.Imm32(0), ir.Imm32(8)), ir.Imm32(0U),
                                             ir.Imm1(false)));
    // Offset 12: a rect texture, sampled with unnormalized coordinates.
    static_cast<void>(ir.ImageSampleImplicitLod(ir.GetCbuf(ir.Imm32(0), ir.Imm32(12)), coords,
                                               ir.Imm32(0.0f), {}, {}, flags));

    Shader::HostTranslateInfo host;
    host.ApplyDescriptorLimitPolicy();
    Shader::Optimization::TexturePass(env, program, host);

    CHECK_FALSE(DescriptorAt(program.info, 0).uses_texel_size);
    CHECK(DescriptorAt(program.info, 4).uses_texel_size);
    CHECK(DescriptorAt(program.info, 8).uses_texel_size);
    CHECK(DescriptorAt(program.info, 12).uses_texel_size);
}
