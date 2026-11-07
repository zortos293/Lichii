// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include "video_core/surface.h"
#include "video_core/textures/texture.h"

namespace Tegra::Texture {
inline bool SamplesStencil(VideoCore::Surface::PixelFormat format,
                           const std::array<SwizzleSource, 4>& swizzle) {
    using VideoCore::Surface::PixelFormat;
    const bool any_r = std::ranges::find(swizzle, SwizzleSource::R) != swizzle.end();
    switch (format) {
    case PixelFormat::D24_UNORM_S8_UINT:
    case PixelFormat::D32_FLOAT_S8_UINT:
        return !any_r;
    case PixelFormat::S8_UINT_D24_UNORM:
        return any_r;
    case PixelFormat::S8_UINT:
        return true;
    default:
        return false;
    }
}

inline bool SamplesDepth24(VideoCore::Surface::PixelFormat format,
                           const std::array<SwizzleSource, 4>& swizzle) {
    using VideoCore::Surface::PixelFormat;
    return (format == PixelFormat::X8_D24_UNORM || format == PixelFormat::D24_UNORM_S8_UINT ||
            format == PixelFormat::S8_UINT_D24_UNORM) &&
           !SamplesStencil(format, swizzle);
}
} // namespace Tegra::Texture
