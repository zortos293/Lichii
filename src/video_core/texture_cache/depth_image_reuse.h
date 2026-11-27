// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <utility>

#include "common/common_types.h"

namespace VideoCommon {

constexpr bool AllowDepthImageReuse(u64 program_id, bool radv, bool reports_budget) {
    return program_id == 0x0400380020056000ULL && radv && reports_budget;
}

struct DepthImageReuseKey {
    u32 width;
    u32 height;
    u32 layers;

    bool operator==(const DepthImageReuseKey&) const = default;
};

template <typename Resource>
class DepthImageReusePool {
public:
    static constexpr u64 MaxBytes = u64{128} << 20;
    static constexpr u64 MaxAge = 30;
    static constexpr size_t MaxImages = 4;

    Resource Acquire(DepthImageReuseKey key) {
        const auto it = std::ranges::find(entries, key, &Entry::key);
        if (it == entries.end()) {
            ++misses;
            return {};
        }
        Resource resource = std::move(it->resource);
        bytes -= it->bytes;
        entries.erase(it);
        ++hits;
        return resource;
    }

    bool Recycle(DepthImageReuseKey key, Resource& resource, u64 charge) {
        if (charge == 0 || charge > MaxBytes) {
            return false;
        }
        while (bytes > MaxBytes - charge || entries.size() == MaxImages) {
            DropOldest();
        }
        entries.push_back({key, std::move(resource), charge, frame});
        bytes += charge;
        ++recycled;
        return true;
    }

    void Tick() {
        ++frame;
        while (!entries.empty() && frame - entries.front().frame >= MaxAge) {
            DropOldest();
        }
    }

    void Clear() {
        entries.clear();
        bytes = 0;
    }

    u64 Bytes() const { return bytes; }
    size_t Size() const { return entries.size(); }
    u64 Hits() const { return hits; }
    u64 Misses() const { return misses; }
    u64 Recycled() const { return recycled; }

private:
    struct Entry {
        DepthImageReuseKey key;
        Resource resource;
        u64 bytes;
        u64 frame;
    };

    void DropOldest() {
        bytes -= entries.front().bytes;
        entries.pop_front();
    }

    std::deque<Entry> entries;
    u64 bytes{};
    u64 frame{};
    u64 hits{};
    u64 misses{};
    u64 recycled{};
};

} // namespace VideoCommon
