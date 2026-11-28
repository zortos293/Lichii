// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <span>
#include <utility>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
using Shader::Ampere::ProgramCaptureState;
using Result = ProgramCaptureState::Result;

constexpr Instruction Exit{{0x794d, 0, 0x03800000, 0}};
constexpr Instruction Nop{{0x7918, 0, 0, 0}};
constexpr Instruction SelfBranch{{0x7947, 0xfffffff0, 0x0383ffff, 0}};
constexpr Instruction InvalidBranch{{0x7947, 0x18, 0x03800000, 0}};

std::pair<Result, u32> Capture(std::span<const Instruction> code) {
    ProgramCaptureState capture;
    for (u32 index = 0; index < code.size(); ++index) {
        const Result result = capture.Observe(code[index], index);
        if (result != Result::Continue) {
            return {result, index};
        }
    }
    return {Result::Continue, static_cast<u32>(code.size())};
}
} // namespace

TEST_CASE("Ampere capture closes after an out-of-line block following EXIT",
          "[video_core][ampere][capture]") {
    const std::array code{
        Instruction{{0x0947, 0x20, 0x03800000, 0}},
        Nop,
        Exit,
        Nop,
        Instruction{{0x7947, 0xffffffc0, 0x0383ffff, 0}},
        SelfBranch,
        Nop,
        InvalidBranch,
    };
    CHECK(Capture(code) == std::pair{Result::Complete, 5u});
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    CHECK(lowered.source_size == 6 * sizeof(Instruction));
}

TEST_CASE("Ampere capture bounds-checks BRA.DIV without following it",
          "[video_core][ampere][capture]") {
    const std::array code{
        Instruction{{0x7947, 0x22, 0x03800000, 0}},
        Nop,
        Exit,
        Nop,
        SelfBranch,
    };
    CHECK(Capture(code) == std::pair{Result::Complete, 2u});
    const auto lowered = LowerToMaxwell(std::span{code}.first(3));
    REQUIRE(static_cast<bool>(lowered));
    CHECK(lowered.source_size == 3 * sizeof(Instruction));

    const Instruction malformed{{0x7947, 0x2a, 0x03800000, 0}};
    CHECK(Capture(std::array{malformed, Exit}) == std::pair{Result::InvalidTarget, 0u});
    const Instruction outside{{0x7947, 0x02, 0x03810000, 0}};
    CHECK(Capture(std::array{outside, Exit}) == std::pair{Result::InvalidTarget, 0u});
    const Instruction before_start{{0x7947, 0xffffff00, 0x0383ffff, 0}};
    CHECK(Capture(std::array{before_start, Exit}) == std::pair{Result::InvalidTarget, 0u});
}

TEST_CASE("Ampere capture and lowering do not close on an outstanding direct target",
          "[video_core][ampere][capture]") {
    const Instruction branch_forward{{0x7947, 0x30, 0x03800000, 0}};
    const std::array branch_code{branch_forward, Exit, SelfBranch, Nop, Exit};
    CHECK(Capture(branch_code) == std::pair{Result::Complete, 4u});
    const auto branch_lowered = LowerToMaxwell(branch_code);
    REQUIRE(static_cast<bool>(branch_lowered));
    CHECK(branch_lowered.source_size == 5 * sizeof(Instruction));

    const Instruction link{{0x7802, 0x20, 0x00000f00, 0}};
    const Instruction call_forward{{0x7944, 0x20, 0x03c00000, 0}};
    const Instruction ret{{0x7950, 0xffffffb0, 0x03c3ffff, 0}};
    const std::array call_code{link, call_forward, Exit, SelfBranch, ret};
    CHECK(Capture(call_code) == std::pair{Result::Complete, 4u});
    const auto call_lowered = LowerToMaxwell(call_code);
    REQUIRE(static_cast<bool>(call_lowered));
    CHECK(call_lowered.source_size == sizeof(call_code));

    const Instruction skip_exit{{0x0947, 0x10, 0x03800000, 0}};
    const Instruction bssy{{0x7945, 0x20, 0x03800000, 0}};
    const std::array reconverge{skip_exit, Exit, bssy, SelfBranch, Nop, Exit};
    CHECK(Capture(reconverge) == std::pair{Result::Complete, 5u});
    const auto reconverge_lowered = LowerToMaxwell(reconverge);
    REQUIRE(static_cast<bool>(reconverge_lowered));
    CHECK(reconverge_lowered.source_size == sizeof(reconverge));
}

TEST_CASE("Ampere closing self-branch needs an unconditional EXIT",
          "[video_core][ampere][capture]") {
    const Instruction conditional_exit{{0x794d, 0, 0x00800000, 0}};
    const Instruction predicated_exit{{0x094d, 0, 0x03800000, 0}};
    for (const auto& instruction : {Nop, conditional_exit, predicated_exit}) {
        const std::array code{instruction, SelfBranch};
        CHECK(Capture(code) == std::pair{Result::Continue, 2u});
        CHECK_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    }

    const Instruction branch_forward{{0x0947, 0x10, 0x03800000, 0}};
    const Instruction predicated_branch{{0x0947, 0xfffffff0, 0x0383ffff, 0}};
    const Instruction conditional_branch{{0x7947, 0xfffffff0, 0x0083ffff, 0}};
    const Instruction diverged_branch{{0x7947, 0xfffffff2, 0x0383ffff, 0}};
    for (const auto& branch : {predicated_branch, conditional_branch, diverged_branch}) {
        const std::array code{branch_forward, Exit, branch, SelfBranch};
        CHECK(Capture(code) == std::pair{Result::Complete, 3u});
        const auto lowered = LowerToMaxwell(code);
        REQUIRE(static_cast<bool>(lowered));
        CHECK(lowered.source_size == 4 * sizeof(Instruction));
    }
}

TEST_CASE("Ampere jump table capture still ends at its unpredicated closing branch",
          "[video_core][ampere][capture]") {
    const Instruction brx{{0x7949, 0xfffffff0, 0x0383ffff, 0}};
    const Instruction conditional_branch{{0x7947, 0xfffffff0, 0x0083ffff, 0}};
    const std::array code{brx, Exit, conditional_branch, SelfBranch, InvalidBranch};
    CHECK(Capture(code) == std::pair{Result::Complete, 3u});
    const auto lowered = LowerToMaxwell(std::span{code}.first(4));
    REQUIRE(static_cast<bool>(lowered));
    CHECK(lowered.source_size == 4 * sizeof(Instruction));
    CHECK(Capture(std::array{brx, SelfBranch}) == std::pair{Result::Complete, 1u});
}
