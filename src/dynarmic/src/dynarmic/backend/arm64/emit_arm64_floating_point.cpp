// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <oaknut/feature_detection/feature_detection.hpp>
#include <oaknut/oaknut.hpp>

#include "dynarmic/backend/arm64/a32_jitstate.h"
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/emit_context.h"
#include "dynarmic/backend/arm64/fpsr_manager.h"
#include "dynarmic/backend/arm64/reg_alloc.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Arm64 {

using namespace oaknut::util;

namespace {

/// Half-precision data processing (FEAT_FP16, ARMv8.2): oaknut has no scalar H forms of these,
/// so they are emitted from their encodings. Without the feature they stay unimplemented.
bool HostHasFP16() {
    static const bool has_fp16 = oaknut::detect_features().has(oaknut::CpuFeature::FP16);
    return has_fp16;
}

void RequireFP16() {
    if (!HostHasFP16()) {
        UNREACHABLE();
    }
}

std::uint32_t Rd(int index) {
    return static_cast<std::uint32_t>(index);
}

std::uint32_t Rn(int index) {
    return static_cast<std::uint32_t>(index) << 5;
}

/// FRINT<mode> Hd, Hn
void EmitFRINTH(oaknut::CodeGenerator& code, std::uint32_t base, oaknut::HReg Hd, oaknut::HReg Hn) {
    code.dw(base | Rn(Hn.index()) | Rd(Hd.index()));
}

}  // namespace

template<std::size_t bitsize, typename EmitFn>
static void EmitTwoOp(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst, EmitFn emit) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Vresult = ctx.reg_alloc.WriteVec<bitsize>(inst);
    auto Voperand = ctx.reg_alloc.ReadVec<bitsize>(args[0]);
    RegAlloc::Realize(Vresult, Voperand);
    ctx.fpsr.Load();

    emit(Vresult, Voperand);
}

template<std::size_t bitsize, typename EmitFn>
static void EmitThreeOp(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst, EmitFn emit) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Vresult = ctx.reg_alloc.WriteVec<bitsize>(inst);
    auto Va = ctx.reg_alloc.ReadVec<bitsize>(args[0]);
    auto Vb = ctx.reg_alloc.ReadVec<bitsize>(args[1]);
    RegAlloc::Realize(Vresult, Va, Vb);
    ctx.fpsr.Load();

    emit(Vresult, Va, Vb);
}

template<std::size_t bitsize, typename EmitFn>
static void EmitFourOp(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst, EmitFn emit) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Vresult = ctx.reg_alloc.WriteVec<bitsize>(inst);
    auto Va = ctx.reg_alloc.ReadVec<bitsize>(args[0]);
    auto Vb = ctx.reg_alloc.ReadVec<bitsize>(args[1]);
    auto Vc = ctx.reg_alloc.ReadVec<bitsize>(args[2]);
    RegAlloc::Realize(Vresult, Va, Vb, Vc);
    ctx.fpsr.Load();

    emit(Vresult, Va, Vb, Vc);
}

template<std::size_t bitsize_from, std::size_t bitsize_to, typename EmitFn>
static void EmitConvert(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst, EmitFn emit) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Vto = ctx.reg_alloc.WriteVec<bitsize_to>(inst);
    auto Vfrom = ctx.reg_alloc.ReadVec<bitsize_from>(args[0]);
    const auto rounding_mode = static_cast<FP::RoundingMode>(args[1].GetImmediateU8());
    RegAlloc::Realize(Vto, Vfrom);
    ctx.fpsr.Load();

    ASSERT(rounding_mode == ctx.FPCR().RMode());

    emit(Vto, Vfrom);
}

