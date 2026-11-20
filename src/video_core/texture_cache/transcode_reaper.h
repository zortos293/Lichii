// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <utility>

#include "common/common_types.h"

namespace VideoCommon {

/// Recycles completed decode images within the idle budget. Completion is queried at most once,
/// after consuming any already-known complete prefix; no wait for unfinished GPU work is needed.
template <typename SchedulerType, typename PendingQueue, typename FreeList>
void ReapFinishedTranscodes(SchedulerType& scheduler, PendingQueue& pending_transcodes,
                            FreeList& free_transcodes, u64 max_free_bytes) {
    const auto decode_bytes = [](const auto& t) {
        u64 bytes{};
        for (u32 level = 0; level < t.levels; ++level) {
            bytes += u64{(std::max)(t.width >> level, 1U)} * (std::max)(t.height >> level, 1U) * 4;
        }
        return bytes * t.layers;
    };
    u64 free_bytes{};
    for (const auto& t : free_transcodes) {
        free_bytes += decode_bytes(t);
    }
    bool refreshed = false;
    while (!pending_transcodes.empty()) {
        if (!scheduler.IsFree(pending_transcodes.front().tick)) {
            if (refreshed) {
                break;
            }
            scheduler.GetMasterSemaphore().Refresh();
            refreshed = true;
            if (!scheduler.IsFree(pending_transcodes.front().tick)) {
                break;
            }
        }
        auto& done = pending_transcodes.front();
        const u64 bytes = decode_bytes(done);
        if (free_bytes + bytes <= max_free_bytes) {
            done.unused_frames = 0;
            free_bytes += bytes;
            free_transcodes.push_back(std::move(done));
        }
        pending_transcodes.pop_front();
    }
}

} // namespace VideoCommon
