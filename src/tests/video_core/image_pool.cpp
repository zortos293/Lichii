// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/image_pool.h"

namespace {

// Each fake image has an id; its destructor counts how many times that id was destroyed.
std::map<int, int> g_destroyed;

struct FakeImage {
    FakeImage() = default;
    explicit FakeImage(int id_) : id{id_} {}
    FakeImage(FakeImage&& other) noexcept : id{std::exchange(other.id, 0)} {}
    FakeImage& operator=(FakeImage&& other) noexcept {
        Reset();
        id = std::exchange(other.id, 0);
        return *this;
    }
    ~FakeImage() {
        Reset();
    }
    explicit operator bool() const noexcept {
        return id != 0;
    }
    void Reset() noexcept {
        if (id != 0) {
            ++g_destroyed[id];
            id = 0;
        }
    }
    int id = 0;
};

// The allocator fails the allocation numbered g_fail_at (from 0, counted in g_allocations), and
// with g_fail_rest every one after it too (memory that has run out), or none while it is negative.
long g_allocations = 0;
long g_fail_at = -1;
bool g_fail_rest = false;

template <typename T>
struct FailingAllocator {
    using value_type = T;
    FailingAllocator() = default;
    template <typename U>
    FailingAllocator(const FailingAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) {
        const long index = g_allocations++;
        if (g_fail_at >= 0 && (index == g_fail_at || (g_fail_rest && index > g_fail_at))) {
            throw std::bad_alloc{};
        }
        return std::allocator<T>{}.allocate(n);
    }
    void deallocate(T* p, std::size_t n) noexcept {
        std::allocator<T>{}.deallocate(p, n);
    }
    template <typename U>
    bool operator==(const FailingAllocator<U>&) const noexcept {
        return true;
    }
};

using Pool = VideoCommon::ImagePool<FakeImage, int, FailingAllocator>;

constexpr u64 MiB = 1ULL << 20;

// The counters match the entries they count, and every image put is either in the pool exactly
// once or destroyed exactly once.
void CheckConsistent(Pool& pool, int images_put) {
    u64 bytes = 0;
    u64 doomed_bytes = 0;
    std::map<int, int> held;
    for (const auto& entry : pool.entries) {
        bytes += entry.bytes;
        REQUIRE(entry.image);
        ++held[entry.image.id];
    }
    for (const auto& entry : pool.doomed) {
        doomed_bytes += entry.bytes;
        REQUIRE(entry.image);
        ++held[entry.image.id];
    }
    CHECK(pool.bytes == bytes);
    CHECK(pool.doomed_bytes == doomed_bytes);
    for (int id = 1; id <= images_put; ++id) {
        const int destroyed = g_destroyed.contains(id) ? g_destroyed[id] : 0;
        CHECK(held[id] + destroyed == 1);
    }
}

void Reset() {
    g_destroyed.clear();
    g_allocations = 0;
    g_fail_at = -1;
    g_fail_rest = false;
}

// Puts images first..first + count - 1, of `size` bytes each, under key `id % 3`.
void Fill(Pool& pool, int first, int count, u64 size) {
    for (int id = first; id < first + count; ++id) {
        pool.Put(id % 3, FakeImage{id}, size);
    }
}

// How many allocations `run` makes on a fresh pool set up by `setup`, with none failing.
template <typename Setup, typename Run>
long CountAllocations(Setup setup, Run run) {
    Reset();
    Pool pool{8 * MiB};
    setup(pool);
    const long before = g_allocations;
    run(pool);
    return g_allocations - before;
}

// Fails each allocation `run` makes in turn (and one past them, where none fails), and checks the
// pool after each: once that allocation alone, and once it and every one after it (memory that has
// run out from there on). Returns how many allocations `run` makes.
template <typename Setup, typename Run>
long FailEachAllocation(int images, Setup setup, Run run) {
    const long count = CountAllocations(setup, run);
    for (long k = 0; k <= 2 * count + 1; ++k) {
        Reset();
        {
            Pool pool{8 * MiB};
            setup(pool);
            g_fail_at = g_allocations + k / 2;
            g_fail_rest = k % 2 == 1;
            run(pool);
            g_fail_at = -1;
            g_fail_rest = false;
            CheckConsistent(pool, images);
            // Whatever is left can still be trimmed away completely.
            pool.Trim(0);
            CHECK(pool.entries.empty());
            CHECK(pool.doomed.empty());
            CHECK(pool.bytes == 0);
            CHECK(pool.doomed_bytes == 0);
            CheckConsistent(pool, images);
        }
    }
    return count;
}

} // Anonymous namespace