template<std::size_t bitsize_from, std::size_t bitsize_to, bool is_signed>
static void EmitToFixed(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Rto = ctx.reg_alloc.WriteReg<std::max<std::size_t>(bitsize_to, 32)>(inst);
    auto Vfrom = ctx.reg_alloc.ReadVec<bitsize_from>(args[0]);
    const std::size_t fbits = args[1].GetImmediateU8();
    const auto rounding_mode = static_cast<FP::RoundingMode>(args[2].GetImmediateU8());
    RegAlloc::Realize(Rto, Vfrom);
    ctx.fpsr.Load();

    if (rounding_mode == FP::RoundingMode::TowardsZero) {
        if constexpr (is_signed) {
            if constexpr (bitsize_to == 16) {
                code.FCVTZS(Rto, Vfrom, fbits + 16);
                code.ASR(Wscratch0, Rto, 31);
                code.ADD(Rto, Rto, Wscratch0, LSR, 16);  // Round towards zero when truncating
                code.LSR(Rto, Rto, 16);
            } else if (fbits) {
                code.FCVTZS(Rto, Vfrom, fbits);
            } else {
                code.FCVTZS(Rto, Vfrom);
            }
        } else {
            if constexpr (bitsize_to == 16) {
                code.FCVTZU(Rto, Vfrom, fbits + 16);
                code.LSR(Rto, Rto, 16);
            } else if (fbits) {
                code.FCVTZU(Rto, Vfrom, fbits);
            } else {
                code.FCVTZU(Rto, Vfrom);
            }
        }
    } else {
        ASSERT(fbits == 0);
        ASSERT(bitsize_to != 16);
        if constexpr (is_signed) {
            switch (rounding_mode) {
            case FP::RoundingMode::ToNearest_TieEven:
                code.FCVTNS(Rto, Vfrom);
                break;
            case FP::RoundingMode::TowardsPlusInfinity:
                code.FCVTPS(Rto, Vfrom);
                break;
            case FP::RoundingMode::TowardsMinusInfinity:
                code.FCVTMS(Rto, Vfrom);
                break;
            case FP::RoundingMode::TowardsZero:
                code.FCVTZS(Rto, Vfrom);
                break;
            case FP::RoundingMode::ToNearest_TieAwayFromZero:
                code.FCVTAS(Rto, Vfrom);
                break;
            case FP::RoundingMode::ToOdd:
                UNREACHABLE();
                break;
            default:
                UNREACHABLE();
            }
        } else {
            switch (rounding_mode) {
            case FP::RoundingMode::ToNearest_TieEven:
                code.FCVTNU(Rto, Vfrom);
                break;
            case FP::RoundingMode::TowardsPlusInfinity:
                code.FCVTPU(Rto, Vfrom);
                break;
            case FP::RoundingMode::TowardsMinusInfinity:
                code.FCVTMU(Rto, Vfrom);
                break;
            case FP::RoundingMode::TowardsZero:
                code.FCVTZU(Rto, Vfrom);
                break;
            case FP::RoundingMode::ToNearest_TieAwayFromZero:
                code.FCVTAU(Rto, Vfrom);
                break;
            case FP::RoundingMode::ToOdd:
                UNREACHABLE();
                break;
            default:
                UNREACHABLE();
            }
        }
    }
}

template<std::size_t bitsize_from, std::size_t bitsize_to, typename EmitFn>
static void EmitFromFixed(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, EmitFn emit) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Vto = ctx.reg_alloc.WriteVec<bitsize_to>(inst);
    auto Rfrom = ctx.reg_alloc.ReadReg<std::max<std::size_t>(bitsize_from, 32)>(args[0]);
    const std::size_t fbits = args[1].GetImmediateU8();
    const auto rounding_mode = static_cast<FP::RoundingMode>(args[2].GetImmediateU8());
    RegAlloc::Realize(Vto, Rfrom);
    ctx.fpsr.Load();

    if (rounding_mode == ctx.FPCR().RMode()) {
        emit(Vto, Rfrom, fbits);
    } else {
        FP::FPCR new_fpcr = ctx.FPCR();
        new_fpcr.RMode(rounding_mode);

        code.MOV(Wscratch0, new_fpcr.Value());
        code.MSR(oaknut::SystemReg::FPCR, Xscratch0);

        emit(Vto, Rfrom, fbits);

        code.MOV(Wscratch0, ctx.FPCR().Value());
        code.MSR(oaknut::SystemReg::FPCR, Xscratch0);
    }
}

template<>
void EmitIR<IR::Opcode::FPAbs16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    // A bit operation (FABS ignores FZ16 and raises nothing): done on the integer value.
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Wresult = ctx.reg_alloc.WriteW(inst);
    auto Woperand = ctx.reg_alloc.ReadW(args[0]);
    RegAlloc::Realize(Wresult, Woperand);
    code.AND(Wresult, Woperand, 0x7FFF);
}

template<>
void EmitIR<IR::Opcode::FPAbs32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitTwoOp<32>(code, ctx, inst, [&](auto& Sresult, auto& Soperand) { code.FABS(Sresult, Soperand); });
}

