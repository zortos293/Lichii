// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <utility>

#include "common/logging.h"
#include "video_core/vulkan_common/vulkan.h"

namespace Vulkan {

/// Reads of a driver pipeline cache a save makes, each after its own size query, before it settles
/// for what it got.
inline constexpr int PipelineCacheReadAttempts = 4;

/// Less than this is no cache: by the spec, a buffer smaller than the version one header gets no
/// bytes at all.
inline constexpr size_t MinPipelineCacheDataSize = sizeof(VkPipelineCacheHeaderVersionOne);

/// Driver pipeline cache data read for a save.
struct PipelineCacheData {
    std::unique_ptr<char[]> bytes;
    size_t size = 0;
    /// VK_SUCCESS: all of it, or, with less than a header, a size that holds no cache.
    /// VK_INCOMPLETE: with more than a header, part of it, a valid cache by the spec that lacks
    /// what was built during the read; with no more, nothing usable after every read. Otherwise
    /// the error a call returned.
    VkResult result = VK_SUCCESS;
    int reads = 0;

    /// All of the cache.
    [[nodiscard]] bool Whole() const {
        return result == VK_SUCCESS && size >= MinPipelineCacheDataSize;
    }
    /// Part of it: a header and at least some of what follows.
    [[nodiscard]] bool Partial() const {
        return result == VK_INCOMPLETE && size > MinPipelineCacheDataSize;
    }
    /// Worth saving over the file: a cache holds a header at least, and a live one never less.
    [[nodiscard]] bool Usable() const {
        return Whole() || Partial();
    }
};

/// Reads a driver pipeline cache through `read` (vkGetPipelineCacheData). Pipelines may build into
/// it between the size query and the read; the size is then stale and the read returns
/// VK_INCOMPLETE. A conforming driver gives back a smaller valid cache then, which is taken at
/// once, as Lichii always did: reading again would serialize the whole cache again for a result
/// as partial. MoltenVK gives none at all, and a larger buffer than the size query gave does not
/// help there: a build marks its cache dirty, which zeroes the size it serializes into, so any
/// buffer gets VK_INCOMPLETE and no bytes until the size is asked for again (asked 511 bytes, a
/// pipeline built, a read into 4607 returned 0). So the size is asked for again, and the cache
/// read again, only after a read that got nothing usable, up to PipelineCacheReadAttempts reads.
/// `progress` is called after each read: a read serializes the whole cache, gigabytes for a large
/// one, and a shutdown waits on it. One buffer is held at a time.
template <typename Read, typename Progress>
PipelineCacheData ReadPipelineCacheData(Read&& read, Progress&& progress) {
    for (int attempt = 1; attempt <= PipelineCacheReadAttempts; ++attempt) {
        size_t size = 0;
        if (const VkResult result = read(&size, nullptr); result != VK_SUCCESS) {
            return {.result = result, .reads = attempt};
        }
        // Less than a header is no cache: a live one always holds one.
        if (size < MinPipelineCacheDataSize) {
            return {.size = size, .reads = attempt};
        }
        const size_t asked = size;
        auto bytes = std::make_unique<char[]>(size);
        const VkResult result = read(&size, bytes.get());
        progress();
        // Never more than the buffer: a driver that reports more would have the write read past
        // it.
        size = std::min(size, asked);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            return {.result = result, .reads = attempt};
        }
        PipelineCacheData data{std::move(bytes), size, result, attempt};
        if (data.Usable() || result == VK_SUCCESS) {
            return data;
        }
        LOG_DEBUG(Render_Vulkan,
                  "Driver pipeline cache changed after its size ({} bytes) was read, read {} of {} "
                  "got {} bytes",
                  asked, attempt, PipelineCacheReadAttempts, size);
    }
    return {.result = VK_INCOMPLETE, .reads = PipelineCacheReadAttempts};
}

