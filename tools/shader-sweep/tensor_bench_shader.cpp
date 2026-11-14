// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// GPU-free producer of the tensor shader benchmark: one HMMA.F16 program plus its storage
// fixture, per tools/shader-sweep/tensor_bench_shader.h.
//
// The shader side reuses the production lowering of spirv_software_matrix.cpp: the same IR
// program shape (compute stage, storage buffer writes, TensorMulAdd chaining), the same software
// helper per depth and, for Mode::Cooperative, the shared 16x16x16 cooperative path with its
// scratch reservation. Inputs are loaded dynamically from the SSBO so nothing folds at compile
// time, and the invocation address is workgroup_id_x * local_size + local_invocation_x, so every
// warp (32 invocations, vk_cooperative_hmma_test.cpp lane layout) owns independent matrices.
//
// The CPU expected results never touch shader emission: lane fragments are packed exactly as the
// production layout maps them, and D is computed with the contract the hardware was measured to
// follow (src/tests/video_core/ampere_matrix.cpp RTX 4090 samples): widen the halfs to float,
// accumulate ascending k with float32 FMA seeded by C, and round ties-to-even to half after each
// group of eight k, including the K16 midpoint. Exact rational arithmetic against the 64 measured
// words matches both depths bit for bit with this model (final-only rounding misses 25 of the 64
// K16 words).

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

#include "common/common_types.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/object_pool.h"

#include "tensor_bench_shader.h"

