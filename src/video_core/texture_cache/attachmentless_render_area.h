// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <bit>
#include <span>

#include "common/common_types.h"
#include "video_core/texture_cache/types.h"

namespace VideoCommon {

/// A guest scissor coordinate scaled to the host resolution as the Vulkan rasterizer scales the
/// host scissors (GetScissorState): times up_scale, shifted down by down_shift, odd products
/// rounded outward when scaling down, never to 0 from a value that was not. The product and the
/// shift are unsigned, as they always were there.
[[nodiscard]] constexpr s32 ScaleScissorCoordinate(s32 value, u32 up_scale, u32 down_shift) {
    if (value == 0) {
        return 0;
    }
    const u32 product = static_cast<u32>(value) * up_scale;
    s32 accumulated = 0;
    if ((up_scale >> down_shift) == 0) {
        accumulated = static_cast<s32>(product) % 2;
    }
    const s32 converted = static_cast<s32>(product >> down_shift);
    return value < 0 ? (std::min)(converted - accumulated, -1)
                     : (std::max)(converted + accumulated, 1);
}

/// The part of a guest scissor that bounds the framebuffer.
struct GuestScissor {
    bool enable;
    u32 min_x;
    u32 max_x;
    u32 min_y;
    u32 max_y;
};

/// What the framebuffer of a pass without attachments depends on.
struct AttachmentlessState {
    Extent2D size;           ///< The surface clip's, scaled
    bool lower_left;         ///< Viewports and scissors are flipped by the surface clip's height
    bool viewport_transform; ///< The guest scissors apply (viewport_scale_offset_enabled)
    u32 up_scale;
    u32 down_shift;
};

/// The framebuffer a pass without attachments needs, for a host that pays for the render area
/// (a tiled GPU: Apple's through MoltenVK).
///
/// Nothing is stored in such a pass, so only rasterisation needs the area, and the scissors bound
/// it: fragments, and with them storage writes and queries, come only from inside them. Games
/// leave the surface clip at the hardware's 32768x32768 there, and an Apple GPU pays for every
/// tile of that area (2.7 ms for one 1x1 draw in Ocarina of Time, 47 ms the first time). So the
/// size is cut to the host scissors the draw can use (`scissors`: the first, or all when a shader
/// picks the viewport), rounded up to a power of two so that a scissor that moves a little keeps
/// the framebuffer, and the render pass, it had.
///
/// It stays the surface clip's when the scissors do not bound the draw: one off (then only the
/// viewport does) or inverted, the viewport transform off (the rasterizer then sets every host
/// scissor to the surface clip), or a lower-left origin (scissors flipped by the clip's height).
[[nodiscard]] constexpr Extent2D AttachmentlessRenderArea(const AttachmentlessState& state,
                                                          std::span<const GuestScissor> scissors) {
    if (state.lower_left || !state.viewport_transform || scissors.empty()) {
        return state.size;
    }
    u32 width = 1;
    u32 height = 1;
    for (const GuestScissor& scissor : scissors) {
        if (!scissor.enable || scissor.max_x < scissor.min_x || scissor.max_y < scissor.min_y) {
            return state.size;
        }
        // Where the host scissor ends: its scaled offset plus its scaled extent.
        const auto end = [&](u32 min, u32 max) {
            const s32 offset =
                ScaleScissorCoordinate(static_cast<s32>(min), state.up_scale, state.down_shift);
            const s32 extent = ScaleScissorCoordinate(static_cast<s32>(max - min), state.up_scale,
                                                      state.down_shift);
            return static_cast<u32>(offset + extent);
        };
        width = (std::max)(width, end(scissor.min_x, scissor.max_x));
        height = (std::max)(height, end(scissor.min_y, scissor.max_y));
    }
    return Extent2D{
        .width = (std::min)(std::bit_ceil(width), (std::max)(state.size.width, 1U)),
        .height = (std::min)(std::bit_ceil(height), (std::max)(state.size.height, 1U)),
    };
}

/// The size an attachment-less pass's framebuffer gets on an update of the render targets, from
/// `full` (the surface clip's) and `needed` (what this call needs: AttachmentlessRenderArea for a
/// draw its scissors bound, `full` for anything else, such as a clear or DrawTexture).
///
/// Without the cut (`cut` false, a host that does not pay for the render area) it is what it
/// always was: the full size when the targets changed, the current one otherwise. With it, the
/// size follows what is needed when the targets changed and only grows while they do not: no call
/// gets a framebuffer smaller than it needs, and draws that need different sizes cost one growth,
/// not a new framebuffer and render pass each.
[[nodiscard]] constexpr Extent2D NextAttachmentlessSize(bool cut, bool targets_changed,
                                                        Extent2D current, Extent2D full,
                                                        Extent2D needed) {
    if (!cut) {
        return targets_changed ? full : current;
    }
    if (targets_changed) {
        return needed;
    }
    return Extent2D{
        .width = (std::max)(current.width, needed.width),
        .height = (std::max)(current.height, needed.height),
    };
}

} // namespace VideoCommon
