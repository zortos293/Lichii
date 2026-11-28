// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <utility>

#include "common/assert.h"

namespace VideoCommon {

/// Append-only storage for the query cache's queries and banks. One thread appends (the thread
/// that records reports); other threads (the fencing thread landing results, freeing queries)
/// read elements by index while it does, for indices handed to them through a lock, or through
/// an atomic stored with release and loaded with acquire.
/// A std::deque does not allow that: an append can move its map of blocks, and a read that
/// indexes the old map at the same time follows freed memory. Here nothing an index leads to ever
/// moves: elements live in segments of FirstSegment, 2 * FirstSegment, 4 * FirstSegment, ...
/// elements, each allocated once, and the segments' addresses in an array inside the object.
/// Each address is written before any index into its segment is handed out, so the handoff that
/// orders an append before a read orders it too, and the loads need no ordering of their own.
template <typename T, std::size_t FirstSegment = 256>
class StableSlots {
    static_assert(std::has_single_bit(FirstSegment), "FirstSegment must be a power of two");

public:
    StableSlots() = default;

    ~StableSlots() {
        const std::size_t count_now = count.load(std::memory_order_relaxed);
        for (std::size_t index = 0; index < count_now; ++index) {
            std::destroy_at(Find(index));
        }
        for (std::size_t segment = 0; segment < Segments; ++segment) {
            if (T* const base = segments[segment].load(std::memory_order_relaxed)) {
                ::operator delete(base, std::align_val_t{alignof(T)});
            }
        }
    }

    StableSlots(const StableSlots&) = delete;
    StableSlots& operator=(const StableSlots&) = delete;

    /// Any thread: the elements appended so far, at least those whose indices it was handed.
    [[nodiscard]] std::size_t size() const noexcept {
        return count.load(std::memory_order_relaxed);
    }

    /// The appending thread only. The new element is at index size() - 1 once this returns.
    template <typename... Args>
    T& emplace_back(Args&&... args) {
        const std::size_t index = count.load(std::memory_order_relaxed);
        const auto [segment, offset] = Locate(index);
        T* base = segments[segment].load(std::memory_order_relaxed);
        if (base == nullptr) {
            // Kept if the constructor below throws: the next append constructs there again.
            base = static_cast<T*>(::operator new(sizeof(T) * (FirstSegment << segment),
                                                  std::align_val_t{alignof(T)}));
            segments[segment].store(base, std::memory_order_relaxed);
        }
        T* const element = std::construct_at(base + offset, std::forward<Args>(args)...);
        count.store(index + 1, std::memory_order_relaxed);
        return *element;
    }

    /// Any thread, for an index handed to it (as above) after its append.
    T& operator[](std::size_t index) noexcept {
        return *Find(index);
    }

    const T& operator[](std::size_t index) const noexcept {
        return *Find(index);
    }

private:
    /// Together they hold FirstSegment * (2^Segments - 1) elements: every index a size_t holds
    /// but the top FirstSegment, more than an append can reach.
    static constexpr std::size_t Segments =
        std::numeric_limits<std::size_t>::digits - std::countr_zero(FirstSegment);

    /// Segment k holds the indices [FirstSegment * (2^k - 1), FirstSegment * (2^(k+1) - 1)).
    static constexpr std::pair<std::size_t, std::size_t> Locate(std::size_t index) noexcept {
        const std::size_t segment = std::bit_width(index / FirstSegment + 1) - 1;
        return {segment, index - FirstSegment * ((std::size_t{1} << segment) - 1)};
    }

    T* Find(std::size_t index) const noexcept {
        const auto [segment, offset] = Locate(index);
        DEBUG_ASSERT(segment < Segments);
        return std::launder(segments[segment].load(std::memory_order_relaxed) + offset);
    }

    std::array<std::atomic<T*>, Segments> segments{};
    std::atomic<std::size_t> count{};
};

} // namespace VideoCommon
