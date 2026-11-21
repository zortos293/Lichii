// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace Dynarmic::Backend::X64 {

/// A field of an x86-64 instruction whose value depends on where the code is, or which can
/// hold a host address.
enum class X64FieldKind : std::uint8_t {
    None,
    Rel8,       ///< jcc/jmp/loop rel8: target = instruction end + disp8
    Rel32,      ///< call/jmp/jcc rel32: target = instruction end + disp32
    RipDisp32,  ///< [rip + disp32] memory operand: target = instruction end + disp32
    Imm64,      ///< mov r64, imm64 (REX.W B8+r)
};

struct X64Instruction {
    std::uint8_t length = 0;
    X64FieldKind field = X64FieldKind::None;
    std::uint8_t field_offset = 0;  ///< offset of the field from the instruction start
};

/// Decodes the length of the instruction at `code` (at most `size` bytes) and the one field of
/// it that depends on the code address or can hold an address (an instruction has at most one
/// of these). Covers the encodings Xbyak generates for 64-bit code: legacy and 0F/0F38/0F3A
/// maps with any prefixes, VEX and EVEX. nullopt for bytes it does not know or that run past
/// `size`: the caller must then treat the code as not relocatable.
std::optional<X64Instruction> DecodeX64Instruction(const std::uint8_t* code, size_t size) noexcept;

}  // namespace Dynarmic::Backend::X64
