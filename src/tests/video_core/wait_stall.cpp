// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/vk_wait_stall.h"

using namespace std::chrono_literals;
using Vulkan::WaitStall;

TEST_CASE("A GPU wait is reported once after 1 s and once when it ends", "[video_core][wait_stall]") {
    const auto t0 = WaitStall::Clock::now();
    WaitStall stall{t0};

    REQUIRE_FALSE(stall.Poll(t0 + 250ms));
    REQUIRE_FALSE(stall.Poll(t0 + 999ms));
    REQUIRE_FALSE(stall.End(t0 + 999ms).has_value());

    REQUIRE(stall.Poll(t0 + 1000ms));
    REQUIRE_FALSE(stall.Poll(t0 + 1250ms));
    REQUIRE_FALSE(stall.Poll(t0 + 5000ms));

    REQUIRE(stall.Elapsed(t0 + 1250ms) == 1250ms);
    const auto ended = stall.End(t0 + 5000ms);
    REQUIRE(ended.has_value());
    REQUIRE(*ended == 5000ms);
}

TEST_CASE("A short GPU wait is never reported", "[video_core][wait_stall]") {
    const auto t0 = WaitStall::Clock::now();
    WaitStall stall{t0};
    REQUIRE_FALSE(stall.Poll(t0 + 40ms));
    REQUIRE_FALSE(stall.End(t0 + 40ms).has_value());
}
