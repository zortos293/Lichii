// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace Tegra::Engines {

/// Repeat the low 8/16 bits in a 32-bit scratch word when a CONST_A DMA remap
/// writes elements narrower than 32 bits. The caller copies count * width bytes.
constexpr std::uint32_t ExpandConstA(std::uint32_t value,
                                     std::uint32_t component_size) noexcept {
    switch (component_size) {
    case 1:
        return (value & 0xffU) * 0x01010101U;
    case 2:
        return (value & 0xffffU) * 0x00010001U;
    default:
        return value;
    }
}

} // namespace Tegra::Engines
