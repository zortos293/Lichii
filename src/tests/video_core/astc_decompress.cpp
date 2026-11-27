// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/textures/astc.h"

namespace {

u64 Fnv1a(std::span<const u8> bytes, u64 hash) {
    for (const u8 byte : bytes) {
        hash = (hash ^ byte) * 1099511628211ULL;
    }
    return hash;
}

// FNV-1a of the RGBA output for 4096 pseudo-random 16-byte blocks (seeded std::mt19937_64, which
// is portable), decoded by the implementation before the per-texel interpolation was hoisted.
// Random blocks cover every partition count, endpoint mode (HDR ones too), dual plane and void
// extent blocks, and the invalid ones that decode to the error fill.
u64 DecodedHash(u32 block_width, u32 block_height, bool single_partition) {
    std::mt19937_64 rng(0x4153544300ULL + block_width * 16 + block_height);
    std::vector<u8> blocks(4096 * 16);
    for (u8& byte : blocks) {
        byte = static_cast<u8>(rng());
    }
    if (single_partition) {
        for (size_t i = 1; i < blocks.size(); i += 16) {
            blocks[i] &= static_cast<u8>(~0x18);
        }
    }
    // A 64 block wide image with a cropped right and bottom edge.
    const u32 width = 64 * block_width - 3;
    const u32 height = 64 * block_height - 2;
    std::vector<u8> output(size_t{width} * height * 4);
    Tegra::Texture::ASTC::Decompress(blocks, width, height, 1, block_width, block_height, output);
    return Fnv1a(output, 14695981039346656037ULL);
}

} // Anonymous namespace

#ifdef NDEBUG
// Invalid random blocks hit assert(false) in the decoder, so this runs in release builds only.
TEST_CASE("ASTC decode of random blocks keeps its output", "[astc_decompress]") {
    struct Expected {
        u32 width;
        u32 height;
        bool single_partition;
        u64 hash;
    };
    const Expected expected[] = {
        {4, 4, false, 0x8445f344ef801a80ULL},   {5, 5, false, 0xb7af70c48c9b62f6ULL},
        {6, 6, false, 0xf94acf0d5365f877ULL},   {8, 8, false, 0xb91715e5e839aa57ULL},
        {10, 10, false, 0x3555b88c6606816aULL}, {12, 12, false, 0x015f029fb52f1c32ULL},
        {4, 4, true, 0x36a6abc937db2675ULL},    {6, 5, true, 0xfbf7a0d39d0f9a05ULL},
        {8, 8, true, 0x4cf96dc611e50330ULL},    {12, 10, true, 0x340db8e9b01e551dULL},
    };
    for (const Expected& e : expected) {
        const u64 hash = DecodedHash(e.width, e.height, e.single_partition);
        INFO(e.width << "x" << e.height << " single_partition=" << e.single_partition);
        CHECK(hash == e.hash);
    }
}
#endif
