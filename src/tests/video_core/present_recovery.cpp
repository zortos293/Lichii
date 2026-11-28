// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/vk_present_recovery.h"

using namespace std::chrono_literals;
using Vulkan::AcquireAction;
using Vulkan::PresentRecovery;

TEST_CASE("An acquire that gets no image recreates the swapchain", "[video_core][present_recovery]") {
    REQUIRE(Vulkan::ClassifyAcquire(VK_SUCCESS) == AcquireAction::Present);
    REQUIRE(Vulkan::ClassifyAcquire(VK_SUBOPTIMAL_KHR) == AcquireAction::Present);
    REQUIRE(Vulkan::ClassifyAcquire(VK_TIMEOUT) == AcquireAction::Recreate);
    REQUIRE(Vulkan::ClassifyAcquire(VK_NOT_READY) == AcquireAction::Recreate);
    REQUIRE(Vulkan::ClassifyAcquire(VK_ERROR_OUT_OF_DATE_KHR) == AcquireAction::Recreate);
    REQUIRE(Vulkan::ClassifyAcquire(VK_ERROR_SURFACE_LOST_KHR) == AcquireAction::Throw);
    REQUIRE(Vulkan::ClassifyAcquire(VK_ERROR_DEVICE_LOST) == AcquireAction::Throw);
}

TEST_CASE("A changed surface extent is detected, an undefined one is not",
          "[video_core][present_recovery]") {
    constexpr VkExtent2D swapchain{1920, 1080};
    REQUIRE_FALSE(Vulkan::SurfaceExtentChanged({1920, 1080}, swapchain));
    REQUIRE(Vulkan::SurfaceExtentChanged({1280, 720}, swapchain));
    REQUIRE(Vulkan::SurfaceExtentChanged({1920, 0}, swapchain));
    REQUIRE_FALSE(Vulkan::SurfaceExtentChanged({0xFFFFFFFFu, 0xFFFFFFFFu}, swapchain));
}

TEST_CASE("One recovery event is reported once at its start and once at its end",
          "[video_core][present_recovery]") {
    PresentRecovery recovery;
    const auto t0 = PresentRecovery::Clock::now();

    REQUIRE_FALSE(recovery.Succeed(t0).has_value());

    REQUIRE(recovery.Fail(VK_TIMEOUT, t0));
    REQUIRE_FALSE(recovery.Fail(VK_ERROR_OUT_OF_DATE_KHR, t0 + 1000ms));
    REQUIRE_FALSE(recovery.Fail(VK_TIMEOUT, t0 + 2000ms));

    const auto recovered = recovery.Succeed(t0 + 2500ms);
    REQUIRE(recovered.has_value());
    REQUIRE(recovered->first_result == VK_TIMEOUT);
    REQUIRE(recovered->failures == 3);
    REQUIRE(recovered->duration == 2500ms);

    REQUIRE_FALSE(recovery.Succeed(t0 + 3000ms).has_value());

    REQUIRE(recovery.Fail(VK_ERROR_SURFACE_LOST_KHR, t0 + 4000ms));
    const auto second = recovery.Succeed(t0 + 4100ms);
    REQUIRE(second.has_value());
    REQUIRE(second->first_result == VK_ERROR_SURFACE_LOST_KHR);
    REQUIRE(second->failures == 1);
}

TEST_CASE("A wait is reported once after the threshold and once when it ends",
          "[video_core][present_recovery]") {
    Vulkan::WaitWatch watch;
    const auto t0 = Vulkan::WaitWatch::Clock::now();

    REQUIRE_FALSE(watch.Poll(t0, 1000ms).has_value());
    REQUIRE_FALSE(watch.End(t0).has_value());

    watch.Begin("scheduler wait", 2, 40, t0);
    REQUIRE_FALSE(watch.Poll(t0 + 999ms, 1000ms).has_value());
    const auto stalled = watch.Poll(t0 + 1250ms, 1000ms);
    REQUIRE(stalled.has_value());
    REQUIRE(stalled->index == 2);
    REQUIRE(stalled->tick == 40);
    REQUIRE(stalled->elapsed == 1250ms);
    REQUIRE_FALSE(watch.Poll(t0 + 2000ms, 1000ms).has_value());

    const auto ended = watch.End(t0 + 3000ms);
    REQUIRE(ended.has_value());
    REQUIRE(ended->elapsed == 3000ms);
    REQUIRE_FALSE(watch.Poll(t0 + 4000ms, 1000ms).has_value());

    watch.Begin("submit", 0, 0, t0 + 5000ms);
    REQUIRE_FALSE(watch.End(t0 + 5100ms).has_value());
}
