// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>

#include "video_core/game_dlss_status.h"

namespace VideoCore {

namespace {
std::atomic<bool> nvidia_dlss_paused{};
std::atomic<s64> nvidia_dlss_frame_ns{};

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // Anonymous namespace

void SetNvidiaDlssPaused(bool paused) {
    nvidia_dlss_paused.store(paused, std::memory_order_relaxed);
}

bool NvidiaDlssPaused() {
    return nvidia_dlss_paused.load(std::memory_order_relaxed);
}

void MarkNvidiaDlssFrame() {
    nvidia_dlss_frame_ns.store(NowNs(), std::memory_order_relaxed);
}

bool NvidiaDlssDrawing() {
    const s64 last = nvidia_dlss_frame_ns.load(std::memory_order_relaxed);
    return last != 0 && NowNs() - last < 250'000'000;
}

const char* GameDlssReasonText(GameDlssReason reason) {
    switch (reason) {
    case GameDlssReason::None:
        return "";
    case GameDlssReason::TurnedOff:
        return "it is turned off";
    case GameDlssReason::NvidiaDlss:
        return "NVIDIA DLSS is turned on and runs in its place";
    case GameDlssReason::ResolutionScaling:
        return "it only makes the game's own resolution (resolution scaling is on)";
    case GameDlssReason::Unsupported:
        return "it needs Vulkan and compute shaders 32 threads wide";
    case GameDlssReason::PassesCannotRun:
        return "some of its passes can't run here";
    case GameDlssReason::InvalidOutput:
        return "its output had invalid values (NaN)";
    case GameDlssReason::OutputMismatch:
        return "its output stopped matching the game's image (frozen or corrupted)";
    }
    return "";
}

} // namespace VideoCore
