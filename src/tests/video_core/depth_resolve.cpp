// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "video_core/texture_cache/depth_resolve.h"

namespace {

using Tegra::Engines::Fermi2D;
using namespace VideoCommon;
constexpr s64 half = s64{1} << 31;
constexpr s64 two = s64{2} << 32;

ImageInfo DepthInfo(u32 samples) {
    ImageInfo info;
    info.format = PixelFormat::D32_FLOAT;
    info.type = ImageType::e2D;
    info.num_samples = samples;
    info.resources.levels = 1;
    info.resources.layers = 1;
    info.size = {1600, 900, 1};
    info.block = {0, 4, 0};
    return info;
}

TEST_CASE("Meeltix_WashiWashi_DepthResolve") {
    // No false Washi. Four samples must resolve correctly.
    using Origin = Fermi2D::Origin;
    CHECK(Fermi2D::IsCentered2x2Filter(Origin::Center, half, half, two, two));
    CHECK_FALSE(Fermi2D::IsCentered2x2Filter(Origin::Center, half / 2, half, two, two));
    CHECK_FALSE(Fermi2D::IsCentered2x2Filter(Origin::Center, half, half + half / 2, two, two));
    CHECK_FALSE(Fermi2D::IsCentered2x2Filter(Origin::Center, half, half, two + (1 << 20), two));
    CHECK_FALSE(Fermi2D::IsCentered2x2Filter(Origin::Center, half, half, two, two - 1));
    CHECK_FALSE(Fermi2D::IsCentered2x2Filter(Origin::Corner, half, half, two, two));

    Fermi2D::Surface src{};
    src.format = Tegra::RenderTargetFormat::R32_FLOAT;
    src.width = 3200;
    src.height = 1800;
    src.depth = 1;
    src.block_height.Assign(4);
    src.addr_lower = 0x10000;
    auto dst = src;
    dst.width = 1600;
    dst.height = 900;
    dst.addr_lower = 0x610000;
    Fermi2D::Config config{
        .operation = Fermi2D::Operation::SrcCopy,
        .filter = Fermi2D::Filter::Bilinear,
        .must_accelerate = true,
        .dst_x0 = 0,
        .dst_y0 = 0,
        .dst_x1 = 1600,
        .dst_y1 = 900,
        .src_x0 = 0,
        .src_y0 = 0,
        .src_x1 = 3200,
        .src_y1 = 1800,
        .centered_2x2_filter = true,
    };
    CHECK(IsFourSampleDepthResolve(dst, src, config));
    ImageInfo expected_source(src);
    expected_source.num_samples = 4;
    const ImageInfo expected_destination(dst);
    auto actual_source = DepthInfo(4);
    actual_source.size = {src.width, src.height, 1};
    auto actual_destination = DepthInfo(1);
    actual_destination.format = PixelFormat::R32_FLOAT;
    CHECK(MatchesDepthResolveLayout(expected_source, actual_source));
    CHECK(MatchesDepthResolveDestination(expected_destination, actual_destination, false, true));
    actual_destination.num_samples = 4;
    CHECK_FALSE(MatchesDepthResolveDestination(expected_destination, actual_destination, false, true));
    actual_source.num_samples = 2;
    CHECK_FALSE(MatchesDepthResolveLayout(expected_source, actual_source));
    config.centered_2x2_filter = false;
    CHECK_FALSE(IsFourSampleDepthResolve(dst, src, config));
    config.centered_2x2_filter = true;
    config.filter = Fermi2D::Filter::Point;
    CHECK_FALSE(IsFourSampleDepthResolve(dst, src, config));
    config.filter = Fermi2D::Filter::Bilinear;
    dst.layer = 1;
    CHECK_FALSE(IsFourSampleDepthResolve(dst, src, config));
    dst.layer = 0;
    src.linear = Fermi2D::MemoryLayout::Pitch;
    CHECK_FALSE(IsFourSampleDepthResolve(dst, src, config));
}

TEST_CASE("Depth resolve requires identical physical layout and exact sample counts",
          "[video_core][depth_resolve]") {
    const auto source = DepthInfo(4);
    const auto destination = DepthInfo(1);
    CHECK(MatchesDepthResolveLayout(source, source));
    auto fermi_source = source;
    fermi_source.format = PixelFormat::R32_FLOAT;
    CHECK(MatchesDepthResolveLayout(fermi_source, source));
    CHECK(MatchesDepthResolveLayout(destination, destination));
    CHECK_FALSE(MatchesDepthResolveLayout(source, destination));
    CHECK_FALSE(MatchesDepthResolveLayout(destination, source));
    CHECK(ConflictsWithDepthResolveDestination(ImageBase(source, 0x10000, 0x10000)));
    CHECK_FALSE(ConflictsWithDepthResolveDestination(ImageBase(destination, 0x10000, 0x10000)));
    auto incompatible = source;
    incompatible.block.height = 3;
    CHECK_FALSE(MatchesDepthResolveLayout(source, incompatible));
    incompatible = source;
    incompatible.tile_width_spacing = 1;
    CHECK_FALSE(MatchesDepthResolveLayout(source, incompatible));
    incompatible = source;
    incompatible.native_tiling = true;
    CHECK_FALSE(MatchesDepthResolveLayout(source, incompatible));
    incompatible = source;
    incompatible.resources.layers = 2;
    CHECK_FALSE(MatchesDepthResolveLayout(source, incompatible));
}

TEST_CASE("Depth resolve preserves the single-sample R32 color view class",
          "[video_core][depth_resolve]") {
    auto expected = DepthInfo(1);
    expected.format = PixelFormat::R32_FLOAT;
    CHECK(MatchesDepthResolveDestination(expected, expected, false, true));
    auto alias = expected;
    alias.format = PixelFormat::R32_UINT;
    CHECK(MatchesDepthResolveDestination(expected, alias, false, true));
    CHECK_FALSE(MatchesDepthResolveDestination(expected, alias, true, true));
    alias.format = PixelFormat::R32_SINT;
    CHECK(MatchesDepthResolveDestination(expected, alias, false, true));
    alias.format = PixelFormat::D32_FLOAT;
    CHECK_FALSE(MatchesDepthResolveDestination(expected, alias, false, true));
}

TEST_CASE("Overlapping writes invalidate a cached depth resolve source",
          "[video_core][depth_resolve]") {
    ImageBase depth(DepthInfo(4), 0x10000, 0x10000);
    depth.flags = ImageFlagBits::GpuModified;
    depth.modification_tick = 5;
    DepthResolveOwnership ownership;
    CHECK(ownership.ConflictsWith(1));
    ownership.full_clear_tick = 5;
    CHECK(IsDepthResolveSourceCurrent(depth));
    depth.flags |= ImageFlagBits::CpuModified;
    CHECK_FALSE(IsDepthResolveSourceCurrent(depth));
    depth.flags = ImageFlagBits::GpuModified | ImageFlagBits::Remapped;
    CHECK_FALSE(IsDepthResolveSourceCurrent(depth));

    const auto linear_info = [] {
        ImageInfo info = DepthInfo(1);
        info.format = PixelFormat::R32_FLOAT;
        info.type = ImageType::Linear;
        info.pitch = 6400;
        return info;
    }();
    ImageBase writer(linear_info, 0x2000000, 0x10004);
    writer.flags = ImageFlagBits::GpuModified;
    writer.modification_tick = 6;
    depth.modification_tick = 7;
    REQUIRE(writer.Overlaps(depth.cpu_addr, depth.guest_size_bytes));
    CHECK(ownership.ConflictsWith(writer.modification_tick));
    writer.flags = {};
    CHECK(ownership.ConflictsWith(writer.modification_tick));
    ownership.retired_writer_tick = writer.modification_tick;
    writer.modification_tick = 4;
    CHECK_FALSE(ownership.ConflictsWith(writer.modification_tick));
    CHECK(ownership.ConflictsWith(ownership.retired_writer_tick));
    ownership.full_clear_tick = 8;
    CHECK_FALSE(ownership.ConflictsWith(ownership.retired_writer_tick));
    CHECK_FALSE(DepthResolveRangesAreDisjoint(0x1000, 0x1000, 0x1800, 0x1000));
    CHECK_FALSE(DepthResolveRangesAreDisjoint(0x1000, 0x1000, 0x1000, 0x1000));
    CHECK(DepthResolveRangesAreDisjoint(0x1000, 0x1000, 0x2000, 0x1000));
}

} // namespace
