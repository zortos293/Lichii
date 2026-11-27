// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <optional>
#include <span>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};
constexpr Instruction Load{{0x10007998, 0x60000000, 0x001e0f1d, 0x010f6600}};
constexpr Instruction Store{{0x0200799c, 0x60000004, 0x00100f00, 0x004fe200}};

TEST_CASE("Ampere captured bindless formatted surfaces preserve operands", "[video_core][ampere]") {
    struct Case {
        Instruction instruction;
        u64 expected;
    };
    for (const auto& test : std::array{
             Case{Load, 0xeb000e8600f71000ULL},
             Case{Store, 0xeb20000600f70204ULL},
             Case{{{0x00037998, 0xa0000000, 0x001e0119, 0x010f6200}},
                  0xeb000c8a00170003ULL},
             Case{{{0x0000799c, 0xa0000004, 0x00100f03, 0x0101e200}},
                  0xeb20018a00f70004ULL},
             Case{{{0x0000799c, 0x8000000c, 0x00100f07, 0x0041e200}},
                  0xeb20038800f7000cULL},
             // Cyberpunk 2077 compute (#1084): .BUFFER loads and stores, one coordinate.
             Case{{{0x01017998, 0x20000000, 0x001e0100, 0x004f6200}},
                  0xeb00000200170101ULL},
             Case{{{0x0000799c, 0x20000008, 0x00100f23, 0x0101e400}},
                  0xeb20118200f70008ULL},
             Case{{{0x0100799c, 0x20000010, 0x00100f00, 0x0041e200}},
                  0xeb20000200f70110ULL},
         }) {
        const auto lowered = LowerToMaxwell(std::array{test.instruction, Exit});
        INFO(lowered.error);
        REQUIRE(lowered);
        REQUIRE(lowered.instructions.size() == 2);
        REQUIRE(lowered.instructions.front() == test.expected);
        REQUIRE(lowered.required_texture_buffer == ~0U);
    }
}

TEST_CASE("Ampere register-handle surface atomics and reductions", "[video_core][ampere]") {
    struct Case {
        Instruction instruction;
        u64 expected;
    };
    // Cyberpunk 2077 compute (#1084): SUATOM.D.BUFFER.ADD (0x394) and SURED.D.BUFFER.ADD
    // (0x3a0), the handle in R2 and R4; Maxwell SUATOM marks the handle with bit 54, SURED
    // clears its bound bit 51.
    for (const auto& test : std::array{
             Case{{{0x0b050394, 0x20000005, 0x001ee002, 0x010ee200}}, 0xea40010200500b05ULL},
             Case{{{0x0a0013a0, 0x20000009, 0x0010e004, 0x0041e200}}, 0xeb40020200010a09ULL},
         }) {
        const auto lowered = LowerToMaxwell(std::array{test.instruction, Exit});
        INFO(lowered.error);
        REQUIRE(lowered);
        REQUIRE(lowered.instructions.size() == 2);
        REQUIRE(lowered.instructions.front() == test.expected);
        REQUIRE(lowered.required_texture_buffer == ~0U);

        auto bound_field = test.instruction;
        bound_field.words[1] |= 1U << 22;
        REQUIRE_FALSE(LowerToMaxwell(std::array{bound_field, Exit}));
        auto scratch_handle = test.instruction;
        scratch_handle.words[2] = (scratch_handle.words[2] & ~0xffU) | 250U;
        REQUIRE_FALSE(LowerToMaxwell(std::array{scratch_handle, Exit}));
    }
}

TEST_CASE("Ampere bindless surfaces preserve predicates", "[video_core][ampere]") {
    for (const Instruction original : {Load, Store}) {
        auto guarded = original;
        guarded.words[0] = (guarded.words[0] & ~0xf000U) | 0x9000U;
        const auto plain = LowerToMaxwell(std::array{original, Exit});
        const auto lowered = LowerToMaxwell(std::array{guarded, Exit});
        REQUIRE(plain);
        REQUIRE(lowered);
        REQUIRE(lowered.instructions.front() ==
                ((plain.instructions.front() & ~(u64{0xf} << 16)) | (u64{9} << 16)));
    }
}

