// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// FRINT* with the rounding mode the instruction names, or FPCR's for FRINTX/FRINTI. On x64 the
// half-precision ones, FRINTA and FRINTX (and every FRINT without SSE4.1) go through a C++
// fallback that is handed the mode and the exact flag.

#include <array>
#include <cstdint>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/exclusive_monitor.h"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

namespace {

using namespace Dynarmic;

constexpr u64 CodeStart = 0x1000;
constexpr u32 Svc0 = 0xD4000001;
constexpr u32 FpcrRoundUp = 0x0040'0000;    // RMode = RP
constexpr u32 FpcrRoundDown = 0x0080'0000;  // RMode = RM
constexpr u32 Ixc = 0x10;

class RoundEnv final : public A64::UserCallbacks {
public:
    A64::Jit* jit = nullptr;
    u32 instruction = 0;

    std::optional<std::uint32_t> MemoryReadCode(u64 vaddr) override {
        return vaddr == CodeStart ? instruction : Svc0;
    }
    u64 MemoryRead(u64, size_t) override { return 0; }
    A64::Vector MemoryRead128(u64) override { return {}; }
    void MemoryWrite(u64, u64, size_t) override {}
    void MemoryWrite128(u64, A64::Vector) override {}
    void CallSVC(std::uint32_t) override { jit->HaltExecution(); }
    void ExceptionRaised(u64, A64::Exception) override {}
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 0; }
    u64 GetCNTPCT() override { return 0; }
};

struct Rounded {
    u64 value;
    u32 fpsr;
};

/// Runs `instruction` (Vd = v0, Vn = v1) on `input` with `fpcr`.
Rounded Run(u32 instruction, u64 input, u32 fpcr) {
    RoundEnv env;
    env.instruction = instruction | (1u << 5);
    ExclusiveMonitor monitor{1};
    A64::UserConfig config{};
    config.callbacks = &env;
    config.global_monitor = &monitor;
    config.enable_cycle_counting = false;
    A64::Jit jit{config};
    env.jit = &jit;
    std::array<A64::Vector, 32> vectors{};
    vectors[1] = {input, 0};
    jit.SetVectors(vectors);
    jit.SetPC(CodeStart);
    jit.SetFpcr(fpcr);
    jit.SetFpsr(0);
    jit.Run();
    return {jit.GetVectors()[0][0], jit.GetFpsr() & 0x9F};
}

} // Anonymous namespace

// The arm64 backend has no half-precision FRINT yet: those cases run where it is the x64 backend's.
#if defined(__x86_64__) || defined(_M_X64)
constexpr bool RunHalves = true;
#else
constexpr bool RunHalves = false;
#endif

TEST_CASE("JIT: FRINT rounds with the mode the instruction names", "[jit][round_int]") {
    // FRINTA S and D: ties round away from zero whatever FPCR says. Only FRINTX reports Inexact.
    for (const auto& [instruction, input, expected] : std::array<std::array<u64, 3>, 4>{{
             {0x1E264000, 0x4020'0000, 0x4040'0000},                          // 2.5f -> 3
             {0x1E264000, 0xC020'0000, 0xC040'0000},                          // -2.5f -> -3
             {0x1E664000, 0x4004'0000'0000'0000, 0x4008'0000'0000'0000},      // 2.5 -> 3
             {0x1E664000, 0xC004'0000'0000'0000, 0xC008'0000'0000'0000},      // -2.5 -> -3
         }}) {
        const Rounded rounded = Run(static_cast<u32>(instruction), input, FpcrRoundDown);
        INFO("instruction 0x" << std::hex << instruction << " input 0x" << input);
        CHECK(rounded.value == expected);
        CHECK(rounded.fpsr == 0);
    }
    if (RunHalves) {
        // FRINTM H under FPCR's round up: 20.796875 goes down to 20.
        const Rounded down = Run(0x1EE54000, 0x4D33, FpcrRoundUp);
        CHECK(down.value == 0x4D00);
        CHECK(down.fpsr == 0);
        // FRINTA H: -2.5 to -3.
        CHECK(Run(0x1EE64000, 0xC100, FpcrRoundDown).value == 0xC200);
        // FRINTI H takes FPCR's mode: 2.5 down to 2, not inexact.
        const Rounded current = Run(0x1EE7C000, 0x4100, FpcrRoundDown);
        CHECK(current.value == 0x4000);
        CHECK(current.fpsr == 0);
    }
}

TEST_CASE("JIT: FRINTX rounds with FPCR's mode and reports inexact", "[jit][round_int]") {
    // FRINTX S and D under round down: 2.5 to 2, inexact.
    const Rounded single = Run(0x1E274000, 0x4020'0000, FpcrRoundDown);
    CHECK(single.value == 0x4000'0000);
    CHECK(single.fpsr == Ixc);
    const Rounded twice = Run(0x1E674000, 0x4004'0000'0000'0000, FpcrRoundDown);
    CHECK(twice.value == 0x4000'0000'0000'0000);
    CHECK(twice.fpsr == Ixc);
    if (RunHalves) {
        // FRINTX H under round down: 2.5 to 2, inexact.
        const Rounded half = Run(0x1EE74000, 0x4100, FpcrRoundDown);
        CHECK(half.value == 0x4000);
        CHECK(half.fpsr == Ixc);
    }
    // An integer stays exact.
    CHECK(Run(0x1E674000, 0x4000'0000'0000'0000, FpcrRoundDown).fpsr == 0);
}