template<>
void EmitIR<IR::Opcode::FPAbs64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitTwoOp<64>(code, ctx, inst, [&](auto& Dresult, auto& Doperand) { code.FABS(Dresult, Doperand); });
}

template<>
void EmitIR<IR::Opcode::FPAdd32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<32>(code, ctx, inst, [&](auto& Sresult, auto& Sa, auto& Sb) { code.FADD(Sresult, Sa, Sb); });
}

template<>
void EmitIR<IR::Opcode::FPAdd64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<64>(code, ctx, inst, [&](auto& Dresult, auto& Da, auto& Db) { code.FADD(Dresult, Da, Db); });
}

template<std::size_t size>
void EmitCompare(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto flags = ctx.reg_alloc.WriteFlags(inst);
    auto Va = ctx.reg_alloc.ReadVec<size>(args[0]);
    const bool exc_on_qnan = args[2].GetImmediateU1();

    if (args[1].IsImmediate() && args[1].GetImmediateU64() == 0) {
        RegAlloc::Realize(flags, Va);
        ctx.fpsr.Load();

        if (exc_on_qnan) {
            code.FCMPE(Va, 0);
        } else {
            code.FCMP(Va, 0);
        }
    } else {
        auto Vb = ctx.reg_alloc.ReadVec<size>(args[1]);
        RegAlloc::Realize(flags, Va, Vb);
        ctx.fpsr.Load();

        if (exc_on_qnan) {
            code.FCMPE(Va, Vb);
        } else {
            code.FCMP(Va, Vb);
        }
    }
}

template<>
void EmitIR<IR::Opcode::FPCompare32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitCompare<32>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPCompare64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitCompare<64>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPDiv32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<32>(code, ctx, inst, [&](auto& Sresult, auto& Sa, auto& Sb) { code.FDIV(Sresult, Sa, Sb); });
}

template<>
void EmitIR<IR::Opcode::FPDiv64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<64>(code, ctx, inst, [&](auto& Dresult, auto& Da, auto& Db) { code.FDIV(Dresult, Da, Db); });
}

template<>
void EmitIR<IR::Opcode::FPMax32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<32>(code, ctx, inst, [&](auto& Sresult, auto& Sa, auto& Sb) { code.FMAX(Sresult, Sa, Sb); });
}

template<>
void EmitIR<IR::Opcode::FPMax64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<64>(code, ctx, inst, [&](auto& Dresult, auto& Da, auto& Db) { code.FMAX(Dresult, Da, Db); });
}

template<>
void EmitIR<IR::Opcode::FPMaxNumeric32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<32>(code, ctx, inst, [&](auto& Sresult, auto& Sa, auto& Sb) { code.FMAXNM(Sresult, Sa, Sb); });
}

template<>
void EmitIR<IR::Opcode::FPMaxNumeric64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<64>(code, ctx, inst, [&](auto& Dresult, auto& Da, auto& Db) { code.FMAXNM(Dresult, Da, Db); });
}

template<>
void EmitIR<IR::Opcode::FPMin32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<32>(code, ctx, inst, [&](auto& Sresult, auto& Sa, auto& Sb) { code.FMIN(Sresult, Sa, Sb); });
}

template<>
void EmitIR<IR::Opcode::FPMin64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<64>(code, ctx, inst, [&](auto& Dresult, auto& Da, auto& Db) { code.FMIN(Dresult, Da, Db); });
}

template<>
void EmitIR<IR::Opcode::FPMinNumeric32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<32>(code, ctx, inst, [&](auto& Sresult, auto& Sa, auto& Sb) { code.FMINNM(Sresult, Sa, Sb); });
}

template<>
void EmitIR<IR::Opcode::FPMinNumeric64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<64>(code, ctx, inst, [&](auto& Dresult, auto& Da, auto& Db) { code.FMINNM(Dresult, Da, Db); });
}

template<>
void EmitIR<IR::Opcode::FPMul32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<32>(code, ctx, inst, [&](auto& Sresult, auto& Sa, auto& Sb) { code.FMUL(Sresult, Sa, Sb); });
}

template<>
void EmitIR<IR::Opcode::FPMul64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<64>(code, ctx, inst, [&](auto& Dresult, auto& Da, auto& Db) { code.FMUL(Dresult, Da, Db); });
}

