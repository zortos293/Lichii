// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>
#include <random>

#include <catch2/catch_test_macros.hpp>

#include "video_core/host1x/vic_geometry.h"
#include "video_core/textures/decoders.h"

using Tegra::Host1x::VicGeometry::BlendInput;
using Tegra::Host1x::VicGeometry::BlendRects;
using Tegra::Host1x::VicGeometry::FitBlend;

namespace {

/// Blend's rectangles as base computed them, before any clamp to the slot surface.
BlendRects BaseRects(const BlendInput& in) {
    BlendRects r{};
    r.rect_left = (std::max)(in.target_left, in.dest_left);
    r.rect_right = (std::min)(in.target_right, in.dest_right);
    r.source_left = (std::max)(in.source_left, r.rect_left);
    r.source_right = (std::min)({in.source_right, r.rect_right, in.out_width});
    r.source_top = (std::max)({in.source_top, in.target_top, in.dest_top});
    r.source_bottom =
        (std::min)({in.source_bottom, in.target_bottom, in.dest_bottom, in.out_height});
    return r;
}

BlendInput Whole(u32 width, u32 height, u32 slot_width, u32 slot_rows) {
    return {.source_left = 0, .source_right = width, .source_top = 0, .source_bottom = height,
            .dest_left = 0, .dest_right = width, .dest_top = 0, .dest_bottom = height,
            .target_left = 0, .target_right = width, .target_top = 0, .target_bottom = height,
            .out_width = width, .out_height = height,
            .slot_width = slot_width, .slot_rows = slot_rows};
}

} // namespace

TEST_CASE("Vic draws a slot that fits its surfaces as it always has",
          "[video_core][vic_geometry]") {
    const auto fit = FitBlend(Whole(1280, 720, 1280, 720));
    REQUIRE(fit.has_value());
    CHECK(fit->source_left == 0);
    CHECK(fit->source_right == 1280);
    CHECK(fit->source_top == 0);
    CHECK(fit->source_bottom == 720);
    CHECK(fit->matrix_right == 1280);
}

TEST_CASE("Vic draws the part of a slot that overlaps a smaller slot surface",
          "[video_core][vic_geometry]") {
    // A 1920x1080 output from a 1280x720 slot: base copied 1:1 and read past the slot surface.
    // The overlapping part is drawn, nothing outside the slot surface is read.
    const auto fit = FitBlend(Whole(1920, 1080, 1280, 720));
    REQUIRE(fit.has_value());
    CHECK(fit->source_right == 1280);
    CHECK(fit->source_bottom == 720);
}

TEST_CASE("Vic draws nothing for an empty or inverted slot", "[video_core][vic_geometry]") {
    auto inverted = Whole(640, 480, 640, 480);
    inverted.source_left = 500;
    inverted.source_right = 100;
    CHECK_FALSE(FitBlend(inverted).has_value());
    auto past = Whole(640, 480, 320, 240);
    past.source_left = 400; // Left edge past the slot surface's right edge.
    past.target_left = 400;
    past.dest_left = 400;
    CHECK_FALSE(FitBlend(past).has_value());
    auto empty = Whole(640, 480, 640, 480);
    empty.source_top = empty.source_bottom = 10;
    CHECK_FALSE(FitBlend(empty).has_value());
}

