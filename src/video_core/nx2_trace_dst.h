// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>

#include "common/common_types.h"

namespace VideoCommon {

/// EDEN_NX2_TRACE_DST=<hex>: engine writes and texture cache images starting within 32 MiB above
/// that GPU address are logged, at most `limit` lines per call site.
inline bool TraceDst(GPUVAddr address, std::atomic<u32>& lines, u32 limit = 512) {
    static const u64 begin = [] {
        const char* value = std::getenv("EDEN_NX2_TRACE_DST");
        return value != nullptr ? std::strtoull(value, nullptr, 16) : 0ULL;
    }();
    return begin != 0 && address >= begin && address < begin + (32ULL << 20) &&
           lines.fetch_add(1, std::memory_order_relaxed) < limit;
}

} // namespace VideoCommon
