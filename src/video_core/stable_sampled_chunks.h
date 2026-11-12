// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <span>

namespace VideoCommon::StableSampled {
using U64 = std::uint64_t;
constexpr size_t ChunkSize = 64;
constexpr size_t MaxEntries = 8192;
constexpr size_t Budget = 16 * 1024 * 1024;
inline bool Enabled() {
    static const bool enabled = [] {
        const auto* value = std::getenv("LICHII_STABLE_IMAGE_BINDINGS");
        return value && value[0] == '1';
    }();
    return enabled;
}
inline bool Validate() {
    static const bool enabled = [] {
        const auto* value = std::getenv("LICHII_STABLE_IMAGE_BINDINGS_VALIDATE");
        return value && value[0] == '1';
    }();
    return enabled;
}
inline std::atomic<U64>& ResourceEpoch() {
    static std::atomic<U64> epoch{1};
    return epoch;
}
inline void InvalidateResources() {
    ResourceEpoch().fetch_add(1, std::memory_order_relaxed);
}
inline U64 Epoch() {
    return ResourceEpoch().load(std::memory_order_relaxed);
}
inline std::atomic<size_t>& ReservedBytes() {
    static std::atomic<size_t> bytes{};
    return bytes;
}
inline std::atomic<bool>& FailedValidation() {
    static std::atomic<bool> failed{};
    return failed;
}

template <class Value>
class ChunkCache {
public:
    struct Chunk {
        U64 epoch{};
        size_t count{};
        bool rescaled{};
        std::array<U64, ChunkSize> keys{};
        std::array<Value, ChunkSize> values{};
    };
    ChunkCache() = default;
    ChunkCache(const ChunkCache&) = delete;
    ChunkCache& operator=(const ChunkCache&) = delete;
    ~ChunkCache() {
        ReservedBytes().fetch_sub(reserved, std::memory_order_relaxed);
    }

    bool Ensure(size_t count) {
        if (chunks)
            return count <= capacity;
        if (!count || count > 2 * MaxEntries / ChunkSize)
            return false;
        const size_t needed = count * sizeof(Chunk);
        auto& total = ReservedBytes();
        size_t before = total.load(std::memory_order_relaxed);
        do {
            if (needed > Budget || before > Budget - needed)
                return false;
        } while (!total.compare_exchange_weak(before, before + needed, std::memory_order_relaxed));
        chunks.reset(new (std::nothrow) Chunk[count]);
        if (!chunks) {
            total.fetch_sub(needed, std::memory_order_relaxed);
            return false;
        }
        reserved = needed;
        capacity = count;
        return true;
    }
    const Chunk* Find(size_t index, U64 epoch, std::span<const U64> keys) const {
        if (!chunks || index >= capacity || keys.empty() || keys.size() > ChunkSize)
            return nullptr;
        const auto& chunk = chunks[index];
        if (chunk.epoch != epoch || chunk.count != keys.size() ||
            !std::equal(keys.begin(), keys.end(), chunk.keys.begin()))
            return nullptr;
        return &chunk;
    }
    void Store(size_t index, U64 epoch, std::span<const U64> keys, std::span<const Value> values,
               bool rescaled) {
        if (!chunks || index >= capacity || keys.empty() || keys.size() > ChunkSize ||
            keys.size() != values.size())
            return;
        auto& chunk = chunks[index];
        chunk.epoch = epoch;
        chunk.count = keys.size();
        chunk.rescaled = rescaled;
        std::copy(keys.begin(), keys.end(), chunk.keys.begin());
        std::copy(values.begin(), values.end(), chunk.values.begin());
    }
    void Clear(size_t index) {
        if (chunks && index < capacity)
            chunks[index].epoch = 0;
    }

private:
    std::unique_ptr<Chunk[]> chunks;
    size_t capacity{}, reserved{};
};
} // namespace VideoCommon::StableSampled