TEST_CASE("Every fitted slot stays within both surfaces on both blend paths",
          "[video_core][vic_geometry]") {
    std::mt19937 rng{20261126};
    const auto below = [&](u32 limit) { return static_cast<u32>(rng() % limit); };
    u32 drawn = 0;
    u32 compared = 0;
    u32 matrix_compared = 0;
    u32 refused = 0;
    constexpr int Configurations = 200'000;
    for (int i = 0; i < Configurations; ++i) {
        const u32 out_w = 1 + below(2048), out_h = 1 + below(2048);
        const u32 slot_w = 1 + below(2048), slot_rows = 1 + below(2048);
        // Three in four configurations are a video's: destination and target near the origin, the
        // source about the slot's size. The others let every edge range over its surface.
        // Each of the inversion classes is generated, and 1 in 8 sources take the 30-bit fields'
        // large values.
        const bool video = below(4) != 0;
        const auto edges = [&](u32& lo, u32& hi, u32 size) {
            if (video) {
                lo = below(16);
                hi = lo + below(size + size / 4 + 1);
            } else {
                lo = below(size + 1);
                hi = lo + below(size - lo + size / 4 + 1);
            }
        };
        BlendInput in{.out_width = out_w, .out_height = out_h, .slot_width = slot_w,
                      .slot_rows = slot_rows};
        edges(in.dest_left, in.dest_right, out_w);
        edges(in.dest_top, in.dest_bottom, out_h);
        edges(in.target_left, in.target_right, out_w);
        edges(in.target_top, in.target_bottom, out_h);
        edges(in.source_left, in.source_right, slot_w);
        edges(in.source_top, in.source_bottom, slot_rows);
        if (below(8) == 0) {
            in.source_right = below(1U << 30);
            in.source_bottom = below(1U << 30);
        }
        // One rectangle's one axis inverted, in 1 of 4 configurations.
        switch (below(24)) {
        case 0: std::swap(in.source_left, in.source_right); break;
        case 1: std::swap(in.source_top, in.source_bottom); break;
        case 2: std::swap(in.dest_left, in.dest_right); break;
        case 3: std::swap(in.dest_top, in.dest_bottom); break;
        case 4: std::swap(in.target_left, in.target_right); break;
        case 5: std::swap(in.target_top, in.target_bottom); break;
        default: break;
        }
        const auto fit = FitBlend(in);
        const auto base = BaseRects(in);
        if (!fit) {
            // Nothing drawn only where base's rectangle, kept to the slot surface, is empty or
            // inverted.
            ++refused;
            const u32 right = (std::min)(base.source_right, slot_w);
            const u32 bottom = (std::min)(base.source_bottom, slot_rows);
            REQUIRE((base.source_left >= right || base.source_top >= bottom));
            continue;
        }
        ++drawn;
        const auto& r = *fit;
        REQUIRE(r.source_left < r.source_right);
        REQUIRE(r.source_top < r.source_bottom);
        // Reads of the slot surface, rows and columns.
        REQUIRE(r.source_right <= slot_w);
        REQUIRE(r.source_bottom <= slot_rows);
        REQUIRE(r.source_bottom <= out_h);
        // The copy path writes min(source width, target width) pixels from rect_left.
        const u32 copy = (std::min)(r.source_right - r.source_left, r.rect_right - r.rect_left);
        REQUIRE(u64{r.rect_left} + copy <= out_w);
        // The colour-matrix path reads at source_left + x and writes at rect_left + x, x from
        // source_left below matrix_right.
        if (r.source_left < r.matrix_right) {
            REQUIRE(u64{r.source_left} + r.matrix_right <= slot_w);
            REQUIRE(u64{r.rect_left} + r.matrix_right <= out_w);
        }
        // Where base drew within the slot surface, the slot is drawn exactly as before, and the
        // colour-matrix path ends where base's did where base's stayed within both rows.
        if (base.source_right <= slot_w && base.source_bottom <= slot_rows) {
            REQUIRE(r.source_left == base.source_left);
            REQUIRE(r.source_right == base.source_right);
            REQUIRE(r.source_top == base.source_top);
            REQUIRE(r.source_bottom == base.source_bottom);
            REQUIRE(r.rect_left == base.rect_left);
            REQUIRE(r.rect_right == base.rect_right);
            ++compared;
            if (u64{base.source_left} + base.source_right <= slot_w &&
                u64{base.rect_left} + base.source_right <= out_w) {
                REQUIRE(r.matrix_right == base.source_right);
                ++matrix_compared;
            }
        }
    }
    // Most configurations are drawn, and many are compared with base on both paths.
    CHECK(drawn > Configurations / 2);
    CHECK(compared > Configurations / 4);
    CHECK(matrix_compared > Configurations / 10);
    CHECK(refused > 0);
}