/// What the timed flush of the driver pipeline cache is decided on. A default time point is
/// "never".
struct PipelineCacheFlushState {
    /// When the last flush that counts was queued: one that was saved, or failed for another
    /// reason than a read with nothing usable (AfterPipelineCacheSave)
    std::chrono::steady_clock::time_point last_saved;
    /// When the last attempt, timed flush or the save after the boot builds, ended
    std::chrono::steady_clock::time_point last_attempt;
    /// Whether that attempt got nothing usable from the driver
    bool last_failed = false;
    /// Pipelines built since the last flush was queued
    size_t pipelines = 0;
};

/// A flush is due once `pipelines_needed` pipelines were built, `interval` after the last flush
/// that counts (PipelineCacheFlushInterval; see AfterPipelineCacheSave), and
/// PipelineCacheFlushRetryFloor after the last attempt ended. After an attempt that got nothing
/// usable from the driver (a build overtook every read) neither the pipeline count nor the
/// interval is waited for, and the floor alone spaces the next try: the pipelines built since may
/// stay fewer than the count (the burst that overtook the reads ended), the last flush that counts
/// may be recent (the save after the boot builds, with the one after the precache overtaken), and
/// everything since that save would then reach the file only on a clean close. The floor runs
/// from the end of the attempt, so a save that itself takes longer than the floor is not queued
/// again with no pause.
inline constexpr std::chrono::seconds PipelineCacheFlushRetryFloor{30};
[[nodiscard]] inline bool PipelineCacheFlushDue(const PipelineCacheFlushState& state,
                                                std::chrono::steady_clock::time_point now,
                                                std::chrono::seconds interval,
                                                size_t pipelines_needed) {
    using TimePoint = std::chrono::steady_clock::time_point;
    if (!state.last_failed) {
        if (state.pipelines < pipelines_needed) {
            return false;
        }
        if (state.last_saved != TimePoint{} && now - state.last_saved < interval) {
            return false;
        }
    }
    return state.last_attempt == TimePoint{} ||
           now - state.last_attempt >= PipelineCacheFlushRetryFloor;
}

/// The timed flush's interval: a second per MiB of the cache last read, and no less than the
/// retry floor, as nx2 had it.
[[nodiscard]] inline std::chrono::seconds PipelineCacheFlushInterval(size_t cache_bytes) {
    return std::chrono::seconds{std::max<std::chrono::seconds::rep>(
        PipelineCacheFlushRetryFloor.count(),
        static_cast<std::chrono::seconds::rep>(cache_bytes / (1024 * 1024)))};
}

/// How a save of the driver pipeline cache ended.
enum class PipelineCacheSaveOutcome {
    /// All of the cache was written.
    Whole,
    /// Part of it was written: pipelines were built into it while it was read.
    Partial,
    /// The driver gave nothing usable, so the file was kept: a build overtook every read
    /// (MoltenVK), or it reported less than a header.
    NothingUsable,
    /// Reading it failed with an error, or the file could not be opened, written or replaced.
    Failed,
};

/// The flush state after an attempt queued at `queued` ended at `ended`. Only a read that got
/// nothing usable is tried again soon, at the retry floor: that is the failure a build overtaking
/// the read makes, and it passes. Any other attempt counts as a flush, as on nx2, whether it was
/// written or not: one that failed to write (a full disk, a directory it cannot write) would fail
/// again, and reading a large cache whole every floor would stall frames for nothing.
[[nodiscard]] inline PipelineCacheFlushState AfterPipelineCacheSave(
    PipelineCacheFlushState state, PipelineCacheSaveOutcome outcome,
    std::chrono::steady_clock::time_point queued, std::chrono::steady_clock::time_point ended) {
    state.last_attempt = ended;
    state.last_failed = outcome == PipelineCacheSaveOutcome::NothingUsable;
    if (!state.last_failed) {
        state.last_saved = queued;
    }
    return state;
}

} // namespace Vulkan
