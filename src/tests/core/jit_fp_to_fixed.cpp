// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// A negative value converted to an unsigned fixed-point or integer value raised Invalid Operation
// even when it rounds to zero. In Arm's FPToFixed the value is rounded first, and only a result
// below zero is out of range; one that rounds to zero gives 0 with Inexact. An Apple M6 gives
// FCVTNU W0, H1 with H1 = 0x83FF (a negative denormal) and FPCR 0x04C00000 the result 0 and
// FPSR IXC (0x10).

#include <array>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "common/common_types.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/fp/fpsr.h"
#include "dynarmic/common/fp/op/FPToFixed.h"
#include "dynarmic/common/fp/rounding_mode.h"
#include "dynarmic/interface/A64/a64.h"

namespace {

using Dynarmic::FP::FPCR;
using Dynarmic::FP::FPSR;
using Dynarmic::FP::RoundingMode;

constexpr u32 IOC = 0x01;
constexpr u32 IXC = 0x10;

enum Value : size_t { MinusZero, Denormal, MinusPoint4, MinusHalf, MinusPoint6, MinusOne, Values };

// Bit patterns of the inputs, in half, single and double precision.
constexpr std::array<u16, Values> Half{0x8000, 0x83FF, 0xB666, 0xB800, 0xB8CD, 0xBC00};
constexpr std::array<u32, Values> Single{0x80000000, 0x80000001, 0xBECCCCCD,
                                         0xBF000000, 0xBF19999A, 0xBF800000};
constexpr std::array<u64, Values> Double{0x8000000000000000, 0x800FFFFFFFFFFFFF,
                                         0xBFD999999999999A, 0xBFE0000000000000,
                                         0xBFE3333333333333, 0xBFF0000000000000};

struct Expected {
    u64 result;
    u32 fpsr;
};

constexpr std::array Modes{RoundingMode::ToNearest_TieEven, RoundingMode::TowardsPlusInfinity,
                           RoundingMode::TowardsMinusInfinity, RoundingMode::TowardsZero,
                           RoundingMode::ToNearest_TieAwayFromZero};
constexpr std::array<const char*, Modes.size()> ModeNames{"RN", "RP", "RM", "RZ", "RA"};

// The rounded value of each input under each mode: 0 or -1.
constexpr std::array<std::array<int, Modes.size()>, Values> Rounded{{
    {0, 0, 0, 0, 0},     // -0.0
    {0, 0, -1, 0, 0},    // negative denormal
    {0, 0, -1, 0, 0},    // -0.4
    {0, 0, -1, 0, -1},   // -0.5: ties to even give 0, ties away give -1
    {-1, 0, -1, 0, -1},  // -0.6
    {-1, -1, -1, -1, -1} // -1.0
}};

Expected ExpectedFor(size_t value, size_t mode, bool unsigned_, size_t ibits) {
    const bool exact = value == MinusZero || value == MinusOne;
    const int rounded = Rounded[value][mode];
    if (unsigned_ && rounded < 0) {
        return {0, IOC};
    }
    const u64 mask = ibits == 64 ? ~u64{0} : (u64{1} << ibits) - 1;
    return {static_cast<u64>(static_cast<s64>(rounded)) & mask, exact ? 0 : IXC};
}

template <typename FPT>
void CheckAll(const std::array<FPT, Values>& inputs) {
    const FPCR fpcr;
    for (size_t ibits : {16, 32, 64}) {
        for (bool unsigned_ : {true, false}) {
            for (size_t value = 0; value < Values; ++value) {
                for (size_t mode = 0; mode < Modes.size(); ++mode) {
                    FPSR fpsr;
                    const u64 result = Dynarmic::FP::FPToFixed<FPT>(
                        ibits, inputs[value], 0, unsigned_, fpcr, Modes[mode], fpsr);
                    const Expected expected = ExpectedFor(value, mode, unsigned_, ibits);
                    INFO(fmt::format("input {:#x}, {} bits, {}, {}", inputs[value], ibits,
                                     unsigned_ ? "unsigned" : "signed", ModeNames[mode]));
                    CHECK(result == expected.result);
                    CHECK(fpsr.Value() == expected.fpsr);
                }
            }
        }
    }
}

} // namespace