TEST_CASE("Image pool: a put that cannot allocate destroys the image, counters intact",
          "[video_core][image_pool]") {
    // 64 puts of 1 MiB into an 8 MiB cap: each put past the cap also dooms the oldest entry.
    const long injected =
        FailEachAllocation(64, [](Pool&) {}, [](Pool& pool) { Fill(pool, 1, 64, MiB); });
    CHECK(injected > 0);
}

TEST_CASE("Image pool: an entry that cannot be doomed is destroyed at once, counters intact",
          "[video_core][image_pool]") {
    // The pool is full; then one put of the cap's size dooms all of it (each Doom may allocate).
    const long injected = FailEachAllocation(
        33, [](Pool& pool) { Fill(pool, 1, 32, MiB / 4); },
        [](Pool& pool) { pool.Put(7, FakeImage{33}, 8 * MiB); });
    CHECK(injected > 0);
}

TEST_CASE("Image pool: a take that cannot doom what it drops keeps the counters intact",
          "[video_core][image_pool]") {
    // 128 entries of 64 KiB kept (8 MiB); the cap then falls to 128 KiB, so the take's DropOld
    // dooms the oldest 126 before it looks for its key: enough to fill several of the doomed
    // deque's blocks, each Doom that starts one allocating. The image it returns is destroyed with
    // the run.
    constexpr u64 Size = MiB / 16;
    const long allocations = FailEachAllocation(
        128,
        [](Pool& pool) {
            Fill(pool, 1, 128, Size);
            pool.cap = 2 * Size;
        },
        [](Pool& pool) {
            const FakeImage taken = pool.Take(128 % 3);
            CHECK(taken);
        });
    CHECK(allocations > 0);
}

TEST_CASE("Image pool: trimming and destroying what is doomed allocate nothing",
          "[video_core][image_pool]") {
    // 8 MiB kept, then 6 MiB of it doomed: 2 MiB kept, and more doomed than the cap DestroyDoomed
    // is run with, so it goes past its count.
    const auto setup = [](Pool& pool) {
        Fill(pool, 1, 8, MiB);
        std::scoped_lock lock{pool.mutex};
        pool.cap = 2 * MiB;
        pool.DropOld(Pool::Clock::now());
        pool.cap = 8 * MiB;
        REQUIRE(pool.doomed_bytes == 6 * MiB);
    };
    CHECK(CountAllocations(setup, [](Pool& pool) { pool.Trim(0); }) == 0);
    CHECK(CountAllocations(setup, [](Pool& pool) { pool.Trim(4 * MiB); }) == 0);
    CHECK(CountAllocations(setup, [](Pool& pool) {
              pool.cap = 0;
              pool.DestroyDoomed(Pool::DestroysPerCall);
          }) == 0);
    Reset();
    Pool pool{8 * MiB};
    setup(pool);
    pool.Trim(0);
    CheckConsistent(pool, 8);
    CHECK(pool.entries.empty());
    CHECK(pool.doomed.empty());
}

TEST_CASE("Image pool: Trim keeps what it kept before", "[video_core][image_pool]") {
    Reset();
    Pool pool{8 * MiB};
    Fill(pool, 1, 8, MiB);
    pool.cap = 2 * MiB;
    {
        std::scoped_lock lock{pool.mutex};
        pool.DropOld(Pool::Clock::now());
    }
    pool.cap = 8 * MiB;
    // 2 MiB kept (images 7 and 8), 6 MiB doomed (1 to 6, oldest first).
    REQUIRE(pool.bytes == 2 * MiB);
    REQUIRE(pool.doomed_bytes == 6 * MiB);

    SECTION("kept bytes within the target: the oldest doomed go until the total is") {
        pool.Trim(5 * MiB);
        CHECK(pool.bytes == 2 * MiB);
        CHECK(pool.doomed_bytes == 3 * MiB);
        CHECK(pool.doomed.front().image.id == 4);
        CHECK(pool.trimmed == 0);
    }
    SECTION("kept bytes over the target: every doomed one goes, then the oldest kept") {
        pool.Trim(MiB);
        CHECK(pool.bytes == MiB);
        CHECK(pool.doomed.empty());
        CHECK(pool.entries.front().image.id == 8);
        CHECK(pool.trimmed == 1);
    }
    CheckConsistent(pool, 8);
}

TEST_CASE("Image pool: DestroyDoomed goes past its count while the doomed exceed the cap",
          "[video_core][image_pool]") {
    Reset();
    Pool pool{8 * MiB};
    Fill(pool, 1, 8, MiB);
    pool.cap = MiB;
    {
        std::scoped_lock lock{pool.mutex};
        pool.DropOld(Pool::Clock::now());
    }
    // 7 doomed, 7 MiB, over a 1 MiB cap: it goes until 1 MiB is left, not just two.
    pool.DestroyDoomed(Pool::DestroysPerCall);
    CHECK(pool.doomed_bytes == MiB);
    CheckConsistent(pool, 8);
}