template<>
void EmitIR<IR::Opcode::FPMulAdd16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    RequireFP16();
    // FMADD Hd, Hn, Hm, Ha
    EmitFourOp<16>(code, ctx, inst, [&](auto& Hresult, auto& Ha, auto& H1, auto& H2) {
        code.dw(0x1FC00000 | (static_cast<std::uint32_t>(H2->index()) << 16) | (static_cast<std::uint32_t>(Ha->index()) << 10) | Rn(H1->index()) | Rd(Hresult->index()));
    });
}

template<>
void EmitIR<IR::Opcode::FPMulAdd32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitFourOp<32>(code, ctx, inst, [&](auto& Sresult, auto& Sa, auto& S1, auto& S2) { code.FMADD(Sresult, S1, S2, Sa); });
}

template<>
void EmitIR<IR::Opcode::FPMulAdd64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitFourOp<64>(code, ctx, inst, [&](auto& Dresult, auto& Da, auto& D1, auto& D2) { code.FMADD(Dresult, D1, D2, Da); });
}

template<>
void EmitIR<IR::Opcode::FPMulSub16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    RequireFP16();
    // FMSUB Hd, Hn, Hm, Ha
    EmitFourOp<16>(code, ctx, inst, [&](auto& Hresult, auto& Ha, auto& H1, auto& H2) {
        code.dw(0x1FC08000 | (static_cast<std::uint32_t>(H2->index()) << 16) | (static_cast<std::uint32_t>(Ha->index()) << 10) | Rn(H1->index()) | Rd(Hresult->index()));
    });
}

template<>
void EmitIR<IR::Opcode::FPMulSub32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitFourOp<32>(code, ctx, inst, [&](auto& Sresult, auto& Sa, auto& S1, auto& S2) { code.FMSUB(Sresult, S1, S2, Sa); });
}

template<>
void EmitIR<IR::Opcode::FPMulSub64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitFourOp<64>(code, ctx, inst, [&](auto& Dresult, auto& Da, auto& D1, auto& D2) { code.FMSUB(Dresult, D1, D2, Da); });
}

template<>
void EmitIR<IR::Opcode::FPMulX32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<32>(code, ctx, inst, [&](auto& Sresult, auto& Sa, auto& Sb) { code.FMULX(Sresult, Sa, Sb); });
}

template<>
void EmitIR<IR::Opcode::FPMulX64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<64>(code, ctx, inst, [&](auto& Dresult, auto& Da, auto& Db) { code.FMULX(Dresult, Da, Db); });
}

template<>
void EmitIR<IR::Opcode::FPNeg16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Wresult = ctx.reg_alloc.WriteW(inst);
    auto Woperand = ctx.reg_alloc.ReadW(args[0]);
    RegAlloc::Realize(Wresult, Woperand);
    code.EOR(Wresult, Woperand, 0x8000);
}

template<>
void EmitIR<IR::Opcode::FPNeg32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitTwoOp<32>(code, ctx, inst, [&](auto& Sresult, auto& Soperand) { code.FNEG(Sresult, Soperand); });
}

template<>
void EmitIR<IR::Opcode::FPNeg64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitTwoOp<64>(code, ctx, inst, [&](auto& Dresult, auto& Doperand) { code.FNEG(Dresult, Doperand); });
}

template<>
void EmitIR<IR::Opcode::FPRecipEstimate16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    (void)code;
    (void)ctx;
    (void)inst;
    UNREACHABLE();
}

template<>
void EmitIR<IR::Opcode::FPRecipEstimate32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitTwoOp<32>(code, ctx, inst, [&](auto& Sresult, auto& Soperand) { code.FRECPE(Sresult, Soperand); });
}

template<>
void EmitIR<IR::Opcode::FPRecipEstimate64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitTwoOp<64>(code, ctx, inst, [&](auto& Dresult, auto& Doperand) { code.FRECPE(Dresult, Doperand); });
}

template<>
void EmitIR<IR::Opcode::FPRecipExponent16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    (void)code;
    (void)ctx;
    (void)inst;
    UNREACHABLE();
}

template<>
void EmitIR<IR::Opcode::FPRecipExponent32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitTwoOp<32>(code, ctx, inst, [&](auto& Sresult, auto& Soperand) { code.FRECPX(Sresult, Soperand); });
}