TEST_CASE("FPToFixed rounds a negative value before it checks the unsigned range",
          "[jit][fp_to_fixed]") {
    SECTION("half") {
        CheckAll(Half);
    }
    SECTION("single") {
        CheckAll(Single);
    }
    SECTION("double") {
        CheckAll(Double);
    }
}

TEST_CASE("FPToFixed scales by the fraction bits before it rounds a negative value",
          "[jit][fp_to_fixed]") {
    // -2^-5 with 4 fraction bits is -0.5 units: ties to even and towards zero give 0, towards
    // minus infinity and ties away give -1.
    constexpr u32 input = 0xBD000000;
    const FPCR fpcr;
    for (size_t mode = 0; mode < Modes.size(); ++mode) {
        INFO(ModeNames[mode]);
        const bool below = Modes[mode] == RoundingMode::TowardsMinusInfinity ||
                           Modes[mode] == RoundingMode::ToNearest_TieAwayFromZero;
        FPSR fpsr;
        CHECK(Dynarmic::FP::FPToFixed<u32>(32, input, 4, true, fpcr, Modes[mode], fpsr) == 0);
        CHECK(fpsr.Value() == (below ? IOC : IXC));
        // With no fraction bits it is -0.03125, which only towards minus infinity rounds below 0.
        FPSR whole;
        CHECK(Dynarmic::FP::FPToFixed<u32>(32, input, 0, true, fpcr, Modes[mode], whole) == 0);
        CHECK(whole.Value() == (Modes[mode] == RoundingMode::TowardsMinusInfinity ? IOC : IXC));
    }
}

TEST_CASE("FPToFixed does not take a negative value rounded up for an overflow",
          "[jit][fp_to_fixed]") {
    // -2147483647.5 and -32767.5 towards zero or plus infinity are -2147483647 and -32767, which
    // fit; the overflow check added the round-up to their magnitude.
    const FPCR fpcr;
    for (const auto mode : {RoundingMode::TowardsZero, RoundingMode::TowardsPlusInfinity}) {
        FPSR fpsr;
        CHECK(Dynarmic::FP::FPToFixed<u64>(32, 0xC1DFFFFFFFE00000, 0, false, fpcr, mode, fpsr) ==
              0x80000001);
        CHECK(fpsr.Value() == IXC);
        FPSR fpsr16;
        CHECK(Dynarmic::FP::FPToFixed<u32>(16, 0xC6FFFF00, 0, false, fpcr, mode, fpsr16) ==
              0x8001);
        CHECK(fpsr16.Value() == IXC);
    }
    // Below the range they still overflow.
    FPSR fpsr;
    CHECK(Dynarmic::FP::FPToFixed<u64>(32, 0xC1E0000000200000, 0, false, fpcr,
                                       RoundingMode::TowardsPlusInfinity, fpsr) == 0x80000000);
    CHECK(fpsr.Value() == IOC);
}

namespace {

constexpr u64 CodeBase = 0x1000;
constexpr u32 Svc = 0xD4000001;

class Environment final : public Dynarmic::A64::UserCallbacks {
public:
    Dynarmic::A64::Jit* jit{};
    std::vector<u32> code;

