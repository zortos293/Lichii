// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

namespace VideoCommon {

// Bound both CPU work and evictions. Candidates the pass can't evict (decoding, costly to load
// again, waiting for a download) use only scan work: they must not keep the collector from the
// evictable images behind them in the same LRU window, and the walk past them stays bounded.
struct CollectionBudget {
    size_t deletions{};
    size_t scans{};

    void Configure(size_t count) {
        deletions = count;
        scans = count * 8;
    }

    /// One more candidate examined; false once the pass may examine or delete no more.
    bool Examine() {
        if (deletions == 0 || scans == 0) {
            return false;
        }
        --scans;
        return true;
    }

    void Deleted() {
        if (deletions != 0) {
            --deletions;
        }
    }
};

} // namespace VideoCommon
