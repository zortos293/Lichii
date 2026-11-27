// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <latch>
#include <span>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon {

// Shared with queued helpers: completing all page reads need not wait for helpers that have
// not started. After Wait(), no helper may dereference a guest pointer from this job again.
class PrefaultPages {
public:
    static constexpr size_t Batch = 64;

    explicit PrefaultPages(std::vector<const u8*> pages_)
        : pages{std::move(pages_)}, remaining{static_cast<std::ptrdiff_t>(pages.size())} {}

    std::span<const u8* const> NextBatch() {
        const size_t first = cursor.fetch_add(Batch, std::memory_order_relaxed);
        if (first >= pages.size()) {
            return {};
        }
        return std::span{pages}.subspan(first, (std::min)(Batch, pages.size() - first));
    }

    void CompleteBatch(size_t count) {
        remaining.count_down(static_cast<std::ptrdiff_t>(count));
    }

    void Touch() {
        for (;;) {
            const auto batch = NextBatch();
            if (batch.empty()) {
                return;
            }
            for (const u8* page : batch) {
                static_cast<void>(*static_cast<const volatile u8*>(page));
            }
            CompleteBatch(batch.size());
        }
    }

    void Wait() const {
        remaining.wait();
    }

private:
    const std::vector<const u8*> pages;
    std::atomic<size_t> cursor{};
    std::latch remaining;
};

} // namespace VideoCommon
