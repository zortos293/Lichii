// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <catch2/catch_test_macros.hpp>
#include "video_core/upscaler_fallback_log.h"

TEST_CASE("Upscaler fallback log reports each stretch's start, reason and length, a line a second "
          "at most",
          "[video_core][upscaler]") {
    using namespace std::chrono_literals;
    using Clock = VideoCommon::UpscalerFallbackLog::Clock;
    VideoCommon::UpscalerFallbackLog log;
    const Clock::time_point t0{};
    const char* const camera = "no scene camera this frame";
    const char* const depth = "no scene depth this frame";

    CHECK_FALSE(log.Replaced(1, t0));
    const auto first = log.Fallback(10, t0, camera);
    REQUIRE(first);
    CHECK(first->frame == 10);
    CHECK(first->unlogged == 0);
    CHECK_FALSE(log.Fallback(11, t0 + 16ms, camera));
    // Another reason in the same stretch waits for the next second.
    CHECK_FALSE(log.Fallback(12, t0 + 33ms, depth));
    CHECK(log.Fallback(70, t0 + 1s, camera));
    const auto end = log.Replaced(610, t0 + 10s);
    REQUIRE(end);
    CHECK(end->frames == 600);
    CHECK(end->seconds == 10.0f);

    // Stretches within a second of the last line are counted, and their ends aren't logged.
    CHECK_FALSE(log.Fallback(620, t0 + 10s + 100ms, camera));
    CHECK_FALSE(log.Replaced(621, t0 + 10s + 116ms));
    CHECK_FALSE(log.Fallback(630, t0 + 10s + 300ms, depth));
    CHECK_FALSE(log.Replaced(640, t0 + 10s + 500ms));
    const auto next = log.Fallback(700, t0 + 12s, depth);
    REQUIRE(next);
    CHECK(next->unlogged == 2);
    const auto next_end = log.Replaced(760, t0 + 13s);
    REQUIRE(next_end);
    CHECK(next_end->frames == 60);

    // A stretch that starts within a second of the last line is logged once the second has
    // passed, from its first frame, and its end covers the whole stretch.
    CHECK_FALSE(log.Fallback(800, t0 + 13s + 200ms, camera));
    const auto late = log.Fallback(850, t0 + 14s, camera);
    REQUIRE(late);
    CHECK(late->frame == 800);
    CHECK(late->unlogged == 0);
    const auto late_end = log.Replaced(1094, t0 + 18s + 100ms);
    REQUIRE(late_end);
    CHECK(late_end->frames == 294);
    CHECK(late_end->seconds > 4.8f);
}
