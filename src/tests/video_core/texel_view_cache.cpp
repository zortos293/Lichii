// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/texel_view_cache.h"

namespace {

/// A move-only stand-in for vk::BufferView that counts the views alive.
struct FakeView {
    static inline int alive = 0;

    FakeView() = default;
    explicit FakeView(u64 id_) : id{id_} {
        ++alive;
    }
    FakeView(FakeView&& other) noexcept : id{std::exchange(other.id, 0)} {}
    FakeView& operator=(FakeView&& other) noexcept {
        if (id != 0) {
            --alive;
        }
        id = std::exchange(other.id, 0);
        return *this;
    }
    FakeView(const FakeView&) = delete;
    FakeView& operator=(const FakeView&) = delete;
    ~FakeView() {
        if (id != 0) {
            --alive;
        }
    }

    u64 id{};
};

} // namespace

TEST_CASE("Texel views at offsets that move every frame stay bounded",
          "[video_core][texel_view_cache]") {
    using Cache = VideoCommon::TexelViewCache<FakeView>;
    FakeView::alive = 0;
    {
        Cache cache;
        constexpr u64 frames = 20000;
        constexpr u64 draws_per_frame = 24;
        constexpr u64 frames_in_flight = 3;
        u64 next_id = 1;
        size_t peak = 0;
        for (u64 frame = 0; frame < frames; ++frame) {
            const u64 completed = frame >= frames_in_flight ? frame - frames_in_flight : 0;
            const auto is_free = [completed](u64 stamp) { return stamp < completed; };
            for (u64 draw = 0; draw < draws_per_frame; ++draw) {
                const u64 offset = (frame * draws_per_frame + draw) * 192;
                if (!cache.Find(offset, 192, 100, frame)) {
                    cache.Insert(offset, 192, 100, frame, FakeView{next_id++}, is_free);
                }
            }
            peak = (std::max)(peak, cache.Size());
        }
        // Without eviction this is 480000 views, close to the ~523k NVIDIA's view table holds.
        CHECK(next_id - 1 == frames * draws_per_frame);
        CHECK(peak <= 2 * (frames_in_flight + 2) * draws_per_frame + Cache::MIN_SWEEP_SIZE);
        CHECK(FakeView::alive == static_cast<int>(cache.Size()));
    }
    CHECK(FakeView::alive == 0);
}

TEST_CASE("Texel views the GPU may still use are kept", "[video_core][texel_view_cache]") {
    VideoCommon::TexelViewCache<FakeView> cache;
    const auto nothing_free = [](u64) { return false; };
    for (u64 i = 0; i < 1000; ++i) {
        cache.Insert(i * 16, 16, 98, i, FakeView{i + 1}, nothing_free);
    }
    CHECK(cache.Size() == 1000);
    for (u64 i = 0; i < 1000; ++i) {
        const FakeView* const view = cache.Find(i * 16, 16, 98, 1000);
        REQUIRE(view != nullptr);
        CHECK(view->id == i + 1);
    }
}

TEST_CASE("A texel view used again keeps its handle across sweeps",
          "[video_core][texel_view_cache]") {
    using Cache = VideoCommon::TexelViewCache<FakeView>;
    Cache cache;
    const auto older_than = [](u64 limit) { return [limit](u64 stamp) { return stamp < limit; }; };
    cache.Insert(0, 64, 100, 0, FakeView{1}, older_than(0));
    u64 id = 2;
    for (u64 stamp = 1; stamp < 10 * Cache::MIN_SWEEP_SIZE; ++stamp) {
        REQUIRE(cache.Find(0, 64, 100, stamp) != nullptr);
        cache.Insert(stamp * 64, 64, 100, stamp, FakeView{id++}, older_than(stamp));
    }
    const FakeView* const kept = cache.Find(0, 64, 100, 0);
    REQUIRE(kept != nullptr);
    CHECK(kept->id == 1);
    CHECK(cache.Find(64, 64, 100, 0) == nullptr);
    CHECK(cache.Find(0, 64, 98, 0) == nullptr);
    CHECK(cache.Find(0, 32, 100, 0) == nullptr);
}
