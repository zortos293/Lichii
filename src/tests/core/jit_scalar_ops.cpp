// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Scalar saturating and half-precision instructions against the software model the x64 backend
// falls back to (dynarmic's FP:: operations): result and FPSR, for random operands, special values
// and FPCR settings. On an arm64 host these go through native instructions, and the random-block
// test cannot tell a wrong one apart: both of its JITs use the same backend there.

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "tests/core/host_cpu_features.h"

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/fp/fpsr.h"
#include "dynarmic/common/fp/op/FPMulAdd.h"
#include "dynarmic/common/fp/op/FPNeg.h"
#include "dynarmic/common/fp/op/FPRoundInt.h"
#include "dynarmic/common/fp/op/FPToFixed.h"
#include "dynarmic/common/fp/rounding_mode.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/exclusive_monitor.h"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

namespace {

using namespace Dynarmic;

constexpr u64 CodeStart = 0x1000;
constexpr u32 Svc0 = 0xD4000001;
/// IOC, DZC, OFC, UFC, IXC, IDC and QC.
constexpr u32 FpsrFlags = 0x0800'009F;

class ScalarEnv final : public A64::UserCallbacks {
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

struct Result {
    A64::Vector v0;
    u64 x0;
    u32 fpsr;
};

class ScalarRunner {
public:
    ScalarRunner() {
        config.callbacks = &env;
        config.global_monitor = &monitor;
        config.enable_cycle_counting = false;
        jit.emplace(config);
        env.jit = &*jit;
    }

