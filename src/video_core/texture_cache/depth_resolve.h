// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <utility>

#include "video_core/compatible_formats.h"
#include "video_core/texture_cache/image_base.h"

namespace VideoCommon {

// Match the centered sample-grid resolve emitted by deko3d's dkCmdBufResolveImage.
[[nodiscard]] inline bool IsFourSampleDepthResolve(const Tegra::Engines::Fermi2D::Surface& dst,
                                                   const Tegra::Engines::Fermi2D::Surface& src,
                                                   const Tegra::Engines::Fermi2D::Config& copy) {
    using Fermi2D = Tegra::Engines::Fermi2D;
    return copy.centered_2x2_filter && copy.operation == Fermi2D::Operation::SrcCopy &&
           copy.filter == Fermi2D::Filter::Bilinear &&
           src.format == Tegra::RenderTargetFormat::R32_FLOAT && src.format == dst.format &&
           src.linear == Fermi2D::MemoryLayout::BlockLinear && src.linear == dst.linear &&
           src.depth == 1 && dst.depth == 1 && src.layer == 0 && dst.layer == 0 &&
           src.block_depth == 0 && dst.block_depth == 0 && src.Address() != dst.Address() &&
           dst.width != 0 && dst.height != 0 && src.width == 2ULL * dst.width &&
           src.height == 2ULL * dst.height && copy.src_x0 == 0 && copy.src_y0 == 0 &&
           copy.dst_x0 == 0 && copy.dst_y0 == 0 && std::cmp_equal(copy.src_x1, src.width) &&
           std::cmp_equal(copy.src_y1, src.height) && std::cmp_equal(copy.dst_x1, dst.width) &&
           std::cmp_equal(copy.dst_y1, dst.height);
}

[[nodiscard]] inline bool MatchesDepthResolveLayout(const ImageInfo& expected,
                                                   const ImageInfo& actual) {
    return actual.type == ImageType::e2D && actual.type == expected.type &&
           actual.num_samples == expected.num_samples &&
           actual.size == expected.size && actual.block == expected.block &&
           actual.native_tiling == expected.native_tiling &&
           actual.tile_width_spacing == expected.tile_width_spacing &&
           actual.resources.levels == 1 && actual.resources.layers == 1;
}

[[nodiscard]] inline bool MatchesDepthResolveDestination(const ImageInfo& expected,
                                                        const ImageInfo& actual,
                                                        bool broken_views, bool native_bgr) {
    return MatchesDepthResolveLayout(expected, actual) &&
           IsViewCompatible(actual.format, expected.format, broken_views, native_bgr);
}

[[nodiscard]] inline bool IsDepthResolveSourceCurrent(const ImageBase& source) {
    return True(source.flags & ImageFlagBits::GpuModified) &&
           False(source.flags & (ImageFlagBits::CpuModified | ImageFlagBits::Remapped |
                                 ImageFlagBits::Unbacked | ImageFlagBits::Sparse |
                                 ImageFlagBits::TrimmedLevels));
}

struct DepthResolveOwnership {
    // Partial depth writes cannot supersede an overlapping writer for the whole surface.
    u64 full_clear_tick = 0;
    u64 retired_writer_tick = 0;

    [[nodiscard]] bool ConflictsWith(u64 writer_tick) const {
        return writer_tick > full_clear_tick;
    }
};

[[nodiscard]] constexpr bool DepthResolveRangesAreDisjoint(
    DAddr src, size_t src_size, DAddr dst, size_t dst_size) {
    return src < dst ? src_size <= dst - src : dst_size <= src - dst;
}

[[nodiscard]] inline bool ConflictsWithDepthResolveDestination(const ImageBase& overlap) {
    return overlap.info.num_samples != 1;
}

} // namespace VideoCommon
