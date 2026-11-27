// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <optional>

namespace Vulkan {

/// How long a wait for the GPU runs before it is logged. Windows ends a device that does not answer
/// in about 2 s (TDR), so a stall logged at 1 s is in the log before the loss.
constexpr std::chrono::milliseconds WAIT_STALL_LOG_AFTER{1000};

/// One wait for a GPU tick, polled by the waiting thread each time its slice ends: reports once
/// when the wait passes WAIT_STALL_LOG_AFTER, and once more with the total when it ends.
class WaitStall {
public:
    using Clock = std::chrono::steady_clock;

    explicit WaitStall(Clock::time_point began_) : began{began_} {}

    /// True the first time the wait has run for WAIT_STALL_LOG_AFTER.
    bool Poll(Clock::time_point now) {
        if (reported || now - began < WAIT_STALL_LOG_AFTER) {
            return false;
        }
        reported = true;
        return true;
    }

    /// The total time of a wait that was reported; nothing for one that was not.
    std::optional<std::chrono::milliseconds> End(Clock::time_point now) const {
        if (!reported) {
            return std::nullopt;
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - began);
    }

    std::chrono::milliseconds Elapsed(Clock::time_point now) const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - began);
    }

private:
    Clock::time_point began;
    bool reported{};
};

} // namespace Vulkan
