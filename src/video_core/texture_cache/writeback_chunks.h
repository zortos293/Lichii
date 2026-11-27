// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <bit>
#include <optional>
#include <span>
#include "common/common_types.h"
#include "video_core/textures/decoders.h"

namespace VideoCommon::WritebackChunks {
struct Layout {
    u32 width;
    u32 height;
    u32 bytes_per_pixel;
    u32 block_height;
};
struct Chunk {
    u32 x;
    u32 y;
    u32 width;
    u32 height;
    u64 guest_offset;
    u64 guest_size;
};
// A whole GOB column within one block-height band. This keeps the original GOB layout,
// including the padded rows in the final band, while bounding both transfer and CPU work.
inline std::optional<Chunk> Next(const Layout& layout, u64 cursor, u64 budget) {
    const auto [width, height, bpp, block_height] = layout;
    if (!width || !height || !std::has_single_bit(bpp) || bpp > 16 || block_height > 6) {
        return std::nullopt;
    }
    const u64 row_bytes = u64{width} * bpp;
    if (row_bytes % 64 != 0 || row_bytes > 0xffffffffULL) {
        return std::nullopt;
    }
    const u64 column_bytes = u64{512} << block_height;
    const u64 columns = row_bytes / 64;
    const u64 band_bytes = columns * column_bytes;
    const u64 band_rows = u64{8} << block_height;
    if (budget < column_bytes || cursor % column_bytes != 0) {
        return std::nullopt;
    }
    const u64 y = (cursor / band_bytes) * band_rows;
    if (y >= height) {
        return std::nullopt;
    }
    const u64 column = (cursor % band_bytes) / column_bytes;
    const u64 count = (std::min)(columns - column, budget / column_bytes);
    return Chunk{static_cast<u32>(column * 64 / bpp),
                 static_cast<u32>(y),
                 static_cast<u32>(count * 64 / bpp),
                 static_cast<u32>((std::min)(band_rows, u64{height} - y)),
                 cursor,
                 count * column_bytes};
}
inline bool Swizzle(std::span<u8> guest, std::span<const u8> linear, const Layout& layout,
                    const Chunk& chunk, bool native_tiling) {
    const u64 linear_bytes = u64{chunk.width} * chunk.height * layout.bytes_per_pixel;
    if (guest.size() < chunk.guest_size || linear.size() < linear_bytes || !chunk.width ||
        !chunk.height) {
        return false;
    }
    Tegra::Texture::SwizzleTexture(guest, linear, layout.bytes_per_pixel, chunk.width, chunk.height,
                                   1, layout.block_height, 0, 0, native_tiling);
    return true;
}
} // namespace VideoCommon::WritebackChunks