namespace TensorBench {
namespace {

using Shader::IR::Block;
using Shader::IR::IREmitter;
using Shader::IR::Inst;
using Shader::IR::U32;
using Shader::IR::Program;
using Shader::ObjectPool;
using Shader::Profile;
using Shader::Stage;

constexpr u32 GuestWarpLanes = 32;
constexpr u32 MaxOperations = 64;
constexpr u32 MaxWorkgroups = 4096;
constexpr u32 SupportedSpirv = 0x00010500;

// ---------------------------------------------------------------------------
// Half encoding
// ---------------------------------------------------------------------------

// The encoding of ampere_matrix.cpp (EncodeTestHalf): ties-to-even nearbyint, saturation from
// 65520 and the exact 2^-24 subnormal grid. Every value the fixture encodes is finite.
u16 EncodeHalf(double value) {
    const u16 sign = std::signbit(value) ? 0x8000 : 0;
    value = std::abs(value);
    if (std::isnan(value)) {
        return 0xfe01; // Deliberately noncanonical, as in the reference.
    }
    if (value >= 65520) {
        return static_cast<u16>(sign | 0x7c00);
    }
    if (value < std::ldexp(1.0, -14)) {
        return static_cast<u16>(sign | static_cast<u16>(std::nearbyint(std::ldexp(value, 24))));
    }
    int exponent{};
    std::frexp(value, &exponent);
    --exponent;
    u32 mantissa = static_cast<u32>(std::nearbyint(std::ldexp(value, 10 - exponent)));
    if (mantissa == 2048) {
        ++exponent;
        mantissa = 1024;
    }
    return static_cast<u16>(sign | static_cast<u16>(((exponent + 15) << 10) | (mantissa - 1024)));
}

// Exact widening of a half to float, including subnormals.
float HalfBitsToFloat(u16 bits) {
    const u32 sign = static_cast<u32>(bits & 0x8000U) << 16;
    const u32 exponent = (bits >> 10) & 0x1fU;
    const u32 mantissa = bits & 0x3ffU;
    u32 fp32_bits{};
    if (exponent == 0) {
        fp32_bits = sign | std::bit_cast<u32>(static_cast<float>(mantissa) * 0x1p-24f);
    } else if (exponent == 0x1fU) {
        fp32_bits = sign | 0x7f800000U | (mantissa != 0 ? 0x400000U : 0U);
    } else {
        fp32_bits = sign | ((exponent + 112U) << 23) | (mantissa << 13);
    }
    return std::bit_cast<float>(fp32_bits);
}

// Ties-to-even fp32 -> fp16, the bit-exact CPU counterpart of the software helper's round_half
// (emit_spirv_tensor.cpp): the magnitude rounds on the half grid with the guard-lsb trick, values
// from the half minimum normal up keep their bit pattern, smaller magnitudes round on the 2^-24
// grid (whose integers up to 1024 are themselves the half subnormal encodings), and overflow
// becomes infinity.
u16 RoundFp32ToFp16Bits(u32 fp32_bits) {
    const u32 sign = fp32_bits & 0x80000000U;
    const u32 magnitude = fp32_bits & 0x7fffffffU;
    if (magnitude >= 0x7f800000U) {
        return static_cast<u16>((sign >> 16) | 0x7c00U);
    }
    const u32 tie = (magnitude >> 13) & 1U;
    const u32 rounded = (magnitude + 0xfffU + tie) & ~0x1fffU;
    if (rounded >= 0x47800000U) {
        return static_cast<u16>((sign >> 16) | 0x7c00U);
    }
    if (magnitude >= 0x38800000U) {
        const u32 exponent = ((rounded >> 23) - 112U) << 10;
        return static_cast<u16>((sign >> 16) | exponent | ((rounded >> 13) & 0x3ffU));
    }
    const float scaled = std::bit_cast<float>(magnitude) * 16777216.0f;
    return static_cast<u16>((sign >> 16) | static_cast<u32>(std::nearbyint(scaled)));
}

// One D element of the HMMA: C seeds the float accumulator, ascending k accumulates with float32
// FMA, and each group of eight k rounds the accumulator to half ties-to-even. K16 therefore
// rounds at its midpoint, as the measured hardware and the production helper do.
u16 ExpectedElement(const std::array<u16, 16>& a_bits, const std::array<u16, 16>& b_bits,
                    u16 c_bits, u32 depth) {
    float accumulator = HalfBitsToFloat(c_bits);
    for (u32 group = 0; group < depth / 8; ++group) {
        for (u32 k = group * 8; k < (group + 1) * 8; ++k) {
            accumulator = std::fma(HalfBitsToFloat(a_bits[k]), HalfBitsToFloat(b_bits[k]),
                                   accumulator);
        }
        accumulator = HalfBitsToFloat(RoundFp32ToFp16Bits(std::bit_cast<u32>(accumulator)));
    }
    return RoundFp32ToFp16Bits(std::bit_cast<u32>(accumulator));
}

u32 PackBits(u16 low, u16 high) {
    return static_cast<u32>(low) | (static_cast<u32>(high) << 16);
}

// ---------------------------------------------------------------------------
// Per-warp matrices
// ---------------------------------------------------------------------------

// One warp's guest matrices as encoded halves: A is row-major over depth k values, B holds k rows
// by the eight n columns, C and D are 16 by 8.
struct WarpBits {
    std::array<std::array<u16, 16>, 16> a{};
    std::array<std::array<u16, 8>, 16> b{};
    std::array<std::array<u16, 8>, 16> c{};
    std::array<std::array<u16, 8>, 16> d{};
};

// The exact fragment matrices of the measured reference (ampere_matrix.cpp): warp zero of seed
// zero encodes them unshifted. Other warps and seeds shift the lookup indices deterministically;
// the coprime 7/11/13 warp multipliers over the prime table sizes 199/211/97 keep every warp of
// any admissible fixture (at most 8192 warps) different from the anchor and, except for
// astronomically distant pairs, from each other.
double FractionA(u32 row, u32 k, u32 shift_a) {
    return (static_cast<int>((row * 17 + k * 5 + shift_a) % 199) - 99) * 0.00731;
}

double FractionB(u32 k, u32 col, u32 shift_b) {
    return (static_cast<int>((k * 7 + col * 3 + shift_b) % 211) - 105) * 0.00917;
}

double FractionC(u32 row, u32 col, u32 shift_c) {
    return (static_cast<int>((row * 5 + col * 3 + shift_c) % 97) - 48) * 0.00319;
}

// Independent deterministic stream per (seed, warp): splitmix64 mixes the pair into a full-period
// LCG state (the generator of vk_cooperative_hmma_test.cpp's MakeMatrices). Entries stay in
// {-1, 0, 1}, so |C + operations*16*A*B| <= 1025 and every intermediate of a chained fixture is
// an integer fp16 represents exactly, whatever accumulation order a driver picks.
u32 IntegerStreamState(u32 seed, u32 warp) {
    u64 state = (u64{seed} + 0x9e3779b97f4a7c15ull) * (u64{warp} + 0xc2b2ae3d27d4eb4full);
    state ^= state >> 30;
    state *= 0xbf58476d1ce4e5b9ull;
    state ^= state >> 27;
    state *= 0x94d049bb133111ebull;
    state ^= state >> 31;
    return static_cast<u32>(state);
}

WarpBits MakeWarpBits(const Parameters& parameters, u32 seed, u32 warp) {
    const u32 depth = parameters.depth;
    WarpBits bits{};
    if (parameters.pattern == Pattern::Integers) {
        u32 state = IntegerStreamState(seed, warp);
        const auto next = [&state] {
            state = state * 1664525U + 1013904223U;
            return static_cast<s32>((state >> 16) % 3) - 1;
        };
        for (u32 row = 0; row < 16; ++row) {
            for (u32 k = 0; k < depth; ++k) {
                bits.a[row][k] = EncodeHalf(static_cast<double>(next()));
            }
        }
        for (u32 k = 0; k < depth; ++k) {
            for (u32 col = 0; col < 8; ++col) {
                bits.b[k][col] = EncodeHalf(static_cast<double>(next()));
            }
        }
        for (u32 row = 0; row < 16; ++row) {
            for (u32 col = 0; col < 8; ++col) {
                bits.c[row][col] = EncodeHalf(static_cast<double>(next()));
            }
        }
    } else {
        const u32 shift_a = (warp * 7 + seed * 29) % 199;
        const u32 shift_b = (warp * 11 + seed * 37) % 211;
        const u32 shift_c = (warp * 13 + seed * 53) % 97;
        for (u32 row = 0; row < 16; ++row) {
            for (u32 k = 0; k < depth; ++k) {
                bits.a[row][k] = EncodeHalf(FractionA(row, k, shift_a));
            }
        }
        for (u32 k = 0; k < depth; ++k) {
            for (u32 col = 0; col < 8; ++col) {
                bits.b[k][col] = EncodeHalf(FractionB(k, col, shift_b));
            }
        }
        for (u32 row = 0; row < 16; ++row) {
            for (u32 col = 0; col < 8; ++col) {
                bits.c[row][col] = EncodeHalf(FractionC(row, col, shift_c));
            }
        }
    }
    // Chained operations keep A and B constant and feed D back as C.
    for (u32 col = 0; col < 8; ++col) {
        std::array<u16, 16> b_column{};
        for (u32 k = 0; k < depth; ++k) {
            b_column[k] = bits.b[k][col];
        }
        for (u32 row = 0; row < 16; ++row) {
            u16 accumulator = bits.c[row][col];
            for (u32 operation = 0; operation < parameters.operations; ++operation) {
                accumulator = ExpectedElement(bits.a[row], b_column, accumulator, depth);
            }
            bits.d[row][col] = accumulator;
        }
    }
    return bits;
}

// The production lane layout (vk_cooperative_hmma_test.cpp's WriteLane): with g = lane / 4 and
// t = lane % 4, the lane holds rows g and g + 8 of A, C and D at column pairs 2t (and 2t + 8 for
// K16), and column g of B at k pairs 2t (and 2t + 8 for K16). Short K8 leaves the upper A and B
// halves zero, exactly as the guest TENSOR_MUL_ADD encoding does (tensor_mul_add.cpp).
std::array<u32, 8> EncodeLane(const WarpBits& bits, u32 lane, u32 depth) {
    const u32 g = lane >> 2;
    const u32 t = lane & 3;
    const u32 k_hi = depth == 16 ? 8U : 0U;
    return {PackBits(bits.a[g][2 * t], bits.a[g][2 * t + 1]),
            PackBits(bits.a[g + 8][2 * t], bits.a[g + 8][2 * t + 1]),
            k_hi != 0 ? PackBits(bits.a[g][2 * t + 8], bits.a[g][2 * t + 9]) : 0U,
            k_hi != 0 ? PackBits(bits.a[g + 8][2 * t + 8], bits.a[g + 8][2 * t + 9]) : 0U,
            PackBits(bits.b[2 * t][g], bits.b[2 * t + 1][g]),
            k_hi != 0 ? PackBits(bits.b[2 * t + 8][g], bits.b[2 * t + 9][g]) : 0U,
            PackBits(bits.c[g][2 * t], bits.c[g][2 * t + 1]),
            PackBits(bits.c[g + 8][2 * t], bits.c[g + 8][2 * t + 1])};
}

void ValidateParameters(const Parameters& parameters) {
    if (parameters.depth != 8 && parameters.depth != 16) {
        throw Shader::InvalidArgument("Invalid tensor bench depth {}", parameters.depth);
    }
    if (parameters.local_size != 32 && parameters.local_size != 64) {
        throw Shader::InvalidArgument("Invalid tensor bench local size {}", parameters.local_size);
    }
    if (parameters.operations < 1 || parameters.operations > MaxOperations) {
        throw Shader::InvalidArgument("Invalid tensor bench operation count {}", parameters.operations);
    }
    // The cooperative path's rounding depends on the driver's internal accumulation, which only
    // exact integers pin down; fractional fixtures have no strict contract there.
    if (parameters.mode == Mode::Cooperative && parameters.pattern == Pattern::Fractions) {
        throw Shader::InvalidArgument(
            "Cooperative tensor bench fixtures require the exact integer pattern");
    }
}

// ---------------------------------------------------------------------------
// Shader emission
// ---------------------------------------------------------------------------

struct TensorProgram {
    ObjectPool<Inst> pool;
    Block block{pool};
    Program program;
    Profile profile;