template<>
void EmitIR<IR::Opcode::FPRecipExponent64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitTwoOp<64>(code, ctx, inst, [&](auto& Dresult, auto& Doperand) { code.FRECPX(Dresult, Doperand); });
}

template<>
void EmitIR<IR::Opcode::FPRecipStepFused16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    (void)code;
    (void)ctx;
    (void)inst;
    UNREACHABLE();
}

template<>
void EmitIR<IR::Opcode::FPRecipStepFused32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<32>(code, ctx, inst, [&](auto& Sresult, auto& Sa, auto& Sb) { code.FRECPS(Sresult, Sa, Sb); });
}

template<>
void EmitIR<IR::Opcode::FPRecipStepFused64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<64>(code, ctx, inst, [&](auto& Dresult, auto& Da, auto& Db) { code.FRECPS(Dresult, Da, Db); });
}

template<>
void EmitIR<IR::Opcode::FPRoundInt16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    RequireFP16();
    const auto rounding_mode = static_cast<FP::RoundingMode>(inst->GetArg(1).GetU8());
    const bool exact = inst->GetArg(2).GetU1();

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Hresult = ctx.reg_alloc.WriteH(inst);
    auto Hoperand = ctx.reg_alloc.ReadH(args[0]);
    RegAlloc::Realize(Hresult, Hoperand);
    ctx.fpsr.Load();

    if (exact) {
        ASSERT(ctx.FPCR().RMode() == rounding_mode);
        EmitFRINTH(code, 0x1EE74000, Hresult, Hoperand);  // FRINTX
        return;
    }
    switch (rounding_mode) {
    case FP::RoundingMode::ToNearest_TieEven:
        EmitFRINTH(code, 0x1EE44000, Hresult, Hoperand);  // FRINTN
        break;
    case FP::RoundingMode::TowardsPlusInfinity:
        EmitFRINTH(code, 0x1EE4C000, Hresult, Hoperand);  // FRINTP
        break;
    case FP::RoundingMode::TowardsMinusInfinity:
        EmitFRINTH(code, 0x1EE54000, Hresult, Hoperand);  // FRINTM
        break;
    case FP::RoundingMode::TowardsZero:
        EmitFRINTH(code, 0x1EE5C000, Hresult, Hoperand);  // FRINTZ
        break;
    case FP::RoundingMode::ToNearest_TieAwayFromZero:
        EmitFRINTH(code, 0x1EE64000, Hresult, Hoperand);  // FRINTA
        break;
    default:
        UNREACHABLE();
    }
}

template<>
void EmitIR<IR::Opcode::FPRoundInt32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    const auto rounding_mode = static_cast<FP::RoundingMode>(inst->GetArg(1).GetU8());
    const bool exact = inst->GetArg(2).GetU1();

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Sresult = ctx.reg_alloc.WriteS(inst);
    auto Soperand = ctx.reg_alloc.ReadS(args[0]);
    RegAlloc::Realize(Sresult, Soperand);
    ctx.fpsr.Load();

    if (exact) {
        ASSERT(ctx.FPCR().RMode() == rounding_mode);
        code.FRINTX(Sresult, Soperand);
    } else {
        switch (rounding_mode) {
        case FP::RoundingMode::ToNearest_TieEven:
            code.FRINTN(Sresult, Soperand);
            break;
        case FP::RoundingMode::TowardsPlusInfinity:
            code.FRINTP(Sresult, Soperand);
            break;
        case FP::RoundingMode::TowardsMinusInfinity:
            code.FRINTM(Sresult, Soperand);
            break;
        case FP::RoundingMode::TowardsZero:
            code.FRINTZ(Sresult, Soperand);
            break;
        case FP::RoundingMode::ToNearest_TieAwayFromZero:
            code.FRINTA(Sresult, Soperand);
            break;
        default:
            UNREACHABLE();
        }
    }
}

