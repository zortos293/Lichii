// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/geometry_exit.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

// Instruction words captured from The Duskbloods network test's static shader bundles.
namespace {
using Shader::Ampere::AnalyzeGeometryExit;
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};
constexpr Instruction Nop{{0x00007918, 0, 0, 0x000fc000}};

// The exit handler setup shared by the native geometry programs, handler at 0xd0.
constexpr std::array<Instruction, 5> Setup{{
    {{0x0000734e, 0, 0, 0x000fe400}},
    {{0x00037802, 0xd0, 0x00000f00, 0x000fc800}},
    {{0x00007210, 3, 0x07f1e0ff, 0x000fc800}},
    {{0x01017210, 0xff, 0x007fe4ff, 0x000fc800}},
    {{0x00007357, 0, 0, 0x0001e400}},
}};
constexpr std::array<Instruction, 3> Handler{{
    {{0x00ff7324, 0xff, 0, 0x0001e200}},
    {{0x00007957, 0, 0, 0x000fe200}},
    {{0x0000794d, 0, 0x03800000, 0x001fea00}},
}};

std::array<Instruction, 16> GeometryProgram(const std::array<Instruction, 8>& body) {
    std::array<Instruction, 16> code{};
    std::copy(Setup.begin(), Setup.end(), code.begin());
    std::copy(body.begin(), body.end(), code.begin() + 5);
    std::copy(Handler.begin(), Handler.end(), code.begin() + 13);
    return code;
}

TEST_CASE("Ampere geometry exit models Duskbloods billboard bodies", "[video_core][ampere]") {
    // GS_GXFfxtessellateBillboard*: CS2R.64 R0, TLD.LZ R0/R2 from (R0, R1), S2R R3, PRMT, ISETP P0.
    const auto billboard = GeometryProgram({{
        {{0x00007805, 0, 0x0001ff00, 0x001fcc00}},
        {{0x00007b66, 0x31c01001, 0x009e0d02, 0x000f4200}},
        {{0x00037919, 0, 0x00001d00, 0x000e2400}},
        {{0x03047816, 0x7772, 0x000000ff, 0x041fe400}},
        {{0x0400720c, 0xff, 0x03f03070, 0x000fc600}},
        Nop,
        Nop,
        {{0x0000794d, 0, 0x03800000, 0x000fea00}},
    }});
    const auto envelope = AnalyzeGeometryExit(billboard);
    INFO(envelope.error << " at " << envelope.error_offset);
    REQUIRE(static_cast<bool>(envelope));
    REQUIRE(envelope.handler == 0xd0);

    // GS_VFX_StandardParticle*: ULDC, UIADD3, S2R R0, PRMT R1, ISETP P0, ALD R2-R3.
    const auto particle = GeometryProgram({{
        {{0x00047ab9, 0x01000c00, 0x00000a00, 0x000fe400}},
        {{0x04047890, 0x00000050, 0x0ff1e03f, 0x000fe400}},
        {{0x00007919, 0, 0x00001d00, 0x001e2200}},
        {{0x00017816, 0x7772, 0x000000ff, 0x001fc400}},
        {{0x0100720c, 0xff, 0x03f03070, 0x000fc600}},
        {{0xff027321, 0x0000b001, 0x00000400, 0x000e2400}},
        Nop,
        {{0x0000794d, 0, 0x03800000, 0x000fea00}},
    }});
    const auto particle_envelope = AnalyzeGeometryExit(particle);
    INFO(particle_envelope.error << " at " << particle_envelope.error_offset);
    REQUIRE(static_cast<bool>(particle_envelope));
}

TEST_CASE("Ampere geometry exit still rejects reads of the setup state", "[video_core][ampere]") {
    // The TLD reads R3 as its y coordinate before anything writes it.
    const auto code = GeometryProgram({{
        {{0x00007805, 0, 0x0001ff00, 0x001fcc00}},
        {{0x00007b66, 0x31c01003, 0x009e0d02, 0x000f4200}},
        {{0x00037919, 0, 0x00001d00, 0x000e2400}},
        {{0x0400720c, 0xff, 0x03f03070, 0x000fc600}},
        Nop,
        Nop,
        Nop,
        {{0x0000794d, 0, 0x03800000, 0x000fea00}},
    }});
    const auto envelope = AnalyzeGeometryExit(code);
    REQUIRE(envelope.error == "native body reads exit setup state");
    REQUIRE(envelope.error_offset == 6 * 16);
}