    explicit TensorProgram(const Parameters& parameters) {
        program.stage = Stage::Compute;
        program.workgroup_size = {parameters.local_size, 1U, 1U};
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, Shader::IR::AbstractSyntaxNode::Type::Block},
                               {{}, Shader::IR::AbstractSyntaxNode::Type::Return}};
        // TensorMulAdd needs the warp helper's lane id, its subgroup shuffles (software) or the
        // per-subgroup scratch (cooperative), and fp32 denorm preservation on accumulation.
        program.info.uses_subgroup_shuffles = true;
        program.info.uses_subgroup_invocation_id = true;
        program.info.uses_fp32_denorms_preserve = true;
        program.info.uses_workgroup_id = true;
        program.info.uses_local_invocation_id = true;
        program.info.used_storage_buffer_types = Shader::IR::Type::U32;
        program.info.storage_buffers_descriptors = {{0, 0, 1, true}};
        profile.supported_spirv = SupportedSpirv;
        profile.supported_subgroup_stages = 1U << static_cast<u32>(Stage::Compute);
        profile.max_subgroup_size = GuestWarpLanes;
        profile.unified_descriptor_binding = true;
        if (parameters.mode == Mode::Cooperative) {
            profile.support_cooperative_hmma_16x16x16 = true;
        }