    /// Runs one instruction with v1..v3 (or the low bits of them) as operands.
    Result Run(u32 instruction, u64 a, u64 b, u64 c, u32 fpcr) {
        env.instruction = instruction;
        // Only the one block at CodeStart changes: dropping it is much cheaper than the whole
        // cache, which the x64 backend's tests spent most of their time rebuilding.
        jit->InvalidateCacheRange(CodeStart, 4);
        std::array<A64::Vector, 32> vectors{};
        vectors[0] = {0x5A5A'5A5A'5A5A'5A5A, 0xA5A5'A5A5'A5A5'A5A5};
        vectors[1] = {a, 0x1111'1111'1111'1111};
        vectors[2] = {b, 0x2222'2222'2222'2222};
        vectors[3] = {c, 0x3333'3333'3333'3333};
        jit->SetVectors(vectors);
        jit->SetRegister(0, 0xDEAD'BEEF'DEAD'BEEF);
        jit->SetPC(CodeStart);
        jit->SetFpcr(fpcr);
        jit->SetFpsr(0);
        jit->Run();
        return {jit->GetVectors()[0], jit->GetRegister(0), jit->GetFpsr() & FpsrFlags};
    }

private:
    ScalarEnv env;
    ExclusiveMonitor monitor{1};
    A64::UserConfig config{};
    std::optional<A64::Jit> jit;
};

std::mt19937_64 rng{0x5ca1a7};

u64 Random() {
    return rng();
}

/// Zeros, denormals, the largest normals, infinities, quiet and signalling NaNs, and some exact
/// values. Every test goes through all of them; random operands cover the rest.
constexpr std::array<u16, 16> SpecialHalves{
    0x0000, 0x8000, 0x0001, 0x83FF, 0x03FF, 0x0400, 0x7BFF, 0xFBFF,
    0x7C00, 0xFC00, 0x7E00, 0x7D00, 0xFE01, 0x3C00, 0x3800, 0xBE00,
};

/// Half-precision operands: random bits (any class), and the special values above.
u16 RandomHalf() {
    const u64 r = Random();
    return (r & 3) == 0 ? SpecialHalves[(r >> 2) % SpecialHalves.size()]
                        : static_cast<u16>(r >> 8);
}

/// Random operands each test runs after its special values. Each one JIT-compiles a block on
/// every backend, slowly on the x64 one, whose half-precision operations fall back to C++: the
/// special values and the FPCR settings give the coverage, not the count.
constexpr int RandomHalfCases = 1500;
constexpr int RandomSaturatingCases = 500;

/// RMode, FZ16, FZ, DN and AHP in every combination the operations read.
u32 RandomFpcr() {
    return static_cast<u32>(Random()) & 0x07C8'0000;
}

FP::FPSR Flags(u32 fpsr) {
    return FP::FPSR{fpsr};
}

// The references stay within 64 bits (no __int128, which MSVC lacks): narrower types widen to
// 64 bits, and 64-bit operands detect overflow from the wrapped result.
template <typename T>
T SaturatingAdd(T a, T b, bool& saturated) {
    if constexpr (sizeof(T) < 8) {
        using Wide = std::conditional_t<std::is_signed_v<T>, s64, u64>;
        const Wide sum = static_cast<Wide>(a) + static_cast<Wide>(b);
        const Wide clamped = std::clamp<Wide>(sum, std::numeric_limits<T>::min(), std::numeric_limits<T>::max());
        saturated = clamped != sum;
        return static_cast<T>(clamped);
    } else if constexpr (std::is_signed_v<T>) {
        const u64 sum = static_cast<u64>(a) + static_cast<u64>(b);
        saturated = static_cast<s64>((static_cast<u64>(a) ^ sum) & (static_cast<u64>(b) ^ sum)) < 0;
        return saturated ? (a < 0 ? std::numeric_limits<T>::min() : std::numeric_limits<T>::max())
                         : static_cast<T>(sum);
    } else {
        const T sum = static_cast<T>(a + b);
        saturated = sum < a;
        return saturated ? std::numeric_limits<T>::max() : sum;
    }
}

template <typename T>
T SaturatingSub(T a, T b, bool& saturated) {
    if constexpr (sizeof(T) < 8) {
        const s64 difference = static_cast<s64>(a) - static_cast<s64>(b);
        const s64 clamped = std::clamp<s64>(difference, static_cast<s64>(std::numeric_limits<T>::min()),
                                            static_cast<s64>(std::numeric_limits<T>::max()));
        saturated = clamped != difference;
        return static_cast<T>(clamped);
    } else if constexpr (std::is_signed_v<T>) {
        const u64 difference = static_cast<u64>(a) - static_cast<u64>(b);
        saturated =
            static_cast<s64>((static_cast<u64>(a) ^ static_cast<u64>(b)) & (static_cast<u64>(a) ^ difference)) < 0;
        return saturated ? (a < 0 ? std::numeric_limits<T>::min() : std::numeric_limits<T>::max())
                         : static_cast<T>(difference);
    } else {
        saturated = a < b;
        return saturated ? T{0} : static_cast<T>(a - b);
    }
}

/// SQDMULH on 16 or 32 bits: the high half of 2ab, which only overflows for min * min.
template <typename T>
T SaturatingDoublingMultiplyHigh(T a, T b, bool& saturated) {
    static_assert(sizeof(T) <= 4);
    constexpr int bits = sizeof(T) * 8;
    saturated = a == std::numeric_limits<T>::min() && b == std::numeric_limits<T>::min();
    if (saturated) {
        return std::numeric_limits<T>::max();
    }
    const s64 product = static_cast<s64>(a) * static_cast<s64>(b) * 2;
    return static_cast<T>(product >> bits);
}

constexpr u32 Operands3(u32 base) {
    return base | (2u << 16) | (1u << 5) | 0u;  // Vd = 0, Vn = 1, Vm = 2
}

template <typename T>
u64 Mask() {
    return sizeof(T) == 8 ? ~u64{0} : (u64{1} << (sizeof(T) * 8)) - 1;
}

/// SQADD/UQADD/SQSUB/UQSUB/SQDMULH on one size: result in the low bits of v0, the rest zeroed.
template <typename T, typename Op>
void CheckSaturating(ScalarRunner& runner, u32 base, Op op) {
    for (int i = 0; i < RandomSaturatingCases; ++i) {
        u64 a = Random() & Mask<T>();
        u64 b = Random() & Mask<T>();
        // Push half of them to the limits, where they saturate: within 3 of the maximum or the
        // minimum (computed in u64, which wraps where T would overflow).
        if (i % 2 == 0) {
            const u64 r = Random() & 3;
            a = ((i & 4) ? static_cast<u64>(std::numeric_limits<T>::max()) - r
                         : static_cast<u64>(std::numeric_limits<T>::min()) + r) &
                Mask<T>();
        }
        bool saturated = false;
        const T expected = op(static_cast<T>(a), static_cast<T>(b), saturated);
        const Result result = runner.Run(Operands3(base), a, b, 0, RandomFpcr());
        INFO("instruction 0x" << std::hex << base << " a=0x" << a << " b=0x" << b);
        REQUIRE(result.v0[0] == (static_cast<u64>(expected) & Mask<T>()));
        REQUIRE(result.v0[1] == 0);
        REQUIRE(Flags(result.fpsr).QC() == saturated);
    }
}

/// FP::FPToFixed raises Invalid Operation for any negative operand of an unsigned conversion, but
/// Arm's FPToFixed only does when the rounded value is negative: one that rounds to zero is merely
/// inexact (FCVTNU of -0.3 gives 0 and IXC on hardware). Returns the flags Arm defines there, on an
/// arm64 host, whose instructions behave as Arm defines. Elsewhere the JIT falls back to that model,
/// which is fixed separately: until then the test expects what the model gives.
std::optional<u32> UnsignedRoundsToZeroFlags([[maybe_unused]] u16 op, [[maybe_unused]] std::size_t fbits,
                                             [[maybe_unused]] FP::FPCR fpcr,
                                             [[maybe_unused]] FP::RoundingMode mode) {
#if !defined(__aarch64__) && !defined(_M_ARM64)
    return std::nullopt;
#else
    const bool sign = (op & 0x8000) != 0;
    const u32 exponent = (op >> 10) & 0x1F;
    const u32 mantissa = op & 0x3FF;
    if (!sign || exponent == 0x1F || (exponent == 0 && mantissa == 0) || (exponent == 0 && fpcr.FZ16())) {
        return std::nullopt;
    }
    const double magnitude = exponent == 0 ? std::ldexp(mantissa, -24) : std::ldexp(mantissa | 0x400, int(exponent) - 25);
    const double scaled = magnitude * std::ldexp(1.0, int(fbits));
    double rounded = 0;
    switch (mode) {
    case FP::RoundingMode::ToNearest_TieEven:
        rounded = std::nearbyint(scaled);  // the default FE_TONEAREST
        break;
    case FP::RoundingMode::TowardsPlusInfinity:  // of the negative value: towards zero
    case FP::RoundingMode::TowardsZero:
        rounded = std::trunc(scaled);
        break;
    case FP::RoundingMode::TowardsMinusInfinity:
        rounded = std::ceil(scaled);
        break;
    case FP::RoundingMode::ToNearest_TieAwayFromZero:
        rounded = std::round(scaled);
        break;
    default:
        return std::nullopt;
    }
    if (rounded != 0) {
        return std::nullopt;
    }
    return 0x10;  // IXC
#endif
}

} // Anonymous namespace

TEST_CASE("JIT: scalar saturating add and subtract match the reference", "[jit][scalar_ops]") {
    ScalarRunner runner;
    const auto add = [](auto a, auto b, bool& s) { return SaturatingAdd(a, b, s); };
    const auto sub = [](auto a, auto b, bool& s) { return SaturatingSub(a, b, s); };
    // SQADD, UQADD, SQSUB, UQSUB with size 0-3 (B, H, S, D) in bits 22-23.
    CheckSaturating<s8>(runner, 0x5E200C00, add);
    CheckSaturating<s16>(runner, 0x5E600C00, add);
    CheckSaturating<s32>(runner, 0x5EA00C00, add);
    CheckSaturating<s64>(runner, 0x5EE00C00, add);
    CheckSaturating<u8>(runner, 0x7E200C00, add);
    CheckSaturating<u16>(runner, 0x7E600C00, add);
    CheckSaturating<u32>(runner, 0x7EA00C00, add);
    CheckSaturating<u64>(runner, 0x7EE00C00, add);
    CheckSaturating<s8>(runner, 0x5E202C00, sub);
    CheckSaturating<s16>(runner, 0x5E602C00, sub);
    CheckSaturating<s32>(runner, 0x5EA02C00, sub);
    CheckSaturating<s64>(runner, 0x5EE02C00, sub);
    CheckSaturating<u8>(runner, 0x7E202C00, sub);
    CheckSaturating<u16>(runner, 0x7E602C00, sub);
    CheckSaturating<u32>(runner, 0x7EA02C00, sub);
    CheckSaturating<u64>(runner, 0x7EE02C00, sub);
}

TEST_CASE("JIT: scalar SQDMULH matches the reference", "[jit][scalar_ops]") {
    ScalarRunner runner;
    const auto op = [](auto a, auto b, bool& s) { return SaturatingDoublingMultiplyHigh(a, b, s); };
    CheckSaturating<s16>(runner, 0x5E60B400, op);
    CheckSaturating<s32>(runner, 0x5EA0B400, op);
}

TEST_CASE("JIT: half-precision scalar arithmetic matches the reference", "[jit][scalar_ops][fp16]") {
#if defined(__aarch64__) || defined(_M_ARM64)
    if (!Tests::HostHasScalarFP16()) {
        SKIP("No FEAT_FP16: the arm64 backend's scalar half-precision emitters would assert");
    }
#endif
    ScalarRunner runner;
    // Every pair of special values as n and m, then random ones.
    constexpr int special_pairs = static_cast<int>(SpecialHalves.size() * SpecialHalves.size());
    for (int i = 0; i < special_pairs + RandomHalfCases; ++i) {
        const bool special = i < special_pairs;
        const u16 a = RandomHalf();
        const u16 n = special ? SpecialHalves[i / SpecialHalves.size()] : RandomHalf();
        const u16 m = special ? SpecialHalves[i % SpecialHalves.size()] : RandomHalf();
        const u32 fpcr_bits = RandomFpcr();
        const FP::FPCR fpcr{fpcr_bits};
        INFO("a=0x" << std::hex << a << " n=0x" << n << " m=0x" << m << " fpcr=0x" << fpcr_bits);

        // FABS h0, h1 / FNEG h0, h1: sign bit only, no flags.
        Result result = runner.Run(0x1EE0C020, n, 0, 0, fpcr_bits);
        REQUIRE(result.v0[0] == static_cast<u16>(n & 0x7FFF));
        REQUIRE(result.v0[1] == 0);
        REQUIRE(result.fpsr == 0);
        result = runner.Run(0x1EE14020, n, 0, 0, fpcr_bits);
        REQUIRE(result.v0[0] == static_cast<u16>(n ^ 0x8000));
        REQUIRE(result.fpsr == 0);

        // FMADD, FMSUB, FNMADD, FNMSUB h0, h1 (n), h2 (m), h3 (a)
        struct FusedCase {
            u32 base;
            bool negate_addend;
            bool subtract;
        };
        for (const FusedCase& fused : {FusedCase{0x1FC00000, false, false}, FusedCase{0x1FC08000, false, true},
                                       FusedCase{0x1FE00000, true, true}, FusedCase{0x1FE08000, true, false}}) {
            FP::FPSR fpsr;
            const u16 addend = fused.negate_addend ? FP::FPNeg(a) : a;
            const u16 expected = fused.subtract ? FP::FPMulSub<u16>(addend, n, m, fpcr, fpsr)
                                                : FP::FPMulAdd<u16>(addend, n, m, fpcr, fpsr);
            result = runner.Run(fused.base | (2u << 16) | (3u << 10) | (1u << 5), n, m, a, fpcr_bits);
            INFO("fused 0x" << std::hex << fused.base);
            REQUIRE(result.v0[0] == expected);
            REQUIRE(result.v0[1] == 0);
            REQUIRE(result.fpsr == (fpsr.Value() & FpsrFlags));
        }

        // FRINTN, FRINTP, FRINTM, FRINTZ, FRINTA, FRINTX, FRINTI h0, h1
        struct RoundCase {
            u32 base;
            std::optional<FP::RoundingMode> mode;  ///< Empty: FPCR's
            bool exact;
        };
        for (const RoundCase& round : {RoundCase{0x1EE44000, FP::RoundingMode::ToNearest_TieEven, false},
                                       RoundCase{0x1EE4C000, FP::RoundingMode::TowardsPlusInfinity, false},
                                       RoundCase{0x1EE54000, FP::RoundingMode::TowardsMinusInfinity, false},
                                       RoundCase{0x1EE5C000, FP::RoundingMode::TowardsZero, false},
                                       RoundCase{0x1EE64000, FP::RoundingMode::ToNearest_TieAwayFromZero, false},
                                       RoundCase{0x1EE74000, std::nullopt, true},
                                       RoundCase{0x1EE7C000, std::nullopt, false}}) {
            FP::FPSR fpsr;
            const u16 expected = static_cast<u16>(
                FP::FPRoundInt<u16>(n, fpcr, round.mode.value_or(fpcr.RMode()), round.exact, fpsr));
            result = runner.Run(round.base | (1u << 5), n, 0, 0, fpcr_bits);
            INFO("round 0x" << std::hex << round.base);
            REQUIRE(result.v0[0] == expected);
            REQUIRE(result.v0[1] == 0);
            REQUIRE(result.fpsr == (fpsr.Value() & FpsrFlags));
        }
    }
}

TEST_CASE("JIT: half-precision to integer and fixed point matches the reference", "[jit][scalar_ops][fp16]") {
#if defined(__aarch64__) || defined(_M_ARM64)
    if (!Tests::HostHasScalarFP16()) {
        SKIP("No FEAT_FP16: the arm64 backend's scalar half-precision emitters would assert");
    }
#endif
    ScalarRunner runner;
    struct ConvertCase {
        u32 base;
        FP::RoundingMode mode;
    };
    static constexpr std::array<ConvertCase, 5> modes{{
        {0x1EE00000, FP::RoundingMode::ToNearest_TieEven},     // FCVTN<S|U>
        {0x1EE80000, FP::RoundingMode::TowardsPlusInfinity},   // FCVTP<S|U>
        {0x1EF00000, FP::RoundingMode::TowardsMinusInfinity},  // FCVTM<S|U>
        {0x1EF80000, FP::RoundingMode::TowardsZero},           // FCVTZ<S|U>
        {0x1EE40000, FP::RoundingMode::ToNearest_TieAwayFromZero},  // FCVTA<S|U>
    }};
    // Every special value with FZ16 clear and set (the only FPCR bit a conversion with its own
    // rounding mode reads), then random ones under random FPCR settings.
    constexpr int specials = static_cast<int>(SpecialHalves.size() * 2);
    for (int i = 0; i < specials + RandomHalfCases; ++i) {
        const bool special = i < specials;
        const u16 n = special ? SpecialHalves[i / 2] : RandomHalf();
        const u32 fpcr_bits = special ? ((i & 1) ? 0x0008'0000U : 0U) | (RandomFpcr() & ~0x0008'0000U)
                                      : RandomFpcr();
        const FP::FPCR fpcr{fpcr_bits};
        for (const bool is_unsigned : {false, true}) {
            for (const std::size_t ibits : {32u, 64u}) {
                const u32 sf = ibits == 64 ? 0x8000'0000 : 0;
                const u32 u = is_unsigned ? 0x10000 : 0;
                // A W destination zero-extends.
                const u64 ibits_mask = ibits == 64 ? ~u64{0} : 0xFFFF'FFFF;
                INFO("n=0x" << std::hex << n << " fpcr=0x" << fpcr_bits << " unsigned=" << is_unsigned
                            << " ibits=" << std::dec << ibits);
                for (const ConvertCase& convert : modes) {
                    FP::FPSR fpsr;
                    const u64 expected = FP::FPToFixed<u16>(ibits, n, 0, is_unsigned, fpcr, convert.mode, fpsr) & ibits_mask;
                    // FCVT<mode><S|U> w0/x0, h1
                    const Result result = runner.Run(convert.base | sf | u | (1u << 5), n, 0, 0, fpcr_bits);
                    INFO("convert 0x" << std::hex << convert.base);
                    REQUIRE(result.x0 == expected);
                    const auto arm_flags = is_unsigned ? UnsignedRoundsToZeroFlags(n, 0, fpcr, convert.mode) : std::nullopt;
                    REQUIRE(result.fpsr == arm_flags.value_or(fpsr.Value() & FpsrFlags));
                }
                // FCVTZ<S|U> w0/x0, h1, #fbits
                const std::size_t fbits = 1 + Random() % ibits;
                FP::FPSR fpsr;
                const u64 expected =
                    FP::FPToFixed<u16>(ibits, n, fbits, is_unsigned, fpcr, FP::RoundingMode::TowardsZero, fpsr) &
                    ibits_mask;
                const Result result = runner.Run(
                    0x1ED80000 | sf | u | (static_cast<u32>(64 - fbits) << 10) | (1u << 5), n, 0, 0, fpcr_bits);
                INFO("fixed fbits=" << std::dec << fbits);
                REQUIRE(result.x0 == expected);
                const auto arm_flags =
                    is_unsigned ? UnsignedRoundsToZeroFlags(n, fbits, fpcr, FP::RoundingMode::TowardsZero) : std::nullopt;
                REQUIRE(result.fpsr == arm_flags.value_or(fpsr.Value() & FpsrFlags));
            }
        }
    }
}
