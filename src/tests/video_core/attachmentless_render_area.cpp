// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/attachmentless_render_area.h"

namespace {
using VideoCommon::AttachmentlessRenderArea;
using VideoCommon::AttachmentlessState;
using VideoCommon::Extent2D;
using VideoCommon::GuestScissor;
using VideoCommon::ScaleScissorCoordinate;

constexpr Extent2D Max{32768, 32768};
constexpr AttachmentlessState Native{Max, false, true, 1, 0};

std::array<GuestScissor, 16> AllScissors(u32 max_x, u32 max_y) {
    std::array<GuestScissor, 16> scissors;
    scissors.fill({true, 0, max_x, 0, max_y});
    return scissors;
}

// GetScissorState's scaling as it was before it moved to ScaleScissorCoordinate, verbatim.
s32 OldScaleUp(s32 value, u32 up_scale, u32 down_shift) {
    if (value == 0) {
        return 0U;
    }
    const s32 upset = value * up_scale;
    s32 acumm = 0;
    if ((up_scale >> down_shift) == 0) {
        acumm = upset % 2;
    }
    const s32 converted_value = (value * up_scale) >> down_shift;
    return value < 0 ? std::min<s32>(converted_value - acumm, -1)
                     : std::max<s32>(converted_value + acumm, 1);
}

constexpr std::array<std::array<u32, 2>, 8> Scales{{
    {1, 0}, {1, 1}, {3, 2}, {3, 1}, {2, 0}, {5, 1}, {3, 0}, {4, 0},
}};
} // Anonymous namespace

TEST_CASE("The scissor scaling is the rasterizer's", "[video_core][texture_cache][render_area]") {
    for (const auto& [up_scale, down_shift] : Scales) {
        for (s32 value = -2048; value <= 2048; ++value) {
            REQUIRE(ScaleScissorCoordinate(value, up_scale, down_shift) ==
                    OldScaleUp(value, up_scale, down_shift));
        }
        for (const s32 value : {32767, 65535, -65535}) {
            REQUIRE(ScaleScissorCoordinate(value, up_scale, down_shift) ==
                    OldScaleUp(value, up_scale, down_shift));
        }
    }
}

TEST_CASE("A pass without attachments is cut to its scissors",
          "[video_core][texture_cache][render_area]") {
    // Ocarina of Time: a 1x1 draw in a 32768x32768 surface clip.
    REQUIRE(AttachmentlessRenderArea(Native, AllScissors(1, 1)) == Extent2D{1, 1});

    // The largest of the scissors given, each dimension on its own, to a power of two.
    auto scissors = AllScissors(16, 16);
    scissors[3] = {true, 0, 640, 0, 8};
    scissors[15] = {true, 100, 120, 200, 360};
    REQUIRE(AttachmentlessRenderArea(Native, scissors) == Extent2D{1024, 512});
    // A scissor that moves a little keeps the size.
    scissors[3].max_x = 1000;
    REQUIRE(AttachmentlessRenderArea(Native, scissors) == Extent2D{1024, 512});

    // Never past the surface clip, never empty.
    const AttachmentlessState small{{300, 128}, false, true, 1, 0};
    REQUIRE(AttachmentlessRenderArea(small, AllScissors(65535, 65535)) == Extent2D{300, 128});
    REQUIRE(AttachmentlessRenderArea(Native, AllScissors(0, 0)) == Extent2D{1, 1});
}

TEST_CASE("A pass without attachments keeps its size when the scissors do not bound it",
          "[video_core][texture_cache][render_area]") {
    // A scissor that is off leaves only the viewport to bound the draw.
    auto scissors = AllScissors(1, 1);
    scissors[9].enable = false;
    REQUIRE(AttachmentlessRenderArea(Native, scissors) == Max);

    // An inverted scissor: the host extent would wrap.
    scissors = AllScissors(1, 1);
    scissors[0] = {true, 10, 5, 0, 1};
    REQUIRE(AttachmentlessRenderArea(Native, scissors) == Max);
    scissors[0] = {true, 0, 1, 10, 5};
    REQUIRE(AttachmentlessRenderArea(Native, scissors) == Max);

    // Without the viewport transform the rasterizer sets every host scissor to the surface clip.
    REQUIRE(AttachmentlessRenderArea({Max, false, false, 1, 0}, AllScissors(1, 1)) == Max);

    // A lower-left origin flips viewports and scissors by the surface clip's height.
    REQUIRE(AttachmentlessRenderArea({Max, true, true, 1, 0}, AllScissors(1, 1)) == Max);

    REQUIRE(AttachmentlessRenderArea(Native, {}) == Max);
}