        IREmitter ir{block};
        const auto invocation = ir.IAdd(ir.IMul(ir.WorkgroupIdX(), ir.Imm32(parameters.local_size)),
                                        ir.LocalInvocationIdX());
        const auto word_base = ir.IMul(invocation, ir.Imm32(WordsPerInvocation));
        const auto byte_base = ir.ShiftLeftLogical(word_base, ir.Imm32(2));
        const auto word_address = [&](u32 word) {
            return ir.IAdd(byte_base, ir.Imm32(word * 4U));
        };
        std::array<U32, 8> inputs{};
        for (u32 word = 0; word < inputs.size(); ++word) {
            Inst& load = *block.PrependNewInst(block.end(), Shader::IR::Opcode::LoadStorage32,
                                               {ir.Imm32(0), word_address(word)});
            inputs[word] = U32{&load};
        }
        // Production reads no guest registers beyond the depth: short K8 passes zero upper A and
        // B halves (tensor_mul_add.cpp), which also pins the shared 16x16x16 cooperative path to
        // K8 semantics even though its tiles cover K16.
        const U32 zero = ir.Imm32(0);
        const bool short_depth = parameters.depth == 8;
        const U32 a0 = inputs[0];
        const U32 a1 = inputs[1];
        const U32 a2 = short_depth ? zero : inputs[2];
        const U32 a3 = short_depth ? zero : inputs[3];
        const U32 b0 = inputs[4];
        const U32 b1 = short_depth ? zero : inputs[5];
        U32 c0 = inputs[6];
        U32 c1 = inputs[7];
        for (u32 operation = 0; operation < parameters.operations; ++operation) {
            const auto result = ir.TensorMulAdd(ir.CompositeConstruct(a0, a1, a2, a3),
                                                ir.CompositeConstruct(b0, b1),
                                                ir.CompositeConstruct(c0, c1), parameters.depth);
            c0 = U32{ir.CompositeExtract(result, 0)};
            c1 = U32{ir.CompositeExtract(result, 1)};
        }
        block.AppendNewInst(Shader::IR::Opcode::WriteStorage32, {ir.Imm32(0), word_address(8), c0});
        block.AppendNewInst(Shader::IR::Opcode::WriteStorage32, {ir.Imm32(0), word_address(9), c1});
    }

