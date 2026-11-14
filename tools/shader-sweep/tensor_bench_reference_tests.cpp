// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Standalone oracle tests for the tensor benchmark fixture (no Catch2 dependency).  The CPU
// expected results produced by TensorBench::MakeFixture are checked against the RTX 4090 HMMA
// reference fragments measured in src/tests/video_core/ampere_matrix.cpp ("Native half matrix
// rounding matches measured hardware fragments"; the samples come from Duck912/eden-fefw
// native_shader.cpp at 8c6f0522b6ee07caa47ab4363823d3dc00aa8ac1, seed zero with a nonzero C
// fragment).
//
// This independent model reconstructs matrices from the fixture's input fragments. It
// accumulates in float32, rounds to half after each K8 group and chains the operations by
// feeding each rounded result back as the next C; the measured samples anchor only the
// single-operation seed-zero fractions warp, not every possible floating-point input.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <string>
#include <vector>

#include "tensor_bench_shader.h"

namespace {
using u32 = std::uint32_t;
using u16 = std::uint16_t;

int check_count = 0;
int failure_count = 0;

void Check(bool ok, const std::string& what) {
    ++check_count;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failure_count;
    }
}

bool MakeFixtureThrows(const TensorBench::Parameters& parameters, u32 workgroups) {
    try {
        TensorBench::MakeFixture(parameters, workgroups, 0);
    } catch (...) {
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Reference half conversion, mirroring DecodeTestHalf/EncodeTestHalf in
// src/tests/video_core/ampere_matrix.cpp (nearbyint ties-to-even, saturation at 65520,
// deliberately noncanonical 0xfe01 NaN).
double DecodeHalf(u16 bits) {
    const u32 exponent = (bits >> 10) & 31, mantissa = bits & 1023;
    const double value = exponent == 31  ? (mantissa ? std::numeric_limits<double>::quiet_NaN()
                                                     : std::numeric_limits<double>::infinity())
                         : exponent == 0 ? std::ldexp(static_cast<double>(mantissa), -24)
                                         : std::ldexp(static_cast<double>(1024 + mantissa),
                                                      static_cast<int>(exponent) - 25);
    return (bits & 0x8000) ? -value : value;
}

u16 EncodeHalf(double value) {
    const u16 sign = std::signbit(value) ? 0x8000 : 0;
    value = std::abs(value);
    if (std::isnan(value))
        return 0xfe01; // Deliberately noncanonical: ReLU must canonicalize it.
    if (value >= 65520)
        return sign | 0x7c00;
    if (value < std::ldexp(1.0, -14))
        return sign | static_cast<u16>(std::nearbyint(std::ldexp(value, 24)));
    int exponent{};
    std::frexp(value, &exponent);
    --exponent;
    u32 mantissa = static_cast<u32>(std::nearbyint(std::ldexp(value, 10 - exponent)));
    if (mantissa == 2048) {
        ++exponent;
        mantissa = 1024;
    }
    return sign | static_cast<u16>(((exponent + 15) << 10) | (mantissa - 1024));
}

u32 PackHalfs(double low, double high) {
    return u32{EncodeHalf(low)} | (u32{EncodeHalf(high)} << 16);
}

double DecodeWordHalf(u32 word, u32 high) {
    return DecodeHalf(static_cast<u16>(word >> (high * 16)));
}

// RTX 4090 driver-API MMA samples, seed zero with a nonzero C fragment, indexed
// [depth == 16][lane * 2 + fragment]; copied from src/tests/video_core/ampere_matrix.cpp
// (originally Duck912/eden-fefw native_shader.cpp at 8c6f0522b6ee07caa47ab4363823d3dc00aa8ac1).
constexpr std::array<std::array<u32, 64>, 2> Hardware{{
    {{
        0x42af42edU, 0xc05ec08fU, 0x42334271U, 0xbff4c02cU, 0x41b641f5U, 0xbf2dbf90U,
        0x413a4178U, 0xbe66bec9U, 0x414e417eU, 0xc1bfc1ffU, 0x40ed411dU, 0xc140c17fU,
        0x408d40bdU, 0xc0c0c100U, 0x402c405cU, 0xc040c080U, 0x3fd8400eU, 0xc01dc042U,
        0x3f4f3f94U, 0xbfa7bff0U, 0x3ec63f0bU, 0xbf13bf5dU, 0x3e3e3e82U, 0xbe80bec9U,
        0x3d153d3dU, 0x32de338dU, 0x3cc43cecU, 0x318d3234U, 0x3c733c9cU, 0x303630dfU,
        0x3c223c4aU, 0x2dbf2f14U, 0x38a438bcU, 0x42a742e1U, 0x3872388bU, 0x4233426dU,
        0x38403859U, 0x41be41f9U, 0x380e3827U, 0x414a4184U, 0xaf15b008U, 0x41454171U,
        0xad24ae1cU, 0x40ed4119U, 0xaa63ac2bU, 0x409540c1U, 0xa4fea870U, 0x403d4069U,
        0xba69bac0U, 0x3fc84002U, 0xb9bbba12U, 0x3f4f3f8bU, 0xb90cb964U, 0x3ed73f13U,
        0xb85eb8b5U, 0x3e5e3e9aU, 0xbdf8be3fU, 0x3d043d24U, 0xbd69bdb0U, 0x3cc43ce4U,
        0xbcd9bd21U, 0x3c833ca4U, 0xbc4abc92U, 0x3c433c63U,
    }},
    {{
        0x43d6441bU, 0xc207c249U, 0x43164376U, 0xc183c1c5U, 0x425542b6U, 0xc0fec140U,
        0x419441f5U, 0xc07ac0bcU, 0x42114255U, 0xc1cdc1edU, 0x418841ccU, 0xc18cc1acU,
        0x40ff4144U, 0xc14ac16bU, 0x407640baU, 0xc108c129U, 0x404b4073U, 0xbc95bc6aU,
        0x3ff44023U, 0xbceabcbfU, 0x3f523fa3U, 0xbd3ebd14U, 0x3eb13f02U, 0xbd94bd69U,
        0x3d0a3d22U, 0x3b713c26U, 0x3cd83cf0U, 0x39bb3a96U, 0x3ca63cc0U, 0x380538e0U,
        0x3c753c8dU, 0x349f3654U, 0x35f83579U, 0x43b14405U, 0x36f13675U, 0x43014359U,
        0x37eb376eU, 0x425042a9U, 0x38723834U, 0x41a041f8U, 0xb81cb8cbU, 0x41eb4227U,
        0xb57eb6dbU, 0x417241aeU, 0xb189b422U, 0x40fa4136U, 0x995cad9eU, 0x408240beU,
        0xbd9abe29U, 0x40254045U, 0xbc7bbd0bU, 0x3fc94005U, 0xbab9bbd9U, 0x3f493f89U,
        0xb87db99bU, 0x3ec83f08U, 0xc093c0f7U, 0x3cbe3cc6U, 0xbf98c02fU, 0x3cad3cb6U,
        0xbe08bed0U, 0x3c9c3ca5U, 0xbc7abd42U, 0x3c8c3c94U,
    }},
}};

// The exact sample matrices of the measured reference: A/B/C take the HMMA fragment
// coordinates (row, k), (k, col), (row, col).
double SampleA(u32 row, u32 k) {
    return (static_cast<int>((row * 17 + k * 5) % 199) - 99) * 0.00731;
}
double SampleB(u32 k, u32 col) {
    return (static_cast<int>((k * 7 + col * 3) % 211) - 105) * 0.00917;
}
double SampleC(u32 row, u32 col) {
    return (static_cast<int>((row * 5 + col * 3) % 97) - 48) * 0.00319;
}

// Per-lane fragment words of the sample matrices in m16n8k8/k16 layout, exactly as
// ampere_matrix.cpp packs registers 8..11 (A), 16..17 (B) and 24..25 (C).
u32 SampleAWord(u32 lane, u32 fragment) {
    const u32 row = lane / 4 + (fragment % 2) * 8, k = (lane % 4) * 2 + (fragment / 2) * 8;
    return PackHalfs(SampleA(row, k), SampleA(row, k + 1));
}
u32 SampleBWord(u32 lane, u32 fragment) {
    const u32 k = (lane % 4) * 2 + fragment * 8;
    return PackHalfs(SampleB(k, lane / 4), SampleB(k + 1, lane / 4));
}
u32 SampleCWord(u32 lane, u32 fragment) {
    const u32 row = lane / 4 + fragment * 8, col = (lane % 4) * 2;
    return PackHalfs(SampleC(row, col), SampleC(row, col + 1));
}

// ---------------------------------------------------------------------------
// Fixture plumbing: one storage buffer with ten words per invocation.  Words 0..3 hold the A
// fragments (registers 8..11), 4..5 the B fragments (16..17), 6..7 the C fragments (24..25);
// words 8..9 are the results.  The first warp of a seed-zero fractions fixture carries the
// measured sample matrices.
constexpr u32 InvocationsPerWarp = 32;

TensorBench::Parameters SampleParameters(u32 depth, u32 local_size) {
    TensorBench::Parameters parameters;
    parameters.mode = TensorBench::Mode::Software;
    parameters.pattern = TensorBench::Pattern::Fractions;
    parameters.depth = depth;
    parameters.operations = 1;
    parameters.local_size = local_size;
    return parameters;
}

u32 FixtureWord(const TensorBench::Fixture& fixture, u32 invocation, u32 word) {
    return fixture.input[invocation * TensorBench::WordsPerInvocation + word];
}

u32 ExpectedWord(const TensorBench::Fixture& fixture, u32 invocation, u32 word) {
    return fixture.expected[invocation * TensorBench::WordsPerInvocation + word];
}

// Locate one matrix element inside a warp's input words and decode it back to a real number.
double GatherA(const TensorBench::Fixture& fixture, u32 warp_base, u32 row, u32 k) {
    const u32 lane = (row % 8) * 4 + ((k % 8) / 2);
    const u32 word = ((row / 8) % 2) + 2 * ((k / 8) % 2);
    return DecodeWordHalf(FixtureWord(fixture, warp_base + lane, word), k % 2);
}

double GatherB(const TensorBench::Fixture& fixture, u32 warp_base, u32 k, u32 col) {
    const u32 lane = col * 4 + ((k % 8) / 2);
    const u32 word = 4 + (k / 8) % 2;
    return DecodeWordHalf(FixtureWord(fixture, warp_base + lane, word), k % 2);
}

double GatherC(const TensorBench::Fixture& fixture, u32 warp_base, u32 row, u32 col) {
    const u32 lane = (row % 8) * 4 + col / 2;
    const u32 word = 6 + (row / 8) % 2;
    return DecodeWordHalf(FixtureWord(fixture, warp_base + lane, word), col % 2);
}

// HMMA.F16 rounds after each K8 group, including the midpoint of K16. Chained operations keep
// the original A/B inputs and seed the accumulator from C only once, feeding each operation's
// rounded accumulator back as the next C; that reconstructs the fixture's D->C chain without
// the producer's own helpers.
double AccumulateElement(const TensorBench::Fixture& fixture, u32 warp_base, u32 row, u32 col,
                         u32 depth, u32 operations = 1) {
    std::array<float, 16> a_row{}, b_column{};
    for (u32 k = 0; k < depth; ++k) {
        a_row[k] = static_cast<float>(GatherA(fixture, warp_base, row, k));
        b_column[k] = static_cast<float>(GatherB(fixture, warp_base, k, col));
    }
    float accumulator = static_cast<float>(GatherC(fixture, warp_base, row, col));
    for (u32 operation = 0; operation < operations; ++operation) {
        for (u32 k = 0; k < depth; ++k) {
            accumulator = std::fma(a_row[k], b_column[k], accumulator);
            if (k % 8 == 7) {
                accumulator = static_cast<float>(DecodeHalf(EncodeHalf(accumulator)));
            }
        }
    }
    return accumulator;
}

u32 OracleOutWord(const TensorBench::Fixture& fixture, u32 warp_base, u32 lane, u32 fragment,
                  u32 depth, u32 operations = 1) {
    const u32 col = (lane % 4) * 2, row = lane / 4 + fragment * 8;
    return PackHalfs(AccumulateElement(fixture, warp_base, row, col, depth, operations),
                     AccumulateElement(fixture, warp_base, row, col + 1, depth, operations));
}

// ---------------------------------------------------------------------------
void CheckFixtureShape(const char* label, u32 depth, u32 local_size, u32 workgroups) {
    const TensorBench::Fixture fixture =
        TensorBench::MakeFixture(SampleParameters(depth, local_size), workgroups, 0);
    const u32 invocations = workgroups * local_size;
    const std::string prefix =
        std::string(label) + " depth " + std::to_string(depth) + " local " +
        std::to_string(local_size) + " workgroups " + std::to_string(workgroups) + ": ";
    Check(fixture.input.size() == static_cast<size_t>(invocations) * TensorBench::WordsPerInvocation,
          prefix + "input carries ten words per invocation");
    Check(
        fixture.expected.size() == static_cast<size_t>(invocations) * TensorBench::WordsPerInvocation,
        prefix + "expected carries ten words per invocation");
    // The buffer image keeps its input words untouched; only the output slots change.
    bool mirrored = true;
    u32 sentinel_lanes = 0;
    for (u32 invocation = 0; invocation < invocations; ++invocation) {
        for (u32 word = 0; word < 8; ++word)
            mirrored = mirrored &&
                       FixtureWord(fixture, invocation, word) == ExpectedWord(fixture, invocation, word);
        if (FixtureWord(fixture, invocation, 8) != ExpectedWord(fixture, invocation, 8) ||
            FixtureWord(fixture, invocation, 9) != ExpectedWord(fixture, invocation, 9))
            ++sentinel_lanes;
    }
    if (depth == 8) {
        bool zero_fill = true;
        for (u32 invocation = 0; invocation < invocations; ++invocation)
            for (u32 word : {2U, 3U, 5U})
                zero_fill = zero_fill && FixtureWord(fixture, invocation, word) == 0;
        Check(zero_fill, prefix + "unused short-depth A/B words stay zero");
    }
    Check(mirrored, prefix + "expected keeps the first eight input words identical");
    Check(sentinel_lanes >= 1, prefix + "input sentinel outputs differ from expected somewhere");
}

void CheckFirstWarpAgainstReference(const char* label, u32 depth) {
    const TensorBench::Fixture fixture = TensorBench::MakeFixture(SampleParameters(depth, 32), 1, 0);
    const std::string prefix = std::string(label) + " depth " + std::to_string(depth) + ": ";
    for (u32 lane = 0; lane < 32; ++lane) {
        // Input words of the first warp must be exactly the measured sample matrices.
        for (u32 fragment = 0; fragment < depth / 4; ++fragment)
            Check(FixtureWord(fixture, lane, fragment) == SampleAWord(lane, fragment),
                  prefix + "A word lane " + std::to_string(lane) + " fragment " +
                      std::to_string(fragment) + " encodes the reference sample");
        for (u32 fragment = 0; fragment < depth / 8; ++fragment)
            Check(FixtureWord(fixture, lane, 4 + fragment) == SampleBWord(lane, fragment),
                  prefix + "B word lane " + std::to_string(lane) + " fragment " +
                      std::to_string(fragment) + " encodes the reference sample");
        for (u32 fragment = 0; fragment < 2; ++fragment)
            Check(FixtureWord(fixture, lane, 6 + fragment) == SampleCWord(lane, fragment),
                  prefix + "C word lane " + std::to_string(lane) + " fragment " +
                      std::to_string(fragment) + " encodes the reference sample");
        // Expected outputs must be the measured hardware words themselves.
        for (u32 fragment = 0; fragment < 2; ++fragment)
            Check(ExpectedWord(fixture, lane, 8 + fragment) ==
                      Hardware[depth / 8 - 1][lane * 2 + fragment],
                  prefix + "expected lane " + std::to_string(lane) + " fragment " +
                      std::to_string(fragment) + " matches the measured hardware word");
    }
}

void CheckOracle(const char* label, u32 depth, u32 local_size, u32 workgroups, u32 seed = 0,
                 u32 operations = 1,
                 TensorBench::Pattern pattern = TensorBench::Pattern::Fractions) {
    TensorBench::Parameters parameters = SampleParameters(depth, local_size);
    parameters.operations = operations;
    parameters.pattern = pattern;
    const TensorBench::Fixture fixture = TensorBench::MakeFixture(parameters, workgroups, seed);
    const u32 warps = workgroups * local_size / InvocationsPerWarp;
    const std::string prefix =
        std::string(label) + " depth " + std::to_string(depth) + " local " +
        std::to_string(local_size) + " workgroups " + std::to_string(workgroups) + " seed " +
        std::to_string(seed) + " ops " + std::to_string(operations) + ": ";
    // The measured goldens anchor only the sample warp: seed zero, warp zero, one operation,
    // fractions. Every other combination leans on the independent model alone.
    const bool anchor_goldens = seed == 0 && operations == 1 &&
                                pattern == TensorBench::Pattern::Fractions;
    for (u32 warp = 0; warp < warps; ++warp) {
        const u32 warp_base = warp * InvocationsPerWarp;
        for (u32 lane = 0; lane < 32; ++lane) {
            for (u32 fragment = 0; fragment < 2; ++fragment) {
                const u32 oracle =
                    OracleOutWord(fixture, warp_base, lane, fragment, depth, operations);
                Check(ExpectedWord(fixture, warp_base + lane, 8 + fragment) == oracle,
                      prefix + "warp " + std::to_string(warp) + " lane " + std::to_string(lane) +
                          " fragment " + std::to_string(fragment) +
                          " expected equals the independent oracle");
                if (warp == 0 && anchor_goldens) {
                    Check(oracle == Hardware[depth / 8 - 1][lane * 2 + fragment],
                          prefix + "oracle lane " + std::to_string(lane) + " fragment " +
                              std::to_string(fragment) + " reproduces the measured hardware word");
                }
            }
        }
    }
}

void CheckSeedContract() {
    auto first = TensorBench::MakeFixture(SampleParameters(16, 64), 2, 0);
    const auto replay = TensorBench::MakeFixture(SampleParameters(16, 64), 2, 0);
    Check(first.input == replay.input && first.expected == replay.expected,
          "same seed reproduces bit-identical fixtures");
    const auto scrambled = TensorBench::MakeFixture(SampleParameters(16, 64), 2, 1);
    Check(scrambled.input != first.input, "a different seed changes the input data");
    // Each warp must carry its own data rather than a copy of the first warp's fragments; the
    // seed derivation itself stays unconstrained, the vectors just have to differ.
    for (u32 warp = 1; warp < 4; ++warp) {
        const u32 warp_base = warp * InvocationsPerWarp;
        bool distinct = false;
        for (u32 lane = 0; lane < 32 && !distinct; ++lane)
            for (u32 word = 0; word < 8 && !distinct; ++word)
                distinct = FixtureWord(first, warp_base + lane, word) != FixtureWord(first, lane, word);
        Check(distinct, "warp " + std::to_string(warp) + " inputs differ from the first warp");
    }
}

void CheckEquivalentWord() {
    using TensorBench::EquivalentWord;
    Check(EquivalentWord(0x3c003c00U, 0x3c003c00U), "identical finite bits are equivalent");
    Check(EquivalentWord(0x00003c00U, 0x80003c00U), "signed zero is permitted per half");
    Check(EquivalentWord(0x80008000U, 0x00000000U), "signed zero is permitted in both halves");
    Check(EquivalentWord(0x7e007e00U, 0x7e007e00U), "bitwise identical NaNs are equivalent");
    Check(!EquivalentWord(0x3c003c00U, 0x3c013c00U), "unequal finite values are rejected");
    Check(!EquivalentWord(0x3bff3c00U, 0x3c003c00U), "one-ulp neighbours are rejected");
    Check(!EquivalentWord(0xbc003c00U, 0x3c003c00U), "sign flips on finite values are rejected");
    Check(!EquivalentWord(0x00003c00U, 0x00003c01U), "zero is not equivalent to a subnormal");
    Check(!EquivalentWord(0x7c007c00U, 0xfc00fc00U), "opposite infinities are rejected");
    Check(!EquivalentWord(0x7c007c00U, 0x7bff7c00U), "infinity is not equivalent to max finite");
    Check(!EquivalentWord(0x7e007e00U, 0x7e017e00U), "different NaN payloads are rejected");
    Check(!EquivalentWord(0x7e003c00U, 0x3c003c00U), "NaN against a finite value is rejected");
}

void CheckInvalidParameters() {
    for (u32 depth : {0U, 1U, 7U, 12U, 24U, 32U})
        Check(MakeFixtureThrows(SampleParameters(depth, 32), 1),
              "unsupported depth " + std::to_string(depth) + " throws");
    for (u32 local_size : {0U, 16U, 48U, 128U})
        Check(MakeFixtureThrows(SampleParameters(16, local_size), 1),
              "unsupported local size " + std::to_string(local_size) + " throws");
    // Shared ValidateParameters rejects cooperative matrices with the fractions pattern.
    TensorBench::Parameters cooperative_fractions = SampleParameters(16, 32);
    cooperative_fractions.mode = TensorBench::Mode::Cooperative;
    Check(MakeFixtureThrows(cooperative_fractions, 1), "cooperative fractions throw");
    TensorBench::Parameters no_operations = SampleParameters(16, 32);
    no_operations.operations = 0;
    Check(MakeFixtureThrows(no_operations, 1), "zero operations throw");
    TensorBench::Parameters too_many_operations = SampleParameters(16, 32);
    too_many_operations.operations = 65;
    Check(MakeFixtureThrows(too_many_operations, 1), "more than 64 operations throw");
    Check(MakeFixtureThrows(SampleParameters(16, 32), 0), "zero workgroups throw");
    Check(MakeFixtureThrows(SampleParameters(16, 32), 4097), "more than 4096 workgroups throw");
    TensorBench::Parameters max_operations = SampleParameters(16, 32);
    max_operations.operations = 64;
    Check(!MakeFixtureThrows(max_operations, 1), "64 operations are accepted");
    Check(!MakeFixtureThrows(SampleParameters(16, 32), 4096), "4096 workgroups are accepted");
}
} // namespace

int main() {
    try {
        CheckFixtureShape("fractions", 8, 32, 1);
        CheckFixtureShape("fractions", 16, 32, 1);
        CheckFixtureShape("fractions", 8, 64, 2);
        CheckFixtureShape("fractions", 16, 64, 2);
        CheckFirstWarpAgainstReference("fractions", 8);
        CheckFirstWarpAgainstReference("fractions", 16);
        CheckOracle("fractions", 8, 32, 1);
        CheckOracle("fractions", 16, 32, 1);
        CheckOracle("fractions", 8, 64, 2);
        CheckOracle("fractions", 16, 64, 2);
        CheckOracle("fractions", 16, 32, 1, 1);
        // Integer chains: exact {-1,0,1} inputs pin the K8/K16 contract without goldens.
        CheckOracle("integers", 8, 32, 1, 0, 1, TensorBench::Pattern::Integers);
        CheckOracle("integers", 16, 32, 1, 0, 1, TensorBench::Pattern::Integers);
        // Fractional chains: two and three dependent operations.
        CheckOracle("fractions", 16, 32, 1, 0, 2);
        CheckOracle("fractions", 16, 32, 1, 0, 3);
        // Operation ceiling: the 64-op integer chain with the maximal seed.
        CheckOracle("integers", 16, 64, 2, 0xffffffffU, 64, TensorBench::Pattern::Integers);
        CheckSeedContract();
        CheckEquivalentWord();
        CheckInvalidParameters();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: unexpected exception: %s\n", e.what());
        return 2;
    } catch (...) {
        std::fprintf(stderr, "FAIL: unexpected non-standard exception\n");
        return 2;
    }
    if (failure_count != 0) {
        std::fprintf(stderr, "%d of %d checks failed\n", failure_count, check_count);
        return 1;
    }
    std::printf("tensor-bench-reference-tests: %d checks passed\n", check_count);
    return 0;
}