TEST_CASE("Ampere geometry exit reads uniform guards as uniform predicates", "[video_core][ampere]") {
    // @UP0 UIADD3 before the ISETP that writes P0: UP0 is not the setup state's P0.
    const auto uniform = GeometryProgram({{
        {{0x00047ab9, 0x01000c00, 0x00000a00, 0x000fe400}},
        {{0x04040890, 0x00000050, 0x0ff1e03f, 0x000fe400}},
        {{0x00007919, 0, 0x00001d00, 0x001e2200}},
        {{0x00017816, 0x7772, 0x000000ff, 0x001fc400}},
        {{0x0100720c, 0xff, 0x03f03070, 0x000fc600}},
        {{0xff027321, 0x0000b001, 0x00000400, 0x000e2400}},
        Nop,
        {{0x0000794d, 0, 0x03800000, 0x000fea00}},
    }});
    const auto uniform_envelope = AnalyzeGeometryExit(uniform);
    INFO(uniform_envelope.error << " at " << uniform_envelope.error_offset);
    REQUIRE(static_cast<bool>(uniform_envelope));

    // @P0 PRMT there still reads the setup state's P0.
    auto ordinary = uniform;
    ordinary[5 + 1] = {{0x04047890, 0x00000050, 0x0ff1e03f, 0x000fe400}};
    ordinary[5 + 3] = {{0x00010816, 0x7772, 0x000000ff, 0x001fc400}};
    const auto ordinary_envelope = AnalyzeGeometryExit(ordinary);
    REQUIRE(ordinary_envelope.error == "native body reads exit setup state");
    REQUIRE(ordinary_envelope.error_offset == 8 * 16);
}

TEST_CASE("Ampere geometry exit reads a multisample fetch's coordinates and sample only",
          "[video_core][ampere]") {
    // CS2R.64 R0; TLD.MS.LZ 2D from (R4, R5), sample R2; S2R R3; ISETP P0. The fetch reads R2 and
    // not R3 (still setup state), which a coordinate group from Rb would include.
    const auto code = GeometryProgram({{
        {{0x00007805, 0, 0x0001ff00, 0x001fcc00}},
        {{0x04007b66, 0x31c01002, 0x009e4d02, 0x000f4200}},
        {{0x00037919, 0, 0x00001d00, 0x000e2400}},
        {{0x0400720c, 0xff, 0x03f03070, 0x000fc600}},
        Nop,
        Nop,
        Nop,
        {{0x0000794d, 0, 0x03800000, 0x000fea00}},
    }});
    const auto envelope = AnalyzeGeometryExit(code);
    INFO(envelope.error << " at " << envelope.error_offset);
    REQUIRE(static_cast<bool>(envelope));
}

TEST_CASE("Ampere IMAD.WIDE with a uniform addend shadows its high registers",
          "[video_core][ampere]") {
    // IMAD.WIDE.U32 R204, R10, R11, UR12 with registers from R200 up shadowed.
    const Instruction wide{{0x0acc7e25, 0x0000000c, 0x0f8e000b, 0x000fca00}};
    const auto pressure = Shader::Ampere::PrepareRegisterPressure(wide, 200);
    REQUIRE(pressure.valid);
    // The destination pair moves to a borrowed low quartet; Ra, Rb and the uniform pair stay.
    REQUIRE(pressure.instruction.Bits(16, 8) < 200);
    REQUIRE(pressure.instruction.Bits(16, 8) % 4 == 0);
    REQUIRE(pressure.instruction.Bits(24, 8) == 10);
    REQUIRE(pressure.instruction.Bits(64, 8) == 11);
    REQUIRE(pressure.instruction.Bits(32, 6) == 12);
    REQUIRE(pressure.prefix.size() == 8);
    REQUIRE(pressure.suffix.size() == 8);
}

