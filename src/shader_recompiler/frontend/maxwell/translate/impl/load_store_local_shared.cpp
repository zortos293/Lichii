// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/bit_field.h"
#include "common/common_types.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"

namespace Shader::Maxwell {
namespace {
enum class Size : u64 {
    U8,
    S8,
    U16,
    S16,
    B32,
    B64,
    B128,
};

IR::U32 Offset(TranslatorVisitor& v, u64 insn) {
    union {
        u64 raw;
        BitField<8, 8, IR::Reg> offset_reg;
        BitField<20, 24, u64> absolute_offset;
        BitField<20, 24, s64> relative_offset;
    } const encoding{insn};

    if (encoding.offset_reg == IR::Reg::RZ) {
        return v.ir.Imm32(static_cast<u32>(encoding.absolute_offset));
    } else {
        const s32 relative{static_cast<s32>(encoding.relative_offset.Value())};
        return v.ir.IAdd(v.X(encoding.offset_reg), v.ir.Imm32(relative));
    }
}

std::pair<IR::U32, IR::U32> WordOffset(TranslatorVisitor& v, u64 insn) {
    const IR::U32 offset{Offset(v, insn)};
    if (offset.IsImmediate()) {
        return {v.ir.Imm32(offset.U32() / 4), offset};
    } else {
        return {v.ir.ShiftRightArithmetic(offset, v.ir.Imm32(2)), offset};
    }
}

/// Where a native program's local access at `offset` (bytes in the 16 MiB window) lands in its
/// local array (Environment::LocalArraySize): the declared memory after the stack region, the
/// upper half of the window (the stack, which grows down from its top) onto the stack region.
/// Accesses outside the declared memory and the stack region are out of bounds, also a stack
/// deeper than the region: mapping it onto the region would alias the declared memory.
struct NativeLocal {
    IR::U32 word;   ///< Word index in the local array (the spare word when out of bounds)
    IR::U32 offset; ///< Byte offset in the local array, for sub-word accesses
    IR::U1 in_bounds;
};

NativeLocal MapNativeLocal(TranslatorVisitor& v, const IR::U32& window_offset) {
    constexpr u32 stack_half = Environment::LOCAL_WINDOW_BYTES / 2;
    constexpr u32 stack_bottom = Environment::LOCAL_WINDOW_BYTES - Environment::NATIVE_STACK_BYTES;
    constexpr u32 stack_base = Environment::NATIVE_STACK_BASE;
    constexpr u32 declared_base = Environment::NATIVE_DECLARED_BASE;
    constexpr u32 spare_word = 0;
    const u32 declared = v.env.LocalMemorySize();
    if (window_offset.IsImmediate()) {
        const u32 value = window_offset.U32();
        const bool stack = value >= stack_half;
        const bool in_bounds =
            stack ? value >= stack_bottom && value < Environment::LOCAL_WINDOW_BYTES
                  : value < declared;
        const u32 offset = stack ? value - stack_bottom + stack_base : value + declared_base;
        return {v.ir.Imm32(in_bounds ? offset / 4 : spare_word), v.ir.Imm32(offset),
                v.ir.Imm1(in_bounds)};
    }
    const IR::U1 stack{v.ir.IGreaterThanEqual(window_offset, v.ir.Imm32(stack_half), false)};
    const IR::U1 in_stack{v.ir.LogicalAnd(
        v.ir.IGreaterThanEqual(window_offset, v.ir.Imm32(stack_bottom), false),
        v.ir.ILessThan(window_offset, v.ir.Imm32(Environment::LOCAL_WINDOW_BYTES), false))};
    const IR::U1 in_declared{v.ir.ILessThan(window_offset, v.ir.Imm32(declared), false)};
    const IR::U1 in_bounds{v.ir.Select(stack, in_stack, in_declared)};
    const IR::U32 offset{v.ir.Select(stack,
                                     v.ir.IAdd(window_offset, v.ir.Imm32(stack_base - stack_bottom)),
                                     v.ir.IAdd(window_offset, v.ir.Imm32(declared_base)))};
    const IR::U32 word{v.ir.Select(in_bounds, v.ir.ShiftRightLogical(offset, v.ir.Imm32(2)),
                                   v.ir.Imm32(spare_word))};
    return {word, offset, in_bounds};
}

IR::U32 LoadNativeLocal(TranslatorVisitor& v, const NativeLocal& local) {
    return IR::U32{v.ir.Select(local.in_bounds, v.ir.LoadLocal(local.word), v.ir.Imm32(0))};
}

std::pair<int, bool> GetSize(u64 insn) {
    union {
        u64 raw;
        BitField<48, 3, Size> size;
    } const encoding{insn};

    switch (encoding.size) {
    case Size::U8:
        return {8, false};
    case Size::S8:
        return {8, true};
    case Size::U16:
        return {16, false};
    case Size::S16:
        return {16, true};
    case Size::B32:
        return {32, false};
    case Size::B64:
        return {64, false};
    case Size::B128:
        return {128, false};
    default:
        throw NotImplementedException("Invalid size {}", encoding.size.Value());
    }
}

IR::Reg Reg(u64 insn) {
    union {
        u64 raw;
        BitField<0, 8, IR::Reg> reg;
    } const encoding{insn};

    return encoding.reg;
}

IR::U32 ByteOffset(IR::IREmitter& ir, const IR::U32& offset) {
    return ir.BitwiseAnd(ir.ShiftLeftLogical(offset, ir.Imm32(3)), ir.Imm32(24));
}

IR::U32 ShortOffset(IR::IREmitter& ir, const IR::U32& offset) {
    return ir.BitwiseAnd(ir.ShiftLeftLogical(offset, ir.Imm32(3)), ir.Imm32(16));
}

IR::U32 LoadLocal(TranslatorVisitor& v, const IR::U32& word_offset, const IR::U32& offset) {
    const IR::U32 local_memory_size{v.ir.Imm32(v.env.LocalMemorySize())};
    const IR::U1 in_bounds{v.ir.ILessThan(offset, local_memory_size, false)};
    return IR::U32{v.ir.Select(in_bounds, v.ir.LoadLocal(word_offset), v.ir.Imm32(0))};
}
} // Anonymous namespace

void TranslatorVisitor::LDL(u64 insn) {
    if (env.IsNativeProgram()) {
        const IR::U32 window_offset{Offset(*this, insn)};
        const NativeLocal local{MapNativeLocal(*this, window_offset)};
        const IR::U32 word{LoadNativeLocal(*this, local)};
        const IR::Reg dest{Reg(insn)};
        const auto [bit_size, is_signed]{GetSize(insn)};
        switch (bit_size) {
        case 8:
            X(dest, ir.BitFieldExtract(word, ByteOffset(ir, local.offset), ir.Imm32(8), is_signed));
            return;
        case 16:
            X(dest,
              ir.BitFieldExtract(word, ShortOffset(ir, local.offset), ir.Imm32(16), is_signed));
            return;
        default:
            if (!IR::IsAligned(dest, static_cast<size_t>(bit_size / 32))) {
                throw NotImplementedException("Unaligned destination register {}", dest);
            }
            X(dest, word);
            // Each word is mapped on its own: a wide access may straddle the declared memory's end.
            for (int i = 1; i < bit_size / 32; ++i) {
                const IR::U32 next{ir.IAdd(window_offset, ir.Imm32(i * 4))};
                X(dest + i, LoadNativeLocal(*this, MapNativeLocal(*this, next)));
            }
            return;
        }
    }
    const auto [word_offset, offset]{WordOffset(*this, insn)};
    const IR::U32 word{LoadLocal(*this, word_offset, offset)};
    const IR::Reg dest{Reg(insn)};
    const auto [bit_size, is_signed]{GetSize(insn)};
    switch (bit_size) {
    case 8: {
        const IR::U32 bit{ByteOffset(ir, offset)};
        X(dest, ir.BitFieldExtract(word, bit, ir.Imm32(8), is_signed));
        break;
    }
    case 16: {
        const IR::U32 bit{ShortOffset(ir, offset)};
        X(dest, ir.BitFieldExtract(word, bit, ir.Imm32(16), is_signed));
        break;
    }
    case 32:
    case 64:
    case 128:
        if (!IR::IsAligned(dest, static_cast<size_t>(bit_size / 32))) {
            throw NotImplementedException("Unaligned destination register {}", dest);
        }
        X(dest, word);
        for (int i = 1; i < bit_size / 32; ++i) {
            const IR::U32 sub_word_offset{ir.IAdd(word_offset, ir.Imm32(i))};
            const IR::U32 sub_offset{ir.IAdd(offset, ir.Imm32(i * 4))};
            X(dest + i, LoadLocal(*this, sub_word_offset, sub_offset));
        }
        break;
    }
}

void TranslatorVisitor::LDS(u64 insn) {
    const IR::U32 offset{Offset(*this, insn)};
    const IR::Reg dest{Reg(insn)};
    const auto [bit_size, is_signed]{GetSize(insn)};
    const IR::Value value{ir.LoadShared(bit_size, is_signed, offset)};
    switch (bit_size) {
    case 8:
    case 16:
    case 32:
        X(dest, IR::U32{value});
        break;
    case 64:
    case 128:
        if (!IR::IsAligned(dest, static_cast<size_t>(bit_size / 32))) {
            throw NotImplementedException("Unaligned destination register {}", dest);
        }
        for (int element = 0; element < bit_size / 32; ++element) {
            X(dest + element, IR::U32{ir.CompositeExtract(value, static_cast<size_t>(element))});
        }
        break;
    }
}

void TranslatorVisitor::STL(u64 insn) {
    if (env.IsNativeProgram()) {
        // Out-of-bounds stores go to the spare word, which no load reads.
        const IR::U32 window_offset{Offset(*this, insn)};
        const NativeLocal local{MapNativeLocal(*this, window_offset)};
        const IR::Reg reg{Reg(insn)};
        const IR::U32 src{X(reg)};
        const int bit_size{GetSize(insn).first};
        switch (bit_size) {
        case 8:
        case 16: {
            const IR::U32 bit{bit_size == 8 ? ByteOffset(ir, local.offset)
                                            : ShortOffset(ir, local.offset)};
            ir.WriteLocal(local.word, ir.BitFieldInsert(ir.LoadLocal(local.word), src, bit,
                                                        ir.Imm32(bit_size)));
            return;
        }
        default:
            if (!IR::IsAligned(reg, static_cast<size_t>(bit_size / 32))) {
                throw NotImplementedException("Unaligned source register");
            }
            ir.WriteLocal(local.word, src);
            for (int i = 1; i < bit_size / 32; ++i) {
                const IR::U32 next{ir.IAdd(window_offset, ir.Imm32(i * 4))};
                ir.WriteLocal(MapNativeLocal(*this, next).word, X(reg + i));
            }
            return;
        }
    }
    const auto [word_offset, offset]{WordOffset(*this, insn)};
    if (offset.IsImmediate()) {
        // TODO: Support storing out of bounds at runtime
        if (offset.U32() >= env.LocalMemorySize()) {
            LOG_WARNING(Shader, "Storing local memory at {:#x} with a size of {:#x}, dropping",
                        offset.U32(), env.LocalMemorySize());
            return;
        }
    }
    const IR::Reg reg{Reg(insn)};
    const IR::U32 src{X(reg)};
    const int bit_size{GetSize(insn).first};
    switch (bit_size) {
    case 8: {
        const IR::U32 bit{ByteOffset(ir, offset)};
        const IR::U32 value{ir.BitFieldInsert(ir.LoadLocal(word_offset), src, bit, ir.Imm32(8))};
        ir.WriteLocal(word_offset, value);
        break;
    }
    case 16: {
        const IR::U32 bit{ShortOffset(ir, offset)};
        const IR::U32 value{ir.BitFieldInsert(ir.LoadLocal(word_offset), src, bit, ir.Imm32(16))};
        ir.WriteLocal(word_offset, value);
        break;
    }
    case 32:
    case 64:
    case 128:
        if (!IR::IsAligned(reg, static_cast<size_t>(bit_size / 32))) {
            throw NotImplementedException("Unaligned source register");
        }
        ir.WriteLocal(word_offset, src);
        for (int i = 1; i < bit_size / 32; ++i) {
            ir.WriteLocal(ir.IAdd(word_offset, ir.Imm32(i)), X(reg + i));
        }
        break;
    }
}

void TranslatorVisitor::STS(u64 insn) {
    const IR::U32 offset{Offset(*this, insn)};
    const IR::Reg reg{Reg(insn)};
    const int bit_size{GetSize(insn).first};
    switch (bit_size) {
    case 8:
    case 16:
    case 32:
        ir.WriteShared(bit_size, offset, X(reg));
        break;
    case 64:
        if (!IR::IsAligned(reg, 2)) {
            throw NotImplementedException("Unaligned source register {}", reg);
        }
        ir.WriteShared(64, offset, ir.CompositeConstruct(X(reg), X(reg + 1)));
        break;
    case 128: {
        if (!IR::IsAligned(reg, 2)) {
            throw NotImplementedException("Unaligned source register {}", reg);
        }
        const IR::Value vector{ir.CompositeConstruct(X(reg), X(reg + 1), X(reg + 2), X(reg + 3))};
        ir.WriteShared(128, offset, vector);
        break;
    }
    }
}

} // namespace Shader::Maxwell
