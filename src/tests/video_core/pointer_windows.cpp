// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/profile.h"
#include "video_core/pointer_windows.h"

namespace {
using namespace Shader;

class VertexEnvironment final : public Environment {
public:
    VertexEnvironment() {
        stage = Stage::VertexB;
    }
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    TextureType ReadTextureType(u32) override { return TextureType::Color2D; }
    TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

// Nintendo Switch Sports' culling vertex shader: the DrawIndexedIndirect arguments are reset
// through a pointer loaded from the table described at c0[0x110]
struct Fixture {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    IR::Inst* reset{};

    explicit Fixture(bool support_pointer_windows) {
        IR::IREmitter ir{block};
        const auto word = [&](u32 offset) { return ir.GetCbuf(ir.Imm32(0), ir.Imm32(offset)); };
        const auto table{ir.PackUint2x32(ir.CompositeConstruct(word(0x110), word(0x114)))};
        const auto pointer{ir.LoadGlobal64(table)};
        const auto arguments{ir.PackUint2x32(ir.CompositeConstruct(
            ir.CompositeExtract(pointer, 0), ir.CompositeExtract(pointer, 1)))};
        ir.WriteGlobal32(arguments, ir.Imm32(0));
        reset = &block.Instructions().back();

        program.stage = Stage::VertexB;
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                               {{}, IR::AbstractSyntaxNode::Type::Return}};
        HostTranslateInfo host{};
        host.min_ssbo_alignment = 16;
        host.support_int64 = true;
        host.support_pointer_windows = support_pointer_windows;
        VertexEnvironment env;
        Optimization::GlobalMemoryToStorageBufferPass(env, program, host);
        Optimization::CollectShaderInfoPass(env, program);
    }
};

bool HasMemberOffset(const std::vector<u32>& code, u32 offset) {
    static constexpr u32 op_member_decorate{72};
    static constexpr u32 decoration_offset{35};
    for (size_t i = 5; i < code.size(); i += code[i] >> 16) {
        if ((code[i] & 0xffff) == op_member_decorate && code[i + 3] == decoration_offset &&
            code[i + 4] == offset) {
            return true;
        }
    }
    return false;
}
} // namespace

TEST_CASE("A store through a pointer loaded from a storage buffer records its pointer table",
          "[shader][pointer_windows]") {
    Fixture fixture{true};
    const auto& info{fixture.program.info};
    REQUIRE(fixture.reset->GetOpcode() == IR::Opcode::WriteGlobal32);
    REQUIRE(info.uses_pointer_windows);
    REQUIRE(info.pointer_table_descriptors.size() == 1);
    CHECK(info.pointer_table_descriptors[0].cbuf_index == 0);
    CHECK(info.pointer_table_descriptors[0].cbuf_offset == 0x110);
    // The table itself is still read as a storage buffer
    REQUIRE(info.storage_buffers_descriptors.size() == 1);
    CHECK(info.storage_buffers_descriptors[0].cbuf_offset == 0x110);
    CHECK_FALSE(info.storage_buffers_descriptors[0].is_pointer_window);
}

TEST_CASE("Without host pointer windows the store records no pointer table",
          "[shader][pointer_windows]") {
    Fixture fixture{false};
    CHECK(fixture.reset->GetOpcode() == IR::Opcode::WriteGlobal32);
    CHECK_FALSE(fixture.program.info.uses_pointer_windows);
    CHECK(fixture.program.info.pointer_table_descriptors.empty());
}

TEST_CASE("The global memory functions read the pointer windows from the push constants",
          "[shader][pointer_windows]") {
    Fixture fixture{true};
    auto& info{fixture.program.info};
    for (u32 window = 0; window < NUM_POINTER_WINDOWS; ++window) {
        info.storage_buffers_descriptors.push_back({
            .cbuf_index = 0,
            .cbuf_offset = window,
            .count = 1,
            .is_written = true,
            .is_pointer_window = true,
        });
    }
    Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.support_int64 = true;
    profile.support_descriptor_aliasing = true;
    profile.unified_descriptor_binding = true;
    profile.min_ssbo_alignment = 16;
    const auto code{Backend::SPIRV::EmitSPIRV(profile, fixture.program)};
    CHECK(HasMemberOffset(code, Backend::SPIRV::POINTER_WINDOWS_LAYOUT_OFFSET));
}

TEST_CASE("Pointers are grouped into device-contiguous windows", "[video_core][pointer_windows]") {
    using VideoCommon::GroupPointerWindows;
    using VideoCommon::PointerWindows;
    constexpr u64 access{VideoCommon::POINTER_WINDOW_ACCESS_SIZE};
    const auto always = [](GPUVAddr, GPUVAddr) { return true; };
    PointerWindows windows;

    SECTION("Close pointers share one window") {
        const std::vector<GPUVAddr> pointers{0x100000, 0x100040, 0x108000};
        REQUIRE(GroupPointerWindows(pointers, always, windows));
        CHECK(windows[0].gpu_addr == 0x100000);
        CHECK(windows[0].size == 0x8000 + access);
        CHECK(windows[1].size == 0);
    }

    SECTION("Larger gaps are bridged only when the pointers do not fit otherwise") {
        const std::vector<GPUVAddr> pointers{0x1000000, 0x1100000, 0x1200000, 0x1300000,
                                             0x1400000};
        REQUIRE(GroupPointerWindows(pointers, always, windows));
        CHECK(windows[0].gpu_addr == 0x1000000);
        CHECK(windows[0].size == 0x300000 + access);
        CHECK(windows[1].gpu_addr == 0x1400000);
        CHECK(windows[1].size == access);
        CHECK(windows[2].size == 0);
    }

    SECTION("A window does not span a discontiguous range") {
        // Device memory changes at the page boundary 0x101000
        const std::vector<GPUVAddr> pointers{0x100f00, 0x101000};
        const auto split = [](GPUVAddr begin, GPUVAddr end) {
            return end <= 0x101000 || begin >= 0x101000;
        };
        REQUIRE(GroupPointerWindows(pointers, split, windows));
        CHECK(windows[0].gpu_addr == 0x100f00);
        CHECK(windows[0].size == access);
        CHECK(windows[1].gpu_addr == 0x101000);
        CHECK(windows[1].size == access);
    }

    SECTION("Pointers further apart than the largest window do not fit") {
        std::vector<GPUVAddr> pointers;
        for (u64 index = 0; index <= Shader::NUM_POINTER_WINDOWS; ++index) {
            pointers.push_back(0x10000000 + index * 0x1000000);
        }
        CHECK_FALSE(GroupPointerWindows(pointers, always, windows));
    }
}