TEST_CASE("A pass without attachments covers its host scissors at every scale",
          "[video_core][texture_cache][render_area]") {
    for (const auto& [up_scale, down_shift] : Scales) {
        const Extent2D size{(32768 * up_scale) >> down_shift, (32768 * up_scale) >> down_shift};
        const AttachmentlessState state{size, false, true, up_scale, down_shift};
        for (u32 min = 0; min < 70; min += 3) {
            for (u32 max = min; max < 300; max += 7) {
                const std::array<GuestScissor, 1> scissor{{{true, min, max, min, max}}};
                const Extent2D area = AttachmentlessRenderArea(state, scissor);
                // GetScissorState: offset scale_up(min), extent scale_up(max - min).
                const s32 end = OldScaleUp(static_cast<s32>(min), up_scale, down_shift) +
                                OldScaleUp(static_cast<s32>(max - min), up_scale, down_shift);
                REQUIRE(static_cast<s32>(area.width) >= end);
                REQUIRE(static_cast<s32>(area.height) >= end);
                REQUIRE(area.width <= (std::max)(2 * static_cast<u32>(end), 1U));
            }
        }
    }
}

TEST_CASE("A pass without attachments is cut to the largest scissor at a non-native scale",
          "[video_core][texture_cache][render_area]") {
    // 1.5x (3 >> 1): each scissor ends at its scaled offset plus scaled extent.
    const AttachmentlessState state{{49152, 49152}, false, true, 3, 1};
    std::array<GuestScissor, 3> scissors{{
        {true, 0, 1, 0, 1},
        {true, 10, 101, 0, 20},
        {true, 0, 40, 33, 300},
    }};
    const auto end = [](u32 min, u32 max) {
        return static_cast<u32>(ScaleScissorCoordinate(static_cast<s32>(min), 3, 1) +
                                ScaleScissorCoordinate(static_cast<s32>(max - min), 3, 1));
    };
    REQUIRE(end(10, 101) == 151);
    REQUIRE(end(33, 300) == 449);
    REQUIRE(AttachmentlessRenderArea(state, scissors) == Extent2D{256, 512});
}

TEST_CASE("An attachment-less framebuffer only grows while its targets stay",
          "[video_core][texture_cache][render_area]") {
    using VideoCommon::NextAttachmentlessSize;
    constexpr Extent2D one{1, 1};
    constexpr Extent2D wide{512, 2};
    SECTION("Without the cut it is what it always was") {
        for (const Extent2D current : {one, wide, Max}) {
            for (const Extent2D needed : {one, wide, Max}) {
                REQUIRE(NextAttachmentlessSize(false, true, current, Max, needed) == Max);
                REQUIRE(NextAttachmentlessSize(false, false, current, Max, needed) == current);
            }
        }
    }
    SECTION("New targets take what the call needs") {
        REQUIRE(NextAttachmentlessSize(true, true, Max, Max, one) == one);
        // A clear or DrawTexture needs the full clip.
        REQUIRE(NextAttachmentlessSize(true, true, one, Max, Max) == Max);
    }
    SECTION("The same targets grow, never shrink") {
        REQUIRE(NextAttachmentlessSize(true, false, one, Max, one) == one);
        REQUIRE(NextAttachmentlessSize(true, false, one, Max, wide) == wide);
        REQUIRE(NextAttachmentlessSize(true, false, wide, Max, one) == wide);
        REQUIRE(NextAttachmentlessSize(true, false, Extent2D{2, 512}, Max, wide) ==
                Extent2D{512, 512});
        // A widened scissor and then a DrawTexture: the full clip, not the stale cut.
        REQUIRE(NextAttachmentlessSize(true, false, one, Max, Max) == Max);
        // Alternating needs cost one growth.
        Extent2D size = one;
        for (int i = 0; i < 8; ++i) {
            size = NextAttachmentlessSize(true, false, size, Max, i % 2 ? Max : one);
        }
        REQUIRE(size == Max);
    }
}