TEST_CASE("Ampere bindless surfaces reject unsupported fields", "[video_core][ampere]") {
    for (const Instruction original : {Load, Store}) {
        for (const auto& [word, bit] : std::array{
                 std::pair{1U, 8U}, std::pair{1U, 28U}, std::pair{2U, 12U},
                 std::pair{2U, 13U}, std::pair{2U, 20U}, std::pair{2U, 31U},
                 std::pair{3U, 0U}, std::pair{3U, 30U}}) {
            auto invalid = original;
            invalid.words[word] ^= 1U << bit;
            REQUIRE_FALSE(LowerToMaxwell(std::array{invalid, Exit}));
        }
        auto no_channels = original;
        no_channels.words[2] &= ~0xf00U;
        REQUIRE_FALSE(LowerToMaxwell(std::array{no_channels, Exit}));
        auto dimension = original;
        dimension.words[1] &= 0x1fffffffU;
        REQUIRE_FALSE(LowerToMaxwell(std::array{dimension, Exit}));
        auto handle = original;
        handle.words[2] = (handle.words[2] & ~0xffU) | 250U;
        REQUIRE_FALSE(LowerToMaxwell(std::array{handle, Exit}));
    }
    auto load_source = Load;
    load_source.words[1] |= 1;
    REQUIRE_FALSE(LowerToMaxwell(std::array{load_source, Exit}));
    auto unaligned_load = Load;
    unaligned_load.words[0] |= 1U << 16;
    REQUIRE_FALSE(LowerToMaxwell(std::array{unaligned_load, Exit}));
    auto store_destination = Store;
    store_destination.words[0] |= 1U << 16;
    REQUIRE_FALSE(LowerToMaxwell(std::array{store_destination, Exit}));
    auto store_mask = Store;
    store_mask.words[2] = (store_mask.words[2] & ~0xf00U) | 0x500U;
    REQUIRE_FALSE(LowerToMaxwell(std::array{store_mask, Exit}));
    auto store_scratch = Store;
    store_scratch.words[1] = (store_scratch.words[1] & ~0xffU) | 244U;
    REQUIRE_FALSE(LowerToMaxwell(std::array{store_scratch, Exit}));
    auto coordinate_scratch = Store;
    coordinate_scratch.words[0] = (coordinate_scratch.words[0] & 0xffffffU) | (245U << 24);
    REQUIRE_FALSE(LowerToMaxwell(std::array{coordinate_scratch, Exit}));
    const Instruction uniform{{0x00047882, 0, 0, 0x000fe200}};
    REQUIRE(LowerToMaxwell(std::array{uniform, Exit}));
    store_scratch.words[1] = (Store.words[1] & ~0xffU) | 214U;
    REQUIRE_FALSE(LowerToMaxwell(std::array{uniform, store_scratch, Exit}));
}

// A compute program of Maxwell words at 0x50, a scheduling word before every three
class SurfaceEnvironment final : public Shader::Environment {
public:
    std::span<const u64> code;

    SurfaceEnvironment() { stage = Shader::Stage::Compute; }

    u64 ReadInstruction(u32 address) override {
        for (u32 index = 0; index < code.size(); ++index) {
            if (address == 0x50 + index * 8 + ((index + 1) / 3) * 8) {
                return code[index];
            }
        }
        FAIL("Read outside the surface fixture at " << address);
        return 0;
    }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 2; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {32, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    bool HasNativeInstructions() const override { return true; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

// Whether the SPIR-V decorates one variable with descriptor set `set` and binding `binding`
bool DecoratesSetBinding(std::span<const u32> spirv, u32 set, u32 binding) {
    std::vector<u32> in_set, at_binding;
    for (size_t at = 5; at < spirv.size();) {
        const u32 count{spirv[at] >> 16};
        if (count == 0) {
            break;
        }
        if ((spirv[at] & 0xffff) == 71 /* OpDecorate */ && count >= 4) {
            if (spirv[at + 2] == 34 /* DescriptorSet */ && spirv[at + 3] == set) {
                in_set.push_back(spirv[at + 1]);
            }
            if (spirv[at + 2] == 33 /* Binding */ && spirv[at + 3] == binding) {
                at_binding.push_back(spirv[at + 1]);
            }
        }
        at += count;
    }
    for (const u32 id : in_set) {
        for (const u32 other : at_binding) {
            if (id == other) {
                return true;
            }
        }
    }
    return false;
}

// Duskbloods' compute shaders store through a handle no constant buffer word holds (the
// register-handle SUST.P above): with pool textures (NVN direct mode) the image is read from the
// image pool by the handle's texture ID, with no handle program to evaluate per dispatch.
TEST_CASE("Ampere register-handle surface stores read the image pool", "[video_core][ampere]") {
    const auto lowered = LowerToMaxwell(std::array{Store, Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    // S2R R0, SR_TID.X: the handle (R0) comes from no constant buffer
    std::vector<u64> code{0xf0c8000002170000ULL};
    code.insert(code.end(), lowered.instructions.begin(), lowered.instructions.end());
    SurfaceEnvironment env;
    env.code = code;
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, Shader::Maxwell::Location{0x50}};
    Shader::ObjectPool<Shader::IR::Inst> inst_pool;
    Shader::ObjectPool<Shader::IR::Block> block_pool;
    Shader::HostTranslateInfo host;
    host.support_pool_textures = true;
    host.ApplyDescriptorLimitPolicy();
    auto program{Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host)};
    REQUIRE(program.info.image_descriptors.size() == 1);
    const Shader::ImageDescriptor& image{program.info.image_descriptors.front()};
    CHECK(image.is_pool);
    CHECK(image.is_written);
    CHECK_FALSE(image.is_integer); // formatted (.P): float
    CHECK(image.handle_program == ~0U);
    CHECK(program.info.handle_programs.empty());

    Shader::Profile profile{};
    profile.supported_spirv = 0x00010500;
    profile.support_typeless_image_loads = true;
    Shader::RuntimeInfo runtime_info{};
    Shader::Backend::Bindings bindings{};
    bindings.pool_set = 1;
    bindings.pool_texture_binding = 0;
    bindings.pool_sampler_binding = 1;
    bindings.pool_image_binding = 2;
    const auto spirv{Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings)};
    REQUIRE(spirv.size() > 5);
    CHECK(DecoratesSetBinding(spirv, 1, 2));
    CHECK_FALSE(DecoratesSetBinding(spirv, 0, 0));
}
} // namespace