TEST_CASE("Ampere SEL and FSEL with a uniform source shadow their high registers",
          "[video_core][ampere]") {
    for (const u32 opcode : {0xc07U, 0xc08U}) {
        // SEL / FSEL R204, R205, UR10, !P3 with registers from R200 up shadowed.
        const Instruction select{{0xcdcc7000U | opcode, 0x0000000a, 0x0d800000, 0x000fe400}};
        const auto pressure = Shader::Ampere::PrepareRegisterPressure(select, 200);
        REQUIRE(pressure.valid);
        // Rd and Ra move to one borrowed low quartet; the uniform source stays.
        const u32 borrowed = pressure.instruction.Bits(16, 8);
        REQUIRE(borrowed < 200);
        REQUIRE(borrowed % 4 == 0);
        REQUIRE(pressure.instruction.Bits(24, 8) == borrowed + 1);
        REQUIRE(pressure.instruction.Bits(32, 6) == 10);
        REQUIRE(pressure.prefix.size() == 8);
        REQUIRE(pressure.suffix.size() == 8);
    }
    // A program using the uniform bank lowers FSEL R220, R221, UR10, !P3 through the shadow.
    const auto lowered = LowerToMaxwell(
        std::array{Instruction{{0xdddc7c08, 0x0000000a, 0x0d800000, 0x000fe400}}, Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    const auto select = std::find_if(lowered.instructions.begin(), lowered.instructions.end(),
                                     [](u64 word) { return (word >> 48) == 0x5ca0; });
    REQUIRE(select != lowered.instructions.end());
    REQUIRE((*select & 0xff) < 216);
    REQUIRE(((*select >> 8) & 0xff) == (*select & 0xff) + 1);
    REQUIRE(((*select >> 20) & 0xff) == 226); // UR10's mirror
}

TEST_CASE("Ampere wide destinations that reach the high uniforms' registers are shadowed",
          "[video_core][ampere]") {
    const auto copies = [](const Shader::Ampere::LoweredProgram& lowered) {
        return std::count_if(lowered.instructions.begin(), lowered.instructions.end(),
                             [](u64 word) { return (word >> 48) == 0x5c98; });
    };
    // With the uniform bank (FSEL R4, R5, UR10, !P3) and two high uniforms, UR28 lives at R214:
    // LDS.128 R212, [R0] writes it. Shadowing works in groups of four, so lowering asks for four.
    const std::array code{Instruction{{0x05047c08, 0x0000000a, 0x0d800000, 0x000fe400}},
                          Instruction{{0x00d47984, 0, 0xc00, 0}}, Exit};
    const auto unaligned = Shader::Ampere::LowerToMaxwellWith(code, 2);
    REQUIRE(!unaligned);
    REQUIRE(unaligned.high_uniform_registers == 4);
    // Then R212-R215 hold UR28-UR31, and the load goes through a borrowed low quartet.
    const auto lowered = Shader::Ampere::LowerToMaxwellWith(code, 4);
    INFO(lowered.error);
    REQUIRE(lowered);
    REQUIRE(copies(lowered) == 16);
    // Without high uniforms R212-R215 are below the uniform bank's registers: no shadow.
    const auto below = Shader::Ampere::LowerToMaxwellWith(code, 0);
    INFO(below.error);
    REQUIRE(below);
    REQUIRE(copies(below) == 0);
}

TEST_CASE("Ampere task programs read back their payload", "[video_core][ampere]") {
    // GXFlver_Common_MeshletAS: ISBERD.O.ATTR.32 R0, [RZ + 0x14].
    const auto lowered = LowerToMaxwell(
        std::array{Instruction{{0xff007923, 0x00001400, 0x0000b800, 0x001e2800}}, Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    const auto read = std::find_if(lowered.instructions.begin(), lowered.instructions.end(),
                                   [](u64 word) { return (word >> 52) == 0xefc; });
    REQUIRE(read != lowered.instructions.end());
    REQUIRE((*read & 0xff) == 0);
    REQUIRE(((*read >> 8) & 0xff) == 255);
    REQUIRE(((*read >> 20) & 0xffffff) == 0x14);
    REQUIRE(((*read >> 44) & 0x1f) == 0b01011); // .32, .ATTR, .O, read

    // The U8 form stays rejected.
    const auto byte = LowerToMaxwell(
        std::array{Instruction{{0xff007923, 0x00001400, 0x00003000 | 0x8000, 0x001e2800}}, Exit});
    REQUIRE(!byte);
}

TEST_CASE("Ampere bindless gathers take AOFFI offsets", "[video_core][ampere]") {
    // FXAA3HQ: TLD4.R with the handle in UR4, x, y in R2, R3 and the offsets in R9.
    const auto lowered = LowerToMaxwell(
        std::array{Instruction{{0x02087f63, 0x30000409, 0x081e15ff, 0x000f4200}}, Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    const auto gather = std::find_if(lowered.instructions.begin(), lowered.instructions.end(),
                                     [](u64 word) { return (word >> 54) == (0xdec0ULL >> 6); });
    REQUIRE(gather != lowered.instructions.end());
    REQUIRE(((*gather >> 36) & 3) == 1);
    REQUIRE(((*gather >> 20) & 0xff) == 246);
    const bool offsets_moved = std::any_of(lowered.instructions.begin(), gather, [](u64 word) {
        return (word >> 48) == 0x5c98 && (word & 0xff) == 247 && ((word >> 20) & 0xff) == 9;
    });
    REQUIRE(offsets_moved);
}

TEST_CASE("Ampere FSEL takes a uniform source", "[video_core][ampere]") {
    // MotionBlur reconstruction: FSEL R8, R4, UR10, !P3.
    const auto lowered = LowerToMaxwell(
        std::array{Instruction{{0x04088c08, 0x0000000a, 0x0d800000, 0x000fe400}}, Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    const auto select = std::find_if(lowered.instructions.begin(), lowered.instructions.end(),
                                     [](u64 word) { return (word >> 48) == 0x5ca0; });
    REQUIRE(select != lowered.instructions.end());
    REQUIRE((*select & 0xff) == 8);
    REQUIRE(((*select >> 8) & 0xff) == 4);
    REQUIRE(((*select >> 39) & 7) == 3);
    REQUIRE(((*select >> 42) & 1) == 1);
}

TEST_CASE("Ampere IMAD.WIDE adds a uniform pair", "[video_core][ampere]") {
    // M[AMSN_V][Grass4]_Animation: IMAD.WIDE.U32 R10, R10, R11, UR12.
    const auto lowered = LowerToMaxwell(
        std::array{Instruction{{0x0a0a7e25, 0x0000000c, 0x0f8e000b, 0x000fca00}}, Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    // The low word adds UR12's mirror (R228) with a carry out, the high word UR13's (R229) with it.
    const auto adds = [&](u32 target, u32 source) {
        return std::any_of(lowered.instructions.begin(), lowered.instructions.end(), [&](u64 word) {
            return (word >> 48) == 0x5c10 && (word & 0xff) == target &&
                   ((word >> 20) & 0xff) == source;
        });
    };
    REQUIRE(adds(250, 228));
    REQUIRE(adds(11, 229));

    // An odd uniform pair is rejected.
    const auto odd = LowerToMaxwell(
        std::array{Instruction{{0x0a0a7e25, 0x0000000d, 0x0f8e000b, 0x000fca00}}, Exit});
    REQUIRE(!odd);
}

TEST_CASE("Ampere multisample texel fetches lower to TLD.MS", "[video_core][ampere]") {
    // GXSimple_ResolveColor_F: TLD.LZ.MS R0-R1/R2-R3 from (R4, R5), sample R6, handle 0, buffer 8.
    const auto lowered = LowerToMaxwell(
        std::array{Instruction{{0x04007b66, 0x32000006, 0x049e4f02, 0x001fc200}}, Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    REQUIRE(lowered.required_texture_buffer == 8);
    const auto fetch = std::find_if(lowered.instructions.begin(), lowered.instructions.end(),
                                    [](u64 word) { return (word >> 56) == 0xdc; });
    REQUIRE(fetch != lowered.instructions.end());
    REQUIRE(((*fetch >> 50) & 1) == 1);  // .MS
    REQUIRE(((*fetch >> 55) & 1) == 0);  // .LZ
    REQUIRE(((*fetch >> 28) & 7) == 2);  // 2D
    REQUIRE(((*fetch >> 31) & 15) == 15);
    const auto copies = [&](u32 target, u32 source) {
        return std::any_of(lowered.instructions.begin(), lowered.instructions.end(), [&](u64 word) {
            return (word >> 48) == 0x5c98 && (word & 0xff) == target &&
                   ((word >> 20) & 0xff) == source;
        });
    };
    REQUIRE(copies(250, 4));
    REQUIRE(copies(251, 5));
    REQUIRE(copies(252, 6));
    for (u32 i = 0; i < 4; ++i) {
        REQUIRE(copies(i, 246 + i));
    }

    // Other multisample forms stay rejected (here .LL).
    const auto level = LowerToMaxwell(
        std::array{Instruction{{0x04007b66, 0x32000006, 0x049e4f02 | (2U << 23), 0x001fc200}}, Exit});
    REQUIRE(!level);
}

TEST_CASE("Ampere TXQ reads a second descriptor buffer bindless", "[video_core][ampere]") {
    // GXDeferred_Debug: a TLD on buffer 8, then TXQ R0-R1 of handle 0x32 on buffer 10.
    const auto lowered = LowerToMaxwell(std::array{
        Instruction{{0x0a027b66, 0x32000003, 0x049e070a, 0x000f4200}},
        Instruction{{0x00007b6f, 0x02803200, 0x000003ff, 0x001f4200}}, Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    REQUIRE(lowered.required_texture_buffer == 8);
    const auto query = std::find_if(lowered.instructions.begin(), lowered.instructions.end(),
                                    [](u64 word) { return (word >> 51) == (0xdf50ULL >> 3); });
    REQUIRE(query != lowered.instructions.end());
    REQUIRE(((*query >> 8) & 0xff) == 252);
    const bool handle_loaded =
        std::any_of(lowered.instructions.begin(), query, [](u64 word) {
            return (word >> 48) == 0x4c98 && (word & 0xff) == 252 && ((word >> 34) & 31) == 10 &&
                   ((word >> 20) & 0x3fff) == 0x32;
        });
    REQUIRE(handle_loaded);
}

} // namespace