    ShaderModule Emit() {
        // ReserveCooperativeHmmaScratch runs inside EmitSPIRV and sizes shared memory for one
        // 16x16x16 tile triple (384 words) per 32-lane subgroup of the workgroup.
        auto words = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
        return ShaderModule{std::move(words), program.shared_memory_size};
    }
};

} // Anonymous namespace

ShaderModule EmitShader(const Parameters& parameters) {
    ValidateParameters(parameters);
    TensorProgram tensor_program{parameters};
    return tensor_program.Emit();
}

Fixture MakeFixture(const Parameters& parameters, u32 workgroups, u32 seed) {
    ValidateParameters(parameters);
    if (workgroups == 0 || workgroups > MaxWorkgroups) {
        throw Shader::InvalidArgument("Invalid tensor bench workgroup count {}", workgroups);
    }
    const u32 invocations = workgroups * parameters.local_size;
    Fixture fixture;
    fixture.input.resize(static_cast<size_t>(invocations) * WordsPerInvocation);
    fixture.expected.resize(fixture.input.size());
    const u32 warps = invocations / GuestWarpLanes;
    for (u32 warp = 0; warp < warps; ++warp) {
        const WarpBits bits = MakeWarpBits(parameters, seed, warp);
        for (u32 lane = 0; lane < GuestWarpLanes; ++lane) {
            const u32 invocation = warp * GuestWarpLanes + lane;
            u32* const input = fixture.input.data() +
                               static_cast<size_t>(invocation) * WordsPerInvocation;
            u32* const expected = fixture.expected.data() +
                                  static_cast<size_t>(invocation) * WordsPerInvocation;
            const std::array<u32, 8> words = EncodeLane(bits, lane, parameters.depth);
            for (u32 word = 0; word < words.size(); ++word) {
                input[word] = words[word];
                expected[word] = words[word];
            }
            const u32 g = lane >> 2;
            const u32 t = lane & 3;
            expected[8] = PackBits(bits.d[g][2 * t], bits.d[g][2 * t + 1]);
            expected[9] = PackBits(bits.d[g + 8][2 * t], bits.d[g + 8][2 * t + 1]);
        }
    }
    return fixture;
}

bool EquivalentWord(u32 actual, u32 expected) {
    const auto same_half = [](u32 lhs, u32 rhs) {
        // Exact packed halves; a sum that cancelled to zero may carry either sign.
        return lhs == rhs || ((lhs & 0x7fffU) == 0 && (rhs & 0x7fffU) == 0);
    };
    return same_half(actual & 0xffffU, expected & 0xffffU) &&
           same_half(actual >> 16, expected >> 16);
}

} // namespace TensorBench
