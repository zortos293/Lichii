// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <mutex>
#include <optional>

#include <vulkan/vulkan_core.h>

#include "common/common_types.h"

namespace Vulkan {

/// How long vkAcquireNextImageKHR may block. A display that was torn down or changed (a streamed
/// session ending, a virtual display going away) can leave an acquire with an infinite timeout
/// waiting for ever on some drivers; the presenter recreates the swapchain instead (#2618).
constexpr u64 ACQUIRE_TIMEOUT_NS = 1'000'000'000;

enum class AcquireAction {
    Present,  ///< An image was acquired (VK_SUBOPTIMAL_KHR still presents, then recreates).
    Recreate, ///< No image: recreate the swapchain and acquire again.
    Throw,    ///< The surface or device is gone for good: raise it.
};

constexpr AcquireAction ClassifyAcquire(VkResult result) {
    switch (result) {
    case VK_SUCCESS:
    case VK_SUBOPTIMAL_KHR:
        return AcquireAction::Present;
    case VK_TIMEOUT:
    case VK_NOT_READY:
    case VK_ERROR_OUT_OF_DATE_KHR:
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
        return AcquireAction::Recreate;
    default:
        return AcquireAction::Throw;
    }
}

/// True when the surface reports a size other than the swapchain's. An undefined current extent
/// (0xFFFFFFFF, the surface follows the swapchain) never counts as a change.
constexpr bool SurfaceExtentChanged(VkExtent2D surface_extent, VkExtent2D swapchain_extent) {
    constexpr u32 undefined = 0xFFFFFFFFu;
    if (surface_extent.width == undefined || surface_extent.height == undefined) {
        return false;
    }
    return surface_extent.width != swapchain_extent.width ||
           surface_extent.height != swapchain_extent.height;
}

/// Groups the failures of one recovery into a single event, so each is logged once at its start
/// and once when presenting works again, however many frames it takes.
class PresentRecovery {
public:
    using Clock = std::chrono::steady_clock;

    struct Recovered {
        VkResult first_result;
        u32 failures;
        std::chrono::milliseconds duration;
    };

    /// Counts a failure. True for the first of an event: the caller logs it.
    bool Fail(VkResult result, Clock::time_point now) {
        ++failures;
        if (active) {
            return false;
        }
        active = true;
        first_result = result;
        began = now;
        return true;
    }

    /// A present went through: the summary of the event it ends, if there was one.
    std::optional<Recovered> Succeed(Clock::time_point now) {
        if (!active) {
            return std::nullopt;
        }
        const Recovered recovered{
            .first_result = first_result,
            .failures = failures,
            .duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - began),
        };
        active = false;
        failures = 0;
        return recovered;
    }

private:
    bool active{};
    VkResult first_result{VK_SUCCESS};
    u32 failures{};
    Clock::time_point began{};
};

/// A wait on the present path that has run for WAIT_STALL_THRESHOLD or more. Diagnostics only: the
/// wait itself is untouched, a watchdog thread polls and each wait is reported once, and once more
/// when it ends.
constexpr std::chrono::milliseconds WAIT_STALL_THRESHOLD{1000};

class WaitWatch {
public:
    using Clock = std::chrono::steady_clock;

    struct Wait {
        const char* what;
        u64 index;
        u64 tick;
        std::chrono::milliseconds elapsed;
    };

    void Begin(const char* what, u64 index, u64 tick, Clock::time_point now) {
        std::scoped_lock lock{mutex};
        active = true;
        reported = false;
        current = {what, index, tick, {}};
        began = now;
    }

    /// The wait in progress once it has run for `threshold`, the first time only.
    std::optional<Wait> Poll(Clock::time_point now, std::chrono::milliseconds threshold) {
        std::scoped_lock lock{mutex};
        if (!active || reported || now - began < threshold) {
            return std::nullopt;
        }
        reported = true;
        return Snapshot(now);
    }

    /// Ends the wait. The wait if it had been reported, with its total time.
    std::optional<Wait> End(Clock::time_point now) {
        std::scoped_lock lock{mutex};
        const bool was_reported = active && reported;
        active = false;
        reported = false;
        if (!was_reported) {
            return std::nullopt;
        }
        return Snapshot(now);
    }

private:
    Wait Snapshot(Clock::time_point now) const {
        Wait wait = current;
        wait.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - began);
        return wait;
    }

    std::mutex mutex;
    bool active{};
    bool reported{};
    Wait current{};
    Clock::time_point began{};
};

} // namespace Vulkan