    std::optional<u32> MemoryReadCode(u64 address) override {
        const u64 index = (address - CodeBase) / 4;
        if (address < CodeBase || index >= code.size()) {
            return std::nullopt;
        }
        return code[index];
    }
    u64 MemoryRead(u64, size_t) override {
        FAIL("Unexpected read");
        return 0;
    }
    Dynarmic::A64::Vector MemoryRead128(u64) override {
        FAIL("Unexpected read");
        return {};
    }
    void MemoryWrite(u64, u64, size_t) override {
        FAIL("Unexpected write");
    }
    void MemoryWrite128(u64, Dynarmic::A64::Vector) override {
        FAIL("Unexpected write");
    }
    bool MemoryWriteExclusive64(u64, u64, u64) override {
        FAIL("Unexpected write");
        return false;
    }
    void CallSVC(u32) override {
        jit->HaltExecution();
    }
    void ExceptionRaised(u64, Dynarmic::A64::Exception) override {
        FAIL("Unexpected exception");
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override {
        return 1000;
    }
    u64 GetCNTPCT() override {
        return 0;
    }
};

struct Conversion {
    u32 fpsr;
    u64 result;
};

Conversion RunConversion(u32 instruction, u64 input, u32 fpcr) {
    Environment env;
    env.code = {instruction, Svc};
    Dynarmic::A64::UserConfig config;
    config.callbacks = &env;
    config.enable_cycle_counting = false;
    Dynarmic::A64::Jit jit{config};
    env.jit = &jit;
    jit.SetVector(1, {input, 0});
    jit.SetRegister(0, ~u64{0});
    jit.SetFpcr(fpcr);
    jit.SetFpsr(0);
    jit.SetPC(CodeBase);
    jit.Run();
    return {jit.GetFpsr(), jit.GetRegister(0)};
}

// FCVT<mode>U W0, <H|S|D>1: rmode 00 N, 01 P, 10 M, 11 Z; opcode 001 (U).
constexpr u32 FcvtU(u32 ftype, u32 rmode) {
    return 0x1E210020 | (ftype << 22) | (rmode << 19);
}
// FCVTAU W0, <H|S|D>1.
constexpr u32 FcvtAU(u32 ftype) {
    return 0x1E250020 | (ftype << 22);
}

} // namespace

// The arm64 backend has no half-precision to integer conversion yet (its emitters assert): the
// half-precision cases run where the x64 backend compiles them.
#if defined(__x86_64__) || defined(_M_X64)
constexpr bool RunHalves = true;
#else
constexpr bool RunHalves = false;
#endif

TEST_CASE("The JIT converts a negative value that rounds to zero to unsigned with Inexact",
          "[jit][fp_to_fixed]") {
    if (RunHalves) {
        // The reference result from an Apple M6.
        const auto reference = RunConversion(FcvtU(3, 0), 0x83FF, 0x04C00000);
        CHECK(reference.result == 0);
        CHECK(reference.fpsr == IXC);
    }

    // These conversions reach FP::FPToFixed through the x64 fallback: every half-precision one,
    // and FCVTAU, which has no SSE rounding mode. The SSE path for the other single and double
    // conversions saturates without raising Invalid Operation, which this does not cover.
    // FPSR.IOC and IXC are sticky; RunConversion starts from 0 each time.
    constexpr std::array<u32, 5> RModeOf{0, 1, 2, 3, 0};
    for (u32 ftype : {3u, 0u, 1u}) {
        if (ftype == 3 && !RunHalves) {
            continue;
        }
        for (size_t value = 0; value < Values; ++value) {
            const u64 input = ftype == 3   ? u64{Half[value]}
                              : ftype == 0 ? u64{Single[value]}
                                           : Double[value];
            for (size_t mode = 0; mode < Modes.size(); ++mode) {
                if (ftype != 3 && mode != 4) {
                    continue;
                }
                const u32 instruction =
                    mode == 4 ? FcvtAU(ftype) : FcvtU(ftype, RModeOf[mode]);
                INFO(fmt::format("ftype {}, input {:#x}, {}", ftype, input, ModeNames[mode]));
                const auto got = RunConversion(instruction, input, 0);
                const Expected expected = ExpectedFor(value, mode, true, 32);
                CHECK(got.result == expected.result);
                CHECK(got.fpsr == expected.fpsr);
            }
        }
    }
}