template<>
void EmitIR<IR::Opcode::FPRoundInt64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    const auto rounding_mode = static_cast<FP::RoundingMode>(inst->GetArg(1).GetU8());
    const bool exact = inst->GetArg(2).GetU1();

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Dresult = ctx.reg_alloc.WriteD(inst);
    auto Doperand = ctx.reg_alloc.ReadD(args[0]);
    RegAlloc::Realize(Dresult, Doperand);
    ctx.fpsr.Load();

    if (exact) {
        ASSERT(ctx.FPCR().RMode() == rounding_mode);
        code.FRINTX(Dresult, Doperand);
    } else {
        switch (rounding_mode) {
        case FP::RoundingMode::ToNearest_TieEven:
            code.FRINTN(Dresult, Doperand);
            break;
        case FP::RoundingMode::TowardsPlusInfinity:
            code.FRINTP(Dresult, Doperand);
            break;
        case FP::RoundingMode::TowardsMinusInfinity:
            code.FRINTM(Dresult, Doperand);
            break;
        case FP::RoundingMode::TowardsZero:
            code.FRINTZ(Dresult, Doperand);
            break;
        case FP::RoundingMode::ToNearest_TieAwayFromZero:
            code.FRINTA(Dresult, Doperand);
            break;
        default:
            UNREACHABLE();
        }
    }
}

template<>
void EmitIR<IR::Opcode::FPRSqrtEstimate16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    (void)code;
    (void)ctx;
    (void)inst;
    UNREACHABLE();
}

template<>
void EmitIR<IR::Opcode::FPRSqrtEstimate32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitTwoOp<32>(code, ctx, inst, [&](auto& Sresult, auto& Soperand) { code.FRSQRTE(Sresult, Soperand); });
}

template<>
void EmitIR<IR::Opcode::FPRSqrtEstimate64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitTwoOp<64>(code, ctx, inst, [&](auto& Dresult, auto& Doperand) { code.FRSQRTE(Dresult, Doperand); });
}

template<>
void EmitIR<IR::Opcode::FPRSqrtStepFused16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    (void)code;
    (void)ctx;
    (void)inst;
    UNREACHABLE();
}

template<>
void EmitIR<IR::Opcode::FPRSqrtStepFused32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<32>(code, ctx, inst, [&](auto& Sresult, auto& Sa, auto& Sb) { code.FRSQRTS(Sresult, Sa, Sb); });
}

template<>
void EmitIR<IR::Opcode::FPRSqrtStepFused64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<64>(code, ctx, inst, [&](auto& Dresult, auto& Da, auto& Db) { code.FRSQRTS(Dresult, Da, Db); });
}

template<>
void EmitIR<IR::Opcode::FPSqrt32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitTwoOp<32>(code, ctx, inst, [&](auto& Sresult, auto& Soperand) { code.FSQRT(Sresult, Soperand); });
}

template<>
void EmitIR<IR::Opcode::FPSqrt64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitTwoOp<64>(code, ctx, inst, [&](auto& Dresult, auto& Doperand) { code.FSQRT(Dresult, Doperand); });
}

template<>
void EmitIR<IR::Opcode::FPSub32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<32>(code, ctx, inst, [&](auto& Sresult, auto& Sa, auto& Sb) { code.FSUB(Sresult, Sa, Sb); });
}

template<>
void EmitIR<IR::Opcode::FPSub64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOp<64>(code, ctx, inst, [&](auto& Dresult, auto& Da, auto& Db) { code.FSUB(Dresult, Da, Db); });
}

template<>
void EmitIR<IR::Opcode::FPHalfToDouble>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitConvert<16, 64>(code, ctx, inst, [&](auto& Dto, auto& Hfrom) { code.FCVT(Dto, Hfrom); });
}

template<>
void EmitIR<IR::Opcode::FPHalfToSingle>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitConvert<16, 32>(code, ctx, inst, [&](auto& Sto, auto& Hfrom) { code.FCVT(Sto, Hfrom); });
}

template<>
void EmitIR<IR::Opcode::FPSingleToDouble>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitConvert<32, 64>(code, ctx, inst, [&](auto& Dto, auto& Sfrom) { code.FCVT(Dto, Sfrom); });
}

template<>
void EmitIR<IR::Opcode::FPSingleToHalf>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitConvert<32, 16>(code, ctx, inst, [&](auto& Hto, auto& Sfrom) { code.FCVT(Hto, Sfrom); });
}

template<>
void EmitIR<IR::Opcode::FPDoubleToHalf>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitConvert<64, 16>(code, ctx, inst, [&](auto& Hto, auto& Dfrom) { code.FCVT(Hto, Dfrom); });
}

