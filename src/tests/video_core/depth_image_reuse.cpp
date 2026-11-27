// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/depth_image_reuse.h"

namespace {

using VideoCommon::DepthImageReuseKey;
using Pool = VideoCommon::DepthImageReusePool<std::unique_ptr<int>>;

constexpr DepthImageReuseKey RaidersDepth{2048, 2048, 2};

} // namespace

TEST_CASE("Depth allocations are only retained for Raiders on RADV with a budget",
          "[depth_image_reuse]") {
    constexpr u64 Raiders = 0x0400380020056000ULL;
    constexpr u64 MarioKartWorld = 0x0400C3F00006E000ULL;
    REQUIRE(VideoCommon::AllowDepthImageReuse(Raiders, true, true));
    REQUIRE_FALSE(VideoCommon::AllowDepthImageReuse(Raiders, false, true));
    REQUIRE_FALSE(VideoCommon::AllowDepthImageReuse(Raiders, true, false));
    REQUIRE_FALSE(VideoCommon::AllowDepthImageReuse(MarioKartWorld, true, true));
    REQUIRE_FALSE(VideoCommon::AllowDepthImageReuse(0, true, true));
}

TEST_CASE("Retired depth allocations match exact extents and layers", "[depth_image_reuse]") {
    Pool pool;
    auto depth = std::make_unique<int>(1);
    REQUIRE(pool.Recycle(RaidersDepth, depth, Pool::MaxBytes / 4));
    REQUIRE_FALSE(depth);
    REQUIRE_FALSE(pool.Acquire({2048, 2048, 1}));
    REQUIRE_FALSE(pool.Acquire({2048, 2048, 3}));
    REQUIRE_FALSE(pool.Acquire({2048, 2047, 2}));
    auto reused = pool.Acquire(RaidersDepth);
    REQUIRE(reused);
    REQUIRE(*reused == 1);
    REQUIRE(pool.Hits() == 1);
    REQUIRE(pool.Misses() == 3);
    REQUIRE(pool.Bytes() == 0);
}

TEST_CASE("Retired depth allocations obey size, count, age and pressure limits",
          "[depth_image_reuse]") {
    Pool pool;
    auto oversized = std::make_unique<int>(1);
    REQUIRE_FALSE(pool.Recycle(RaidersDepth, oversized, Pool::MaxBytes + 1));
    REQUIRE(oversized);
    for (size_t i = 0; i < Pool::MaxImages + 1; ++i) {
        auto depth = std::make_unique<int>(static_cast<int>(i));
        REQUIRE(pool.Recycle(RaidersDepth, depth, 1));
    }
    REQUIRE(pool.Size() == Pool::MaxImages);
    auto reused = pool.Acquire(RaidersDepth);
    REQUIRE(reused);
    REQUIRE(*reused == 1);
    auto full = std::make_unique<int>(42);
    REQUIRE(pool.Recycle(RaidersDepth, full, Pool::MaxBytes));
    REQUIRE(pool.Size() == 1);
    REQUIRE(pool.Bytes() == Pool::MaxBytes);
    for (u64 frame = 0; frame < Pool::MaxAge; ++frame) {
        pool.Tick();
    }
    REQUIRE(pool.Size() == 0);
    REQUIRE(pool.Bytes() == 0);
    full = std::make_unique<int>(42);
    REQUIRE(pool.Recycle(RaidersDepth, full, Pool::MaxBytes));
    pool.Clear();
    REQUIRE_FALSE(pool.Acquire(RaidersDepth));
    REQUIRE(pool.Bytes() == 0);
}