namespace {

/// Where byte x of row y of a pitch plane lands in a block-linear one, from the GOB layout
/// itself (64 bytes by 8 rows, blocks of 2^block_height GOBs a column), apart from any
/// implementation the swizzle uses.
size_t BlockLinearOffset(u32 x, u32 y, u32 row_bytes, u32 block_height) {
    const size_t gobs_a_row = (size_t{row_bytes} + 63) / 64;
    const size_t gobs_a_block = size_t{1} << block_height;
    const size_t block = y / (8 * gobs_a_block);
    const size_t gob_in_block = (y / 8) % gobs_a_block;
    const size_t gob = (block * gobs_a_row + x / 64) * gobs_a_block + gob_in_block;
    const size_t in_gob = ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 +
                          (y % 2) * 16 + x % 16;
    return gob * 512 + in_gob;
}

/// The swizzle of `rows` rows of row_bytes, checked against BlockLinearOffset byte by byte, and
/// that nothing is written past the plane's block-linear size.
void CheckSwizzle(u32 row_bytes, u32 rows, u32 block_height) {
    using Tegra::Host1x::VicGeometry::BlockLinearSize;
    using Tegra::Host1x::VicGeometry::SwizzleBlockLinear;
    constexpr u8 Guard = 0xA5;
    constexpr size_t GuardBytes = 4096;
    const size_t size = BlockLinearSize(row_bytes, rows, block_height);
    REQUIRE(size == Tegra::Texture::CalculateSize(true, 1, row_bytes, rows, 1, block_height, 0));
    // A hash of the byte's index: no two rows alike, so a swizzle that moves whole rows or
    // blocks (rows 16 apart swapped, rows reversed) is caught.
    std::vector<u8> input(size_t{row_bytes} * rows);
    for (size_t i = 0; i < input.size(); ++i) {
        u64 h = (i + 1) * 0x9E3779B97F4A7C15ULL;
        h ^= h >> 29;
        h *= 0xBF58476D1CE4E5B9ULL;
        input[i] = static_cast<u8>(h >> 56);
    }
    std::vector<u8> output(size + GuardBytes, Guard);
    REQUIRE(SwizzleBlockLinear(std::span(output).first(size), input, row_bytes, rows,
                               block_height));
    INFO("block height " << block_height << ", row bytes " << row_bytes << ", rows " << rows);
    size_t outside = 0;
    size_t wrong = 0;
    for (u32 y = 0; y < rows; ++y) {
        for (u32 x = 0; x < row_bytes; ++x) {
            const size_t at = BlockLinearOffset(x, y, row_bytes, block_height);
            if (at >= size) {
                ++outside;
            } else if (output[at] != input[size_t{y} * row_bytes + x]) {
                ++wrong;
            }
        }
    }
    REQUIRE(outside == 0);
    REQUIRE(wrong == 0);
    REQUIRE(std::all_of(output.begin() + size, output.end(),
                        [](u8 byte) { return byte == Guard; }));
}

} // namespace

TEST_CASE("Vic's block-linear swizzle puts every byte where the GOB layout says",
          "[video_core][vic_geometry]") {
    using Tegra::Host1x::VicGeometry::MaxBlockHeight;
    // 6 block heights x 32 row sizes x 48 heights = 9,216 planes, the fast path's included.
    for (u32 block_height = 0; block_height <= MaxBlockHeight; ++block_height) {
        for (u32 row_bytes = 16; row_bytes <= 512; row_bytes += 16) {
            for (u32 rows = 1; rows <= 48; ++rows) {
                CheckSwizzle(row_bytes, rows, block_height);
            }
        }
    }
}

TEST_CASE("Vic's block-linear swizzle handles the planes its writers pass",
          "[video_core][vic_geometry]") {
    // What WriteY8__V8U8_N420 and WriteABGR pass: each plane's 16-byte-aligned stride as its row
    // bytes, the chroma plane with two bytes a pixel, the ABGR plane with four.
    const auto stride = [](u32 bytes) { return (bytes + 15) & ~15U; };
    for (const auto& [width, height] : {std::pair{1280U, 720U}, {854U, 480U}, {641U, 361U},
                                        {100U, 37U}}) {
        for (u32 block_height = 0; block_height <= 4; ++block_height) {
            CheckSwizzle(stride(width), height, block_height);                 // Y8 luma
            CheckSwizzle(stride((width + 1) / 2 * 2), (height + 1) / 2, block_height); // V8U8
            CheckSwizzle(stride(width * 4), height, block_height);             // ABGR
        }
    }
}

TEST_CASE("Vic's block-linear swizzle refuses a block height or spans it cannot use",
          "[video_core][vic_geometry]") {
    using Tegra::Host1x::VicGeometry::BlockLinearSize;
    using Tegra::Host1x::VicGeometry::MaxBlockHeight;
    using Tegra::Host1x::VicGeometry::SwizzleBlockLinear;
    std::vector<u8> input(64 * 16, 1);
    std::vector<u8> output(BlockLinearSize(64, 16, MaxBlockHeight), 0);
    CHECK_FALSE(SwizzleBlockLinear(output, input, 64, 16, MaxBlockHeight + 1));
    CHECK_FALSE(SwizzleBlockLinear(std::span(output).first(output.size() - 1), input, 64, 16,
                                   MaxBlockHeight));
    CHECK_FALSE(SwizzleBlockLinear(output, std::span(input).first(input.size() - 1), 64, 16,
                                   MaxBlockHeight));
    CHECK(std::all_of(output.begin(), output.end(), [](u8 byte) { return byte == 0; }));
}
