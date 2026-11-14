// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon {

// Completed buffer bytes stay private until an ordinary command-read buffer flush.
class CommandReadSnapshots {
public:
    static constexpr u64 PageSize = 4096;
    static constexpr size_t Capacity = 512;
    static constexpr size_t BatchPages = 16;
    static constexpr size_t MaxPendingBatches = 32;
    static constexpr size_t MaxPendingPages = MaxPendingBatches * BatchPages;
    struct Token {
        DAddr page;
        u64 generation;
    };
    struct Counters {
        u64 hits{}, misses{}, served{}, prefetched{}, invalidated{}, evicted{}, discarded{};
        u64 verified{}, verified_bytes{}, mismatch_bytes{}, fallbacks{};
        u64 async_batches{}, async_pages{}, async_ready{}, async_discarded{}, async_cpu_discarded{};
        u64 async_hits{}, async_pending_misses{}, async_unsubmitted_misses{}, async_capacity_skips{};
        u64 async_duplicate_skips{}, async_partial_skips{}, async_peak_pages{};
    } counters;

    static bool Verify() {
        static const bool value = Flag("EDEN_NX2_COMMAND_PREFETCH_VERIFY");
        return value;
    }

    void Learn(DAddr address, u64 size) {
        const DAddr end = address + size;
        size_t count = 0;
        for (DAddr page = address & ~(PageSize - 1); page < end && count < Capacity;
             page += PageSize, ++count) {
            if (pages.contains(page)) {
                continue;
            }
            if (pages.size() == Capacity) {
                pages.erase(fifo[next]);
                ++counters.evicted;
            }
            fifo[next] = page;
            next = (next + 1) % Capacity;
            pages.emplace(page, Entry{.generation = ++generation});
        }
    }

    void Invalidate(DAddr address, u64 size) {
        if (size == 0 || pages.empty()) {
            return;
        }
        const DAddr end = address + size;
        for (auto it = pages.lower_bound(address & ~(PageSize - 1));
             it != pages.end() && it->first < end; ++it) {
            Invalidate(it->second);
        }
    }
    void InvalidateAll() {
        for (auto& [page, entry] : pages) {
            Invalidate(entry);
        }
    }

    bool Covers(DAddr address, u64 size) const {
        const DAddr end = address + size;
        for (DAddr page = address & ~(PageSize - 1); page < end; page += PageSize) {
            const auto it = pages.find(page);
            if (it == pages.end() || it->second.bytes.empty()) {
                return false;
            }
        }
        return true;
    }
    bool Copy(DAddr address, std::span<u8> out) const {
        bool asynchronous = false;
        while (!out.empty()) {
            const DAddr page = address & ~(PageSize - 1);
            const size_t offset = address - page;
            const size_t size = (std::min<size_t>)(out.size(), PageSize - offset);
            const auto& entry = pages.at(page);
            std::memcpy(out.data(), entry.bytes.data() + offset, size);
            asynchronous |= entry.asynchronous;
            address += size;
            out = out.subspan(size);
        }
        return asynchronous;
    }

    // Round-robin selection avoids repeatedly scanning only the lowest-address ring pages.
    template <class Select>
    void Candidates(DAddr exclude, u64 size, Select&& select) {
        if (pages.empty()) {
            return;
        }
        auto it = pages.upper_bound(cursor);
        size_t accepted = 0;
        for (size_t visited = 0; visited < pages.size() && accepted < BatchPages; ++visited) {
            if (it == pages.end()) {
                it = pages.begin();
            }
            const auto& [page, entry] = *it++;
            cursor = page;
            if (!entry.bytes.empty() || (page < exclude + size && exclude < page + PageSize)) {
                continue;
            }
            if (entry.pending_tick != 0) {
                ++counters.async_duplicate_skips;
                continue;
            }
            accepted += select(Token{page, entry.generation});
        }
    }

    void BeginPending(Token token, u64 tick) {
        const auto it = pages.find(token.page);
        if (it != pages.end() && it->second.generation == token.generation) {
            it->second.pending_tick = tick;
        }
    }
    bool EndPending(Token token) {
        const auto it = pages.find(token.page);
        if (it == pages.end() || it->second.generation != token.generation) {
            return false;
        }
        it->second.pending_tick = 0;
        return true;
    }
    u64 PendingTick(DAddr address, u64 size) const {
        u64 tick = 0;
        for (auto it = pages.lower_bound(address & ~(PageSize - 1));
             it != pages.end() && it->first < address + size; ++it) {
            tick = (std::max)(tick, it->second.pending_tick);
        }
        return tick;
    }

    bool Store(Token token, std::span<const u8> bytes, bool asynchronous = false) {
        const auto it = pages.find(token.page);
        if (it == pages.end() || it->second.generation != token.generation ||
            bytes.size() != PageSize) {
            ++counters.discarded;
            return false;
        }
        it->second.bytes.assign(bytes.begin(), bytes.end());
        it->second.asynchronous = asynchronous;
        ++counters.prefetched;
        return true;
    }

    void Report(bool verify, size_t pending_batches = 0, size_t pending_pages = 0,
                size_t retired_sources = 0, u64 retired_bytes = 0) {
        if (!verify) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report < std::chrono::seconds{10}) {
            return;
        }
        LOG_INFO(HW_GPU, "Command snapshots learned={} capacity={} batch_pages={} hits={} misses={} "
                        "served_without_download={} prefetched={} invalidated={} evicted={} discarded={} "
                        "verify={} verified_reads={} verified_bytes={} mismatch_bytes={} verification_fallbacks={}",
                 pages.size(), Capacity, BatchPages, counters.hits, counters.misses, counters.served,
                 counters.prefetched, counters.invalidated, counters.evicted, counters.discarded,
                 verify, counters.verified, counters.verified_bytes, counters.mismatch_bytes, counters.fallbacks);
        LOG_INFO(HW_GPU,
                 "Command async snapshots batches={} pages={} ready={} discarded={} "
                 "cpu_discarded={} hits={} pending_misses={} unsubmitted_misses={} "
                 "capacity_skips={} duplicate_skips={} partial_skips={} peak_pages={} "
                 "pending_batches={} pending_pages={} retired_sources={} retired_bytes={}",
                 counters.async_batches, counters.async_pages, counters.async_ready,
                 counters.async_discarded, counters.async_cpu_discarded, counters.async_hits,
                 counters.async_pending_misses, counters.async_unsubmitted_misses,
                 counters.async_capacity_skips, counters.async_duplicate_skips,
                 counters.async_partial_skips,
                 (std::max<u64>)(counters.async_peak_pages, pending_pages), pending_batches,
                 pending_pages, retired_sources, retired_bytes);
        counters = {};
        last_report = now;
    }

private:
    struct Entry {
        u64 generation;
        std::vector<u8> bytes;
        u64 pending_tick{};
        bool asynchronous{};
    };
    static bool Flag(const char* name) {
        const char* value = std::getenv(name);
        return value && value[0] == '1' && value[1] == '\0';
    }
    void Invalidate(Entry& entry) {
        counters.invalidated += !entry.bytes.empty();
        entry.bytes.clear();
        entry.pending_tick = 0;
        entry.generation = ++generation;
    }
    std::map<DAddr, Entry> pages;
    std::array<DAddr, Capacity> fifo{};
    size_t next{};
    DAddr cursor{};
    u64 generation{};
    std::chrono::steady_clock::time_point last_report{std::chrono::steady_clock::now()};
};

} // namespace VideoCommon
