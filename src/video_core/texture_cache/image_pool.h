// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <type_traits>

#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon {

/// The pool behind EDEN_NX2_IMAGE_POOL: host images of destroyed cache images, kept for a few
/// seconds so that a new image with the same key can take one instead of being made. Generic over
/// the image (moved, tested for emptiness, destroyed by its destructor) and the allocator, so that
/// a test can fail every allocation in turn.
///
/// Memory pressure: Put, Maintain, Trim and DestroyDoomed do not throw, and Trim and DestroyDoomed
/// do not allocate. Put is reached from an image's destructor and Trim from the runtime's, so an
/// allocation failure there would end the process. When an allocation fails:
/// - Put does not keep the image: it is destroyed at once, as without the pool;
/// - an entry that cannot move to the images still to destroy is destroyed at once instead, under
///   the lock (only then).
/// The byte counters always equal the bytes of the entries they count.
template <typename Image, typename Key, template <typename> typename Allocator = std::allocator>
struct ImagePool {
    using Clock = std::chrono::steady_clock;
    struct Entry {
        Key key;
        Image image;
        u64 bytes;
        Clock::time_point at;
    };
    using Entries = std::deque<Entry, Allocator<Entry>>;
    // Doom, Put and the destroy loops rely on these: an entry or image that moves only by
    // allocation failure would make their noexcept a terminate.
    static_assert(std::is_nothrow_move_constructible_v<Entry> &&
                      std::is_nothrow_move_assignable_v<Image>,
                  "the pool's entries and images must move without throwing");

    static constexpr auto MaxAge = std::chrono::seconds{5};
    /// Images that left the pool (expired, over the cap, trimmed) are destroyed a few at a time by
    /// the pool's later calls, outside its lock, instead of all at once where they left.
    static constexpr size_t DestroysPerCall = 2;

    explicit ImagePool(u64 fixed_cap_ = 0)
        : fixed_cap{fixed_cap_}, cap{fixed_cap_ != 0 ? fixed_cap_ : 256ULL << 20} {}

    std::mutex mutex;
    Entries entries;
    Entries doomed;
    u64 bytes = 0;
    u64 doomed_bytes = 0;
    /// EDEN_NX2_IMAGE_POOL_MIB fixes the cap; else the texture cache sets it (SetImagePoolCap).
    const u64 fixed_cap;
    u64 cap;
    // Per window, for the log.
    u64 hits = 0, misses = 0, puts = 0, expired = 0, trimmed = 0, destroyed = 0, not_kept = 0;
    Clock::time_point last_log = Clock::now();

    /// `mutex` held: the oldest entry leaves for `doomed`, or is destroyed at once if that cannot
    /// allocate. The counters change only once it has moved.
    void Doom() noexcept {
        Entry& front = entries.front();
        try {
            // No effect if it throws: only the allocation can (an entry moves without throwing).
            doomed.push_back(std::move(front));
        } catch (...) {
            bytes -= front.bytes;
            ++destroyed;
            entries.pop_front();
            return;
        }
        bytes -= doomed.back().bytes;
        doomed_bytes += doomed.back().bytes;
        entries.pop_front();
    }

    /// `mutex` held.
    void DropOld(Clock::time_point now) noexcept {
        while (!entries.empty() && (bytes > cap || now - entries.front().at > MaxAge)) {
            ++expired;
            Doom();
        }
    }

    /// Destroys up to `count` doomed images, and more while the doomed ones hold more than the cap:
    /// the pool's own bytes and those still to destroy stay within twice the cap however the
    /// images that leave it are sized. One at a time: taken under the lock, destroyed after it.
    void DestroyDoomed(size_t count) noexcept {
        for (size_t taken = 0;; ++taken) {
            Image image;
            {
                std::scoped_lock lock{mutex};
                if (doomed.empty() || (taken >= count && doomed_bytes <= cap)) {
                    return;
                }
                doomed_bytes -= doomed.front().bytes;
                image = std::move(doomed.front().image);
                doomed.pop_front();
                ++destroyed;
            }
            // `image` is destroyed here.
        }
    }

    /// `mutex` held.
    void MaybeLog(Clock::time_point now) {
        if (now - last_log < std::chrono::seconds{10}) {
            return;
        }
        const double s = std::chrono::duration<double>(now - last_log).count();
        LOG_INFO(Render_Vulkan,
                 "image pool {:.1f}s (per s): taken {:.1f}, made new {:.1f}, kept {:.1f}, expired "
                 "{:.1f}, trimmed {}, destroyed {:.1f}, not kept for lack of memory {} | now {} "
                 "images {} MiB, {} to destroy {} MiB",
                 s, hits / s, misses / s, puts / s, expired / s, trimmed, destroyed / s, not_kept,
                 entries.size(), bytes >> 20, doomed.size(), doomed_bytes >> 20);
        hits = misses = puts = expired = trimmed = destroyed = not_kept = 0;
        last_log = now;
    }

    Image Take(const Key& key) {
        Image image;
        {
            std::scoped_lock lock{mutex};
            const auto now = Clock::now();
            DropOld(now);
            MaybeLog(now);
            for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
                if (it->key == key) {
                    image = std::move(it->image);
                    bytes -= it->bytes;
                    entries.erase(std::next(it).base());
                    ++hits;
                    break;
                }
            }
            if (!image) {
                ++misses;
            }
        }
        DestroyDoomed(DestroysPerCall);
        return image;
    }

    /// Keeps `image` (of `size` bytes) if it fits. Best effort: an image that is not kept (too
    /// large, or no memory for its entry) is destroyed, after the lock, as without the pool.
    void Put(const Key& key, Image&& image, u64 size) noexcept {
        Entry entry{key, std::move(image), size, {}};
        {
            std::scoped_lock lock{mutex};
            if (size == 0 || size > cap) {
                return;
            }
            const auto now = Clock::now();
            entry.at = now;
            try {
                // No effect if it throws: `entry` still holds the image.
                entries.push_back(std::move(entry));
            } catch (...) {
                ++not_kept;
                return;
            }
            bytes += size;
            ++puts;
            DropOld(now);
        }
        DestroyDoomed(DestroysPerCall);
    }

    /// Once a frame: expires what is past MaxAge, then destroys a few that left, so the pool
    /// lets go of its images also when no image is made or collected.
    void Maintain() noexcept {
        {
            std::scoped_lock lock{mutex};
            DropOld(Clock::now());
        }
        DestroyDoomed(DestroysPerCall);
    }

    /// Destroys images until at most `target` bytes are kept, those still to destroy included:
    /// the texture cache asks when it needs the memory, so they are destroyed now. With 0 (memory
    /// ran out, or the device goes) every image goes. While the pool's own bytes are over
    /// `target`, every doomed image goes, then the oldest entries until they are not; otherwise
    /// the oldest doomed ones until the total is within it. One at a time, with no container:
    /// taken under the lock, destroyed after it.
    void Trim(u64 target) noexcept {
        for (;;) {
            Image image;
            {
                std::scoped_lock lock{mutex};
                if (!doomed.empty() && bytes + doomed_bytes > target) {
                    doomed_bytes -= doomed.front().bytes;
                    image = std::move(doomed.front().image);
                    doomed.pop_front();
                } else if (!entries.empty() && bytes > target) {
                    ++trimmed;
                    bytes -= entries.front().bytes;
                    image = std::move(entries.front().image);
                    entries.pop_front();
                } else {
                    return;
                }
                ++destroyed;
            }
            // `image` is destroyed here.
        }
    }
};

} // namespace VideoCommon
