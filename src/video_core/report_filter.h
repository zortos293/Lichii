// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>

#include "common/common_types.h"

namespace VideoCore::ReportFilter {

// Which counter reports the split's front has forwarded to the back whose guest write has not
// happened yet. The front counts each report it forwards in `issued[slot]`; the query cache counts
// it in `done[slot]` when its guest write is finished (written, discarded or never needed). A slot
// is a 16-byte block of the report's GPU address, hashed into 64K entries. issued != done means a
// report on that block, or on one that hashes with it, has not finished: a collision only makes a
// read look pending, never the opposite. Counts are unsigned and wrap.
//
// The counters of Tegra::Nx2AsyncReportsSettled start when the back issues the report, so a report
// the front sent and the back has not reached yet counts as settled. These start on the front.
inline constexpr u32 Slots = 1u << 16;
inline std::array<std::atomic<u32>, Slots> g_issued{};
inline std::array<std::atomic<u32>, Slots> g_done{};

/// Blocks (hashed) where a macro DMA read found a report pending: the next report written there
/// is followed by a submission, so its value is there when the read comes.
inline std::array<std::atomic<bool>, Slots> g_hot{};
inline std::atomic<u32> g_hot_count{};

/// Set once a wait for a report ran out of time: the filter is not trusted for the rest of the
/// session and reads take the paths they took without it.
inline std::atomic<bool> g_gave_up{false};

/// The front waits for reports on these.
inline std::atomic<bool> g_waiting{false};
inline std::mutex g_wait_mutex;
inline std::condition_variable g_wait_cv;

[[nodiscard]] constexpr u32 Slot(u64 block) {
    return static_cast<u32>((block ^ (block >> 16) ^ (block >> 32)) & (Slots - 1));
}

inline void Add(std::array<std::atomic<u32>, Slots>& table, u64 address) {
    const u64 first = address >> 4;
    const u64 last = (address + 15) >> 4;
    table[Slot(first)].fetch_add(1, std::memory_order_release);
    if (last != first) {
        table[Slot(last)].fetch_add(1, std::memory_order_release);
    }
}

/// True on the back while it runs a report the front forwarded (and counted in Issue): only those
/// are counted as finished. Reports from the DMA engine, the puller, the compute engine and
/// Maxwell3D without a front reach the same code but never pass the front's counter.
inline thread_local bool t_counted = false;

/// Front: a report is about to be forwarded to the back. Returns whether its block is hot.
[[nodiscard]] inline bool Issue(u64 address) {
    Add(g_issued, address);
    return g_hot[Slot(address >> 4)].load(std::memory_order_relaxed);
}

/// A report's guest write is finished. Wakes the front when it waits (a missed wake only costs
/// the 1 ms slice its wait is made of).
inline void Done(u64 address) {
    Add(g_done, address);
    if (g_waiting.load(std::memory_order_seq_cst)) [[unlikely]] {
        { std::scoped_lock lock{g_wait_mutex}; }
        g_wait_cv.notify_all();
    }
}

/// Front: whether a read of [source, source + size) may overlap a report that is not finished.
[[nodiscard]] inline bool Pending(u64 source, u64 size) {
    if (size == 0) {
        return false;
    }
    const u64 first = source >> 4;
    const u64 last = (source + size - 1) >> 4;
    for (u64 block = first; block <= last; ++block) {
        const u32 slot = Slot(block);
        if (g_done[slot].load(std::memory_order_acquire) !=
            g_issued[slot].load(std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

/// Front: marks the blocks of [source, source + size) that hold an unfinished report as hot.
inline void MarkHot(u64 source, u64 size) {
    if (size == 0) {
        return;
    }
    if (g_hot_count.load(std::memory_order_relaxed) > 512) {
        // Stale marks (collisions, addresses no longer read): start over.
        for (auto& hot : g_hot) {
            hot.store(false, std::memory_order_relaxed);
        }
        g_hot_count.store(0, std::memory_order_relaxed);
    }
    const u64 first = source >> 4;
    const u64 last = (source + size - 1) >> 4;
    for (u64 block = first; block <= last; ++block) {
        const u32 slot = Slot(block);
        if (g_done[slot].load(std::memory_order_acquire) !=
                g_issued[slot].load(std::memory_order_relaxed) &&
            !g_hot[slot].exchange(true, std::memory_order_relaxed)) {
            g_hot_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

/// A new emulation session starts: reports the previous one abandoned are not pending, and a
/// wait that gave up there does not stop this one from waiting.
inline void Reset() {
    for (auto& count : g_issued) {
        count.store(0, std::memory_order_relaxed);
    }
    for (auto& count : g_done) {
        count.store(0, std::memory_order_relaxed);
    }
    for (auto& hot : g_hot) {
        hot.store(false, std::memory_order_relaxed);
    }
    g_hot_count.store(0, std::memory_order_relaxed);
    g_gave_up.store(false, std::memory_order_relaxed);
}

} // namespace VideoCore::ReportFilter