template<>
void EmitIR<IR::Opcode::FPDoubleToSingle>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    const auto rounding_mode = static_cast<FP::RoundingMode>(inst->GetArg(1).GetU8());

    if (rounding_mode == FP::RoundingMode::ToOdd) {
        auto args = ctx.reg_alloc.GetArgumentInfo(inst);
        auto Sto = ctx.reg_alloc.WriteS(inst);
        auto Dfrom = ctx.reg_alloc.ReadD(args[0]);
        RegAlloc::Realize(Sto, Dfrom);
        ctx.fpsr.Load();

        code.FCVTXN(Sto, Dfrom);

        return;
    }

    EmitConvert<64, 32>(code, ctx, inst, [&](auto& Sto, auto& Dfrom) { code.FCVT(Sto, Dfrom); });
}

template<>
void EmitIR<IR::Opcode::FPDoubleToFixedS16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitToFixed<64, 16, true>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPDoubleToFixedS32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitToFixed<64, 32, true>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPDoubleToFixedS64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    // TODO: Consider fpr source
    EmitToFixed<64, 64, true>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPDoubleToFixedU16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitToFixed<64, 16, false>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPDoubleToFixedU32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitToFixed<64, 32, false>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPDoubleToFixedU64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    // TODO: Consider fpr source
    EmitToFixed<64, 64, false>(code, ctx, inst);
}

/// FCVT<mode><S|U> from a half to a 32 or 64-bit integer, or FCVTZ<S|U> to fixed point.
template<std::size_t bitsize_to, bool is_signed>
void EmitHalfToFixed(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    RequireFP16();
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Rto = ctx.reg_alloc.WriteReg<bitsize_to>(inst);
    auto Hfrom = ctx.reg_alloc.ReadH(args[0]);
    const std::size_t fbits = args[1].GetImmediateU8();
    const auto rounding_mode = static_cast<FP::RoundingMode>(args[2].GetImmediateU8());
    RegAlloc::Realize(Rto, Hfrom);
    ctx.fpsr.Load();

    const std::uint32_t sf = bitsize_to == 64 ? 0x80000000 : 0;
    const std::uint32_t u = is_signed ? 0 : 0x10000;
    const std::uint32_t operands = Rn(Hfrom->index()) | Rd(Rto->index());
    if (fbits != 0) {
        ASSERT(rounding_mode == FP::RoundingMode::TowardsZero);
        // FCVTZ<S|U> (scalar, fixed-point): scale = 64 - fbits
        code.dw(0x1ED80000 | sf | u | (static_cast<std::uint32_t>(64 - fbits) << 10) | operands);
        return;
    }
    std::uint32_t base;
    switch (rounding_mode) {
    case FP::RoundingMode::ToNearest_TieEven:
        base = 0x1EE00000;  // FCVTN<S|U>
        break;
    case FP::RoundingMode::TowardsPlusInfinity:
        base = 0x1EE80000;  // FCVTP<S|U>
        break;
    case FP::RoundingMode::TowardsMinusInfinity:
        base = 0x1EF00000;  // FCVTM<S|U>
        break;
    case FP::RoundingMode::TowardsZero:
        base = 0x1EF80000;  // FCVTZ<S|U>
        break;
    case FP::RoundingMode::ToNearest_TieAwayFromZero:
        base = 0x1EE40000;  // FCVTA<S|U>
        break;
    default:
        UNREACHABLE();
    }
    code.dw(base | sf | u | operands);
}

template<>
void EmitIR<IR::Opcode::FPHalfToFixedS16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    (void)code;
    (void)ctx;
    (void)inst;
    UNREACHABLE();
}

template<>
void EmitIR<IR::Opcode::FPHalfToFixedS32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitHalfToFixed<32, true>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPHalfToFixedS64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitHalfToFixed<64, true>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPHalfToFixedU16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    (void)code;
    (void)ctx;
    (void)inst;
    UNREACHABLE();
}

template<>
void EmitIR<IR::Opcode::FPHalfToFixedU32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitHalfToFixed<32, false>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPHalfToFixedU64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitHalfToFixed<64, false>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPSingleToFixedS16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitToFixed<32, 16, true>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPSingleToFixedS32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    // TODO: Consider fpr source
    EmitToFixed<32, 32, true>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPSingleToFixedS64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitToFixed<32, 64, true>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPSingleToFixedU16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitToFixed<32, 16, false>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPSingleToFixedU32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    // TODO: Consider fpr source
    EmitToFixed<32, 32, false>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPSingleToFixedU64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitToFixed<32, 64, false>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPFixedU16ToSingle>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitFromFixed<16, 32>(code, ctx, inst, [&](auto& Sto, auto& Wfrom, u8 fbits) {
        code.LSL(Wscratch0, Wfrom, 16);
        code.UCVTF(Sto, Wscratch0, fbits + 16);
    });
}

