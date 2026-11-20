// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <deque>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/transcode_reaper.h"

namespace {

struct CompletionScheduler {
    u64 known{};
    u64 completed{};
    unsigned refreshes{};

    bool IsFree(u64 tick) const {
        return known >= tick;
    }
    CompletionScheduler& GetMasterSemaphore() {
        return *this;
    }
    void Refresh() {
        ++refreshes;
        known = completed;
    }
};

struct DecodeImage {
    u64 tick{};
    u32 width{1};
    u32 height{1};
    u32 levels{1};
    u32 layers{1};
    u32 unused_frames{20};
};

} // namespace

TEST_CASE("ASTC reaping refreshes past an already-complete prefix without waiting",
          "[video_core][transcode_reaper]") {
    CompletionScheduler scheduler{.known = 10, .completed = 11};
    std::deque<DecodeImage> pending{{.tick = 10}, {.tick = 11}, {.tick = 12}};
    std::vector<DecodeImage> idle;
    VideoCommon::ReapFinishedTranscodes(scheduler, pending, idle, 64);
    REQUIRE(idle.size() == 2);
    CHECK(idle[0].tick == 10);
    CHECK(idle[1].tick == 11);
    CHECK(idle[1].unused_frames == 0);
    REQUIRE(pending.size() == 1);
    CHECK(pending.front().tick == 12);
    CHECK(scheduler.refreshes == 1);
}

TEST_CASE("ASTC reaping does not query completion when cached progress is sufficient",
          "[video_core][transcode_reaper]") {
    CompletionScheduler scheduler{.known = 12, .completed = 12};
    std::deque<DecodeImage> pending{{.tick = 10}, {.tick = 11}};
    std::vector<DecodeImage> idle;
    VideoCommon::ReapFinishedTranscodes(scheduler, pending, idle, 64);
    CHECK(pending.empty());
    CHECK(idle.size() == 2);
    CHECK(scheduler.refreshes == 0);
    VideoCommon::ReapFinishedTranscodes(scheduler, pending, idle, 64);
    CHECK(scheduler.refreshes == 0);
}

TEST_CASE("ASTC reaping preserves unfinished images and enforces the idle budget",
          "[video_core][transcode_reaper]") {
    CompletionScheduler scheduler{.known = 0, .completed = 2};
    std::deque<DecodeImage> pending{{.tick = 1}, {.tick = 2}, {.tick = 3}};
    std::vector<DecodeImage> idle{{.tick = 0}};
    VideoCommon::ReapFinishedTranscodes(scheduler, pending, idle, 8);
    REQUIRE(idle.size() == 2);
    CHECK(idle[1].tick == 1);
    CHECK(idle[1].unused_frames == 0);
    REQUIRE(pending.size() == 1);
    CHECK(pending.front().tick == 3);
    CHECK(scheduler.refreshes == 1);
}

TEST_CASE("ASTC reaping returns when refreshing reports no progress",
          "[video_core][transcode_reaper]") {
    CompletionScheduler scheduler{};
    std::deque<DecodeImage> pending{{.tick = 1}};
    std::vector<DecodeImage> idle;
    VideoCommon::ReapFinishedTranscodes(scheduler, pending, idle, 64);
    CHECK(idle.empty());
    REQUIRE(pending.size() == 1);
    CHECK(pending.front().tick == 1);
    CHECK(scheduler.refreshes == 1);
}
