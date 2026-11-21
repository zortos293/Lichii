// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dynarmic/backend/x64/x64_decode.h"

namespace Dynarmic::Backend::X64 {

using u8 = std::uint8_t;

namespace {

enum class OpcodeMap { OneByte,
                       Map0F,
                       Map0F38,
                       Map0F3A };

struct OperandLayout {
    bool modrm = false;
    /// Immediate bytes after the ModRM/SIB/displacement.
    u8 imm = 0;
    /// Width of the operand-size dependent immediate (imm16 with 0x66, else imm32) is in imm.
    X64FieldKind field = X64FieldKind::None;  // Rel8/Rel32/Imm64: the immediate is this field
    bool f6_f7 = false;                       // TEST r/m, imm: only with ModRM.reg 0 or 1
    bool valid = true;
};

OperandLayout OneByteLayout(u8 op, bool imm16) {
    const u8 immz = imm16 ? 2 : 4;
    OperandLayout l;
    if (op < 0x40) {
        switch (op & 7) {
        case 0:
        case 1:
        case 2:
        case 3:
            l.modrm = true;
            return l;
        case 4:
            l.imm = 1;
            return l;
        case 5:
            l.imm = immz;
            return l;
        default:
            l.valid = false;  // segment push/pop, BCD ops, prefixes: invalid here
            return l;
        }
    }
    if (op >= 0x50 && op <= 0x5F) {
        return l;
    }
    if (op >= 0x70 && op <= 0x7F) {
        l.imm = 1;
        l.field = X64FieldKind::Rel8;
        return l;
    }
    if (op >= 0x84 && op <= 0x8F) {
        l.modrm = true;
        return l;
    }
    if ((op >= 0x90 && op <= 0x99) || (op >= 0x9B && op <= 0x9F)) {
        return l;
    }
    if (op >= 0xB0 && op <= 0xB7) {
        l.imm = 1;
        return l;
    }
    if (op >= 0xB8 && op <= 0xBF) {
        l.imm = immz;  // REX.W: imm64, handled by the caller
        return l;
    }
    if ((op >= 0xD0 && op <= 0xD3) || (op >= 0xD8 && op <= 0xDF)) {
        l.modrm = true;
        return l;
    }
    switch (op) {
    case 0x63:
        l.modrm = true;
        return l;
    case 0x68:
        l.imm = immz;
        return l;
    case 0x69:
        l.modrm = true;
        l.imm = immz;
        return l;
    case 0x6A:
        l.imm = 1;
        return l;
    case 0x6B:
        l.modrm = true;
        l.imm = 1;
        return l;
    case 0x80:
    case 0x83:
    case 0xC0:
    case 0xC1:
    case 0xC6:
        l.modrm = true;
        l.imm = 1;
        return l;
    case 0x81:
    case 0xC7:
        l.modrm = true;
        l.imm = immz;
        return l;
    case 0xA4:
    case 0xA5:
    case 0xA6:
    case 0xA7:
    case 0xAA:
    case 0xAB:
    case 0xAC:
    case 0xAD:
    case 0xAE:
    case 0xAF:
    case 0xC3:
    case 0xC9:
    case 0xCB:
    case 0xCC:
    case 0xEC:
    case 0xED:
    case 0xEE:
    case 0xEF:
    case 0xF1:
    case 0xF4:
    case 0xF5:
    case 0xF8:
    case 0xF9:
    case 0xFA:
    case 0xFB:
    case 0xFC:
    case 0xFD:
        return l;
    case 0xA8:
    case 0xCD:
    case 0xE4:
    case 0xE5:
    case 0xE6:
    case 0xE7:
        l.imm = 1;
        return l;
    case 0xA9:
        l.imm = immz;
        return l;
    case 0xC2:
    case 0xCA:
        l.imm = 2;
        return l;
    case 0xC8:
        l.imm = 3;
        return l;
    case 0xE0:
    case 0xE1:
    case 0xE2:
    case 0xE3:
    case 0xEB:
        l.imm = 1;
        l.field = X64FieldKind::Rel8;
        return l;
    case 0xE8:
    case 0xE9:
        l.imm = 4;
        l.field = X64FieldKind::Rel32;
        return l;
    case 0xF6:
    case 0xF7:
        l.modrm = true;
        l.imm = op == 0xF6 ? 1 : immz;
        l.f6_f7 = true;
        return l;
    case 0xFE:
    case 0xFF:
        l.modrm = true;
        return l;
    default:
        l.valid = false;  // moffs moves, far calls, string I/O and others Xbyak code does not use
        return l;
    }
}

OperandLayout Map0FLayout(u8 op) {
    OperandLayout l;
    if (op >= 0x80 && op <= 0x8F) {
        l.imm = 4;
        l.field = X64FieldKind::Rel32;
        return l;
    }
    if ((op >= 0x70 && op <= 0x73) || op == 0xA4 || op == 0xAC || op == 0xBA || (op >= 0xC2 && op <= 0xC6 && op != 0xC3)) {
        l.modrm = true;
        l.imm = 1;
        return l;
    }
    switch (op) {
    case 0x04:
    case 0x0A:
    case 0x0C:
    case 0x0F:
    case 0x24:
    case 0x25:
    case 0x26:
    case 0x27:
    case 0x36:
    case 0xA6:
    case 0xA7:
        l.valid = false;
        return l;
    case 0x05:
    case 0x06:
    case 0x07:
    case 0x08:
    case 0x09:
    case 0x0B:
    case 0x0E:
    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33:
    case 0x34:
    case 0x35:
    case 0x37:
    case 0x77:
    case 0xA0:
    case 0xA1:
    case 0xA2:
    case 0xA8:
    case 0xA9:
    case 0xAA:
        return l;
    default:
        break;
    }
    if (op >= 0xC8 && op <= 0xCF) {
        return l;  // bswap
    }
    if (op >= 0x38 && op <= 0x3F) {
        l.valid = false;  // escapes handled by the caller, the rest unused
        return l;
    }
    l.modrm = true;
    return l;
}

}  // namespace

std::optional<X64Instruction> DecodeX64Instruction(const u8* code, size_t size) noexcept {
    constexpr size_t max_length = 15;
    size_t i = 0;
    const auto at = [&](size_t index) -> int {
        return index < size && index < max_length ? int(code[index]) : -1;
    };

    bool operand_size_prefix = false;
    for (;;) {
        const int b = at(i);
        if (b < 0) {
            return std::nullopt;
        }
        if (b == 0x66) {
            operand_size_prefix = true;
        } else if (b != 0x67 && b != 0xF0 && b != 0xF2 && b != 0xF3 && b != 0x2E && b != 0x36 && b != 0x3E && b != 0x26 && b != 0x64 && b != 0x65) {
            break;
        }
        ++i;
    }

    bool rex_w = false;
    int b = at(i);
    if ((b & 0xF0) == 0x40) {
        rex_w = (b & 8) != 0;
        ++i;
        b = at(i);
    }
    if (b < 0) {
        return std::nullopt;
    }

    OpcodeMap map = OpcodeMap::OneByte;
    bool vex = false;
    if (b == 0xC5) {
        // 2-byte VEX: map 0F.
        vex = true;
        map = OpcodeMap::Map0F;
        i += 2;
    } else if (b == 0xC4 || b == 0x62) {
        // 3-byte VEX (map in bits 0-4 of the first payload byte) or EVEX (bits 0-2).
        const int p0 = at(i + 1);
        if (p0 < 0) {
            return std::nullopt;
        }
        const int m = b == 0xC4 ? (p0 & 0x1F) : (p0 & 0x07);
        switch (m) {
        case 1:
            map = OpcodeMap::Map0F;
            break;
        case 2:
            map = OpcodeMap::Map0F38;
            break;
        case 3:
            map = OpcodeMap::Map0F3A;
            break;
        default:
            return std::nullopt;
        }
        vex = true;
        i += b == 0xC4 ? 3 : 4;
    } else if (b == 0x0F) {
        const int b1 = at(i + 1);
        if (b1 == 0x38) {
            map = OpcodeMap::Map0F38;
            i += 2;
        } else if (b1 == 0x3A) {
            map = OpcodeMap::Map0F3A;
            i += 2;
        } else {
            map = OpcodeMap::Map0F;
            i += 1;
        }
    }

    const int opcode_byte = at(i);
    if (opcode_byte < 0) {
        return std::nullopt;
    }
    const u8 op = u8(opcode_byte);
    ++i;

    OperandLayout layout;
    switch (map) {
    case OpcodeMap::OneByte:
        layout = OneByteLayout(op, operand_size_prefix && !rex_w);
        if (op >= 0xB8 && op <= 0xBF && rex_w) {
            layout.imm = 8;
            layout.field = X64FieldKind::Imm64;
        }
        break;
    case OpcodeMap::Map0F:
        if (vex) {
            layout.modrm = op != 0x77;  // vzeroupper/vzeroall have no ModRM
            layout.imm = ((op >= 0x70 && op <= 0x73) || op == 0xC2 || op == 0xC4 || op == 0xC5 || op == 0xC6) ? 1 : 0;
        } else {
            layout = Map0FLayout(op);
        }
        break;
    case OpcodeMap::Map0F38:
        layout.modrm = true;
        break;
    case OpcodeMap::Map0F3A:
        layout.modrm = true;
        layout.imm = 1;
        break;
    }
    if (!layout.valid) {
        return std::nullopt;
    }

    X64Instruction result;
    if (layout.modrm) {
        const int modrm = at(i);
        if (modrm < 0) {
            return std::nullopt;
        }
        ++i;
        const int mod = modrm >> 6;
        const int rm = modrm & 7;
        if (layout.f6_f7 && ((modrm >> 3) & 7) > 1) {
            layout.imm = 0;
        }
        if (mod != 3) {
            size_t disp = 0;
            if (rm == 4) {
                const int sib = at(i);
                if (sib < 0) {
                    return std::nullopt;
                }
                ++i;
                if ((sib & 7) == 5 && mod == 0) {
                    disp = 4;
                }
            }
            if (mod == 0 && rm == 5) {
                result.field = X64FieldKind::RipDisp32;
                result.field_offset = u8(i);
                disp = 4;
            } else if (mod == 1) {
                disp = 1;
            } else if (mod == 2) {
                disp = 4;
            }
            i += disp;
        }
    }
    if (layout.field != X64FieldKind::None) {
        result.field = layout.field;
        result.field_offset = u8(i);
    }
    i += layout.imm;
    if (i > size || i > max_length) {
        return std::nullopt;
    }
    result.length = u8(i);
    return result;
}

}  // namespace Dynarmic::Backend::X64