template<>
void EmitIR<IR::Opcode::FPFixedS16ToSingle>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitFromFixed<16, 32>(code, ctx, inst, [&](auto& Sto, auto& Wfrom, u8 fbits) {
        code.LSL(Wscratch0, Wfrom, 16);
        code.SCVTF(Sto, Wscratch0, fbits + 16);
    });
}

template<>
void EmitIR<IR::Opcode::FPFixedU16ToDouble>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitFromFixed<16, 64>(code, ctx, inst, [&](auto& Dto, auto& Wfrom, u8 fbits) {
        code.LSL(Wscratch0, Wfrom, 16);
        code.UCVTF(Dto, Wscratch0, fbits + 16);
    });
}

template<>
void EmitIR<IR::Opcode::FPFixedS16ToDouble>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitFromFixed<16, 64>(code, ctx, inst, [&](auto& Dto, auto& Wfrom, u8 fbits) {
        code.LSL(Wscratch0, Wfrom, 16);
        code.SCVTF(Dto, Wscratch0, fbits + 16);
    });
}

template<>
void EmitIR<IR::Opcode::FPFixedU32ToSingle>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    // TODO: Consider fpr source
    EmitFromFixed<32, 32>(code, ctx, inst, [&](auto& Sto, auto& Wfrom, u8 fbits) { fbits ? code.UCVTF(Sto, Wfrom, fbits) : code.UCVTF(Sto, Wfrom); });
}

template<>
void EmitIR<IR::Opcode::FPFixedS32ToSingle>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    // TODO: Consider fpr source
    EmitFromFixed<32, 32>(code, ctx, inst, [&](auto& Sto, auto& Wfrom, u8 fbits) { fbits ? code.SCVTF(Sto, Wfrom, fbits) : code.SCVTF(Sto, Wfrom); });
}

template<>
void EmitIR<IR::Opcode::FPFixedU32ToDouble>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitFromFixed<32, 64>(code, ctx, inst, [&](auto& Dto, auto& Wfrom, u8 fbits) { fbits ? code.UCVTF(Dto, Wfrom, fbits) : code.UCVTF(Dto, Wfrom); });
}

template<>
void EmitIR<IR::Opcode::FPFixedS32ToDouble>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitFromFixed<32, 64>(code, ctx, inst, [&](auto& Dto, auto& Wfrom, u8 fbits) { fbits ? code.SCVTF(Dto, Wfrom, fbits) : code.SCVTF(Dto, Wfrom); });
}

template<>
void EmitIR<IR::Opcode::FPFixedU64ToDouble>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    // TODO: Consider fpr source
    EmitFromFixed<64, 64>(code, ctx, inst, [&](auto& Dto, auto& Xfrom, u8 fbits) { fbits ? code.UCVTF(Dto, Xfrom, fbits) : code.UCVTF(Dto, Xfrom); });
}

template<>
void EmitIR<IR::Opcode::FPFixedU64ToSingle>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitFromFixed<64, 32>(code, ctx, inst, [&](auto& Sto, auto& Xfrom, u8 fbits) { fbits ? code.UCVTF(Sto, Xfrom, fbits) : code.UCVTF(Sto, Xfrom); });
}

template<>
void EmitIR<IR::Opcode::FPFixedS64ToDouble>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    // TODO: Consider fpr source
    EmitFromFixed<64, 64>(code, ctx, inst, [&](auto& Dto, auto& Xfrom, u8 fbits) { fbits ? code.SCVTF(Dto, Xfrom, fbits) : code.SCVTF(Dto, Xfrom); });
}

template<>
void EmitIR<IR::Opcode::FPFixedS64ToSingle>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitFromFixed<64, 32>(code, ctx, inst, [&](auto& Sto, auto& Xfrom, u8 fbits) { fbits ? code.SCVTF(Sto, Xfrom, fbits) : code.SCVTF(Sto, Xfrom); });
}

}  // namespace Dynarmic::Backend::Arm64
