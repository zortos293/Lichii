// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/pipeline_cache_data.h"

namespace {

using Vulkan::PipelineCacheData;
using Vulkan::PipelineCacheReadAttempts;

constexpr size_t Header = Vulkan::MinPipelineCacheDataSize;

// A driver pipeline cache. `grow` pipelines build into it, one after each size query, before the
// read. A read into a smaller buffer gets nothing as on MoltenVK, or, `conforming`, as much as
// fits as the spec has it (whole entries of 8 bytes after the header).
struct FakeCache {
    std::vector<char> data = std::vector<char>(Header + 16, 1);
    bool conforming = false;
    int grow = 0;
    int calls = 0;

    VkResult Read(size_t* size, void* out) {
        ++calls;
        if (out == nullptr) {
            *size = data.size();
            if (grow > 0) {
                --grow;
                data.resize(data.size() + 8, static_cast<char>(data.size()));
            }
            return VK_SUCCESS;
        }
        if (*size >= data.size()) {
            std::memcpy(out, data.data(), data.size());
            *size = data.size();
            return VK_SUCCESS;
        }
        *size = conforming && *size >= Header ? Header + (*size - Header) / 8 * 8 : 0;
        std::memcpy(out, data.data(), *size);
        return VK_INCOMPLETE;
    }
};

PipelineCacheData Read(FakeCache& cache, int* progress = nullptr) {
    return Vulkan::ReadPipelineCacheData(
        [&cache](size_t* size, void* out) { return cache.Read(size, out); },
        [progress] {
            if (progress) {
                ++*progress;
            }
        });
}

std::vector<char> Bytes(const PipelineCacheData& data) {
    return {data.bytes.get(), data.bytes.get() + data.size};
}

} // Anonymous namespace

TEST_CASE("A driver pipeline cache that holds still is read once",
          "[video_core][vulkan_pipeline_cache]") {
    FakeCache cache;
    int progress = 0;
    const auto data = Read(cache, &progress);
    CHECK(data.Whole());
    CHECK(Bytes(data) == cache.data);
    CHECK(cache.calls == 2);
    CHECK(progress == 1);
}

TEST_CASE("A driver pipeline cache that reads back fewer bytes than asked is whole",
          "[video_core][vulkan_pipeline_cache]") {
    int calls = 0;
    const auto data = Vulkan::ReadPipelineCacheData(
        [&](size_t* size, void* out) {
            ++calls;
            if (out == nullptr) {
                *size = Header + 64;
            } else {
                *size = Header + 8;
                std::memset(out, 7, *size);
            }
            return VK_SUCCESS;
        },
        [] {});
    CHECK(data.Whole());
    CHECK(data.size == Header + 8);
    CHECK(calls == 2);
}

TEST_CASE("A driver pipeline cache read that returns less than a header is no data",
          "[video_core][vulkan_pipeline_cache]") {
    // The size query said a cache, the read gave less than its header: the file is kept.
    int calls = 0;
    const auto data = Vulkan::ReadPipelineCacheData(
        [&](size_t* size, void* out) {
            ++calls;
            *size = out == nullptr ? Header + 64 : 16;
            return VK_SUCCESS;
        },
        [] {});
    CHECK_FALSE(data.Usable());
    CHECK(data.result == VK_SUCCESS);
    CHECK(calls == 2);
}

TEST_CASE("A driver pipeline cache read that reports more than its buffer is held to it",
          "[video_core][vulkan_pipeline_cache]") {
    const auto data = Vulkan::ReadPipelineCacheData(
        [](size_t* size, void* out) {
            if (out == nullptr) {
                *size = Header + 8;
            } else {
                std::memset(out, 7, *size);
                *size = Header + 4096;
            }
            return VK_SUCCESS;
        },
        [] {});
    CHECK(data.Whole());
    CHECK(data.size == Header + 8);
}

TEST_CASE("A driver pipeline cache smaller than a header is no data",
          "[video_core][vulkan_pipeline_cache]") {
    // A live cache always holds a header: a size of 0, or less than one, would replace a good
    // file with an empty cache.
    for (const size_t size : {size_t{0}, Header - 1}) {
        FakeCache cache{.data = std::vector<char>(size, 1)};
        const auto data = Read(cache);
        CHECK_FALSE(data.Usable());
        CHECK(data.result == VK_SUCCESS);
        CHECK(cache.calls == 1);
    }
}

TEST_CASE("A driver pipeline cache that returns nothing when it grows is asked again",
          "[video_core][vulkan_pipeline_cache]") {
    // MoltenVK: no bytes until the size is asked for again.
    FakeCache cache{.grow = 1};
    const auto data = Read(cache);
    CHECK(data.Whole());
    CHECK(Bytes(data) == cache.data);
    CHECK(cache.calls == 4);
}

TEST_CASE("A driver pipeline cache that returns nothing each time it grows is not saved",
          "[video_core][vulkan_pipeline_cache]") {
    // MoltenVK: what it returned would replace the file with an empty cache.
    FakeCache cache{.grow = 100};
    int progress = 0;
    const auto data = Read(cache, &progress);
    CHECK_FALSE(data.Usable());
    CHECK(data.result == VK_INCOMPLETE);
    CHECK(data.reads == PipelineCacheReadAttempts);
    CHECK(cache.calls == 2 * PipelineCacheReadAttempts);
    CHECK(progress == PipelineCacheReadAttempts);
}

TEST_CASE("A driver pipeline cache that returns part of itself when it grows saves that part",
          "[video_core][vulkan_pipeline_cache]") {
    // A conforming driver: the part is a valid, smaller cache, taken at once as nx2 took it,
    // rather than serializing the whole cache again for a result as partial.
    FakeCache cache{.conforming = true, .grow = 100};
    const auto data = Read(cache);
    REQUIRE(data.Partial());
    CHECK(data.size == Header + 16);
    CHECK(std::equal(data.bytes.get(), data.bytes.get() + data.size, cache.data.begin()));
    CHECK(cache.calls == 2);
}

TEST_CASE("A driver pipeline cache part of a header alone is asked again",
          "[video_core][vulkan_pipeline_cache]") {
    // A header and nothing after it holds no pipeline; it must not replace a full file.
    FakeCache cache{.data = std::vector<char>(Header, 1), .conforming = true, .grow = 100};
    const auto data = Read(cache);
    REQUIRE(data.Partial());
    CHECK(data.size == Header + 8);
    CHECK(cache.calls == 4);
}

TEST_CASE("A driver pipeline cache whose read fails is not saved",
          "[video_core][vulkan_pipeline_cache]") {
    int calls = 0;
    const auto failing = Vulkan::ReadPipelineCacheData(
        [&](size_t* size, void* out) {
            ++calls;
            *size = 64;
            return out == nullptr ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
        },
        [] {});
    CHECK_FALSE(failing.Usable());
    CHECK(failing.result == VK_ERROR_OUT_OF_HOST_MEMORY);
    CHECK(calls == 2);
    const auto failing_size = Vulkan::ReadPipelineCacheData(
        [](size_t*, void*) { return VK_ERROR_OUT_OF_HOST_MEMORY; }, [] {});
    CHECK_FALSE(failing_size.Usable());
    CHECK(failing_size.result == VK_ERROR_OUT_OF_HOST_MEMORY);
}

TEST_CASE("A driver pipeline cache flush waits for its pipelines and its interval",
          "[video_core][vulkan_pipeline_cache]") {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    using Vulkan::PipelineCacheFlushDue;
    const Clock::time_point start = Clock::now();
    const std::chrono::seconds interval{300}; // a 300 MB cache
    constexpr size_t Needed = 128;
    // Never saved nor tried: due once enough pipelines were built.
    CHECK(PipelineCacheFlushDue({.pipelines = Needed}, start, interval, Needed));
    CHECK_FALSE(PipelineCacheFlushDue({.pipelines = Needed - 1}, start, interval, Needed));
    // Saved a minute ago: not due until the interval has passed.
    const Vulkan::PipelineCacheFlushState saved{
        .last_saved = start, .last_attempt = start, .pipelines = Needed};
    CHECK_FALSE(PipelineCacheFlushDue(saved, start + 60s, interval, Needed));
    CHECK(PipelineCacheFlushDue(saved, start + 300s, interval, Needed));
}

TEST_CASE("A failed driver pipeline cache flush is tried again after the retry floor",
          "[video_core][vulkan_pipeline_cache]") {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    using Vulkan::PipelineCacheFlushDue;
    using Vulkan::PipelineCacheFlushRetryFloor;
    const Clock::time_point start = Clock::now();
    const std::chrono::seconds interval{300};
    constexpr size_t Needed = 128;
    // A timed flush that failed 400 s after the last good save, ending at `ended`.
    const auto ended = start + 400s;
    const Vulkan::PipelineCacheFlushState failed{
        .last_saved = start, .last_attempt = ended, .last_failed = true, .pipelines = 1};
    // Inside the floor: not due, however many pipelines.
    CHECK_FALSE(PipelineCacheFlushDue(failed, ended + 10s, interval, Needed));
    CHECK_FALSE(PipelineCacheFlushDue({.last_saved = start,
                                       .last_attempt = ended,
                                       .last_failed = true,
                                       .pipelines = Needed},
                                      ended + 10s, interval, Needed));
    // Past it: due with the next pipeline, though fewer than the count were built since.
    CHECK(PipelineCacheFlushDue(failed, ended + PipelineCacheFlushRetryFloor, interval, Needed));
    // Not failed, under the count: not due, the floor long past.
    CHECK_FALSE(PipelineCacheFlushDue({.last_saved = start, .last_attempt = ended, .pipelines = 1},
                                      ended + 600s, interval, Needed));
    // The save after the boot builds failed, with none before it: the same floor.
    const Vulkan::PipelineCacheFlushState boot_failed{
        .last_attempt = start, .last_failed = true, .pipelines = 1};
    CHECK_FALSE(PipelineCacheFlushDue(boot_failed, start + 10s, interval, Needed));
    CHECK(PipelineCacheFlushDue(boot_failed, start + PipelineCacheFlushRetryFloor, interval,
                                Needed));
}

TEST_CASE("A driver pipeline cache flush that failed to write waits out the interval, as on nx2",
          "[video_core][vulkan_pipeline_cache]") {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    using Vulkan::PipelineCacheSaveOutcome;
    const Clock::time_point queued = Clock::now();
    const auto ended = queued + 5s;
    const std::chrono::seconds interval{300};
    constexpr size_t Needed = 128;
    // A full disk, a directory it cannot write: it would fail again, and a large cache read
    // whole every floor stalls frames.
    for (const auto outcome : {PipelineCacheSaveOutcome::Failed, PipelineCacheSaveOutcome::Whole,
                               PipelineCacheSaveOutcome::Partial}) {
        auto state = Vulkan::AfterPipelineCacheSave({}, outcome, queued, ended);
        CHECK_FALSE(state.last_failed);
        state.pipelines = Needed;
        CHECK_FALSE(Vulkan::PipelineCacheFlushDue(state, ended + 60s, interval, Needed));
        CHECK(Vulkan::PipelineCacheFlushDue(state, queued + interval, interval, Needed));
    }
}

TEST_CASE("A driver pipeline cache flush that read nothing usable is due at the floor",
          "[video_core][vulkan_pipeline_cache]") {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    const Clock::time_point saved = Clock::now();
    const auto queued = saved + 400s;
    const auto ended = queued + 5s;
    auto state = Vulkan::AfterPipelineCacheSave({.last_saved = saved, .last_attempt = saved},
                                                Vulkan::PipelineCacheSaveOutcome::NothingUsable,
                                                queued, ended);
    CHECK(state.last_failed);
    CHECK(state.last_saved == saved);
    state.pipelines = 1;
    CHECK_FALSE(Vulkan::PipelineCacheFlushDue(state, ended + 10s, std::chrono::seconds{300}, 128));
    CHECK(Vulkan::PipelineCacheFlushDue(state, ended + Vulkan::PipelineCacheFlushRetryFloor,
                                        std::chrono::seconds{300}, 128));
}

TEST_CASE("A driver pipeline cache save overtaken soon after a good one is due at the floor",
          "[video_core][vulkan_pipeline_cache]") {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    // The save after the boot builds worked; the one after the precache, a minute later, was
    // overtaken by the prebuild. The cache is 600 MiB, so the interval is ten minutes.
    const Clock::time_point boot_saved = Clock::now();
    const auto ended = boot_saved + 60s;
    const auto interval = Vulkan::PipelineCacheFlushInterval(size_t{600} * 1024 * 1024);
    auto state =
        Vulkan::AfterPipelineCacheSave({.last_saved = boot_saved, .last_attempt = boot_saved},
                                       Vulkan::PipelineCacheSaveOutcome::NothingUsable, ended,
                                       ended);
    state.pipelines = 1;
    CHECK_FALSE(Vulkan::PipelineCacheFlushDue(state, ended + 10s, interval, 128));
    // The floor after the attempt, not the interval after the boot save.
    CHECK(Vulkan::PipelineCacheFlushDue(state, ended + Vulkan::PipelineCacheFlushRetryFloor,
                                        interval, 128));
    // The retry is written: the interval counts again, from when it was queued.
    const auto retried = ended + Vulkan::PipelineCacheFlushRetryFloor;
    state = Vulkan::AfterPipelineCacheSave(state, Vulkan::PipelineCacheSaveOutcome::Whole, retried,
                                           retried + 2s);
    state.pipelines = 128;
    CHECK_FALSE(Vulkan::PipelineCacheFlushDue(state, retried + 599s, interval, 128));
    CHECK(Vulkan::PipelineCacheFlushDue(state, retried + 600s, interval, 128));
}

TEST_CASE("A driver pipeline cache flush that failed to write waits the interval its size gives",
          "[video_core][vulkan_pipeline_cache]") {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    // A second per MiB of the cache last read, no less than the floor, as on nx2.
    CHECK(Vulkan::PipelineCacheFlushInterval(0) == Vulkan::PipelineCacheFlushRetryFloor);
    CHECK(Vulkan::PipelineCacheFlushInterval(size_t{10} << 20) == 30s);
    // A 600 MB cache read whole and not written (a full disk): its size is still what the
    // interval is computed from, so the next flush waits 600 s, not 30.
    const size_t read_bytes = size_t{600} << 20;
    const auto interval = Vulkan::PipelineCacheFlushInterval(read_bytes);
    CHECK(interval == 600s);
    const Clock::time_point queued = Clock::now();
    auto state = Vulkan::AfterPipelineCacheSave({}, Vulkan::PipelineCacheSaveOutcome::Failed,
                                                queued, queued + 20s);
    state.pipelines = 128;
    CHECK_FALSE(Vulkan::PipelineCacheFlushDue(state, queued + 60s, interval, 128));
    CHECK_FALSE(Vulkan::PipelineCacheFlushDue(state, queued + 599s, interval, 128));
    CHECK(Vulkan::PipelineCacheFlushDue(state, queued + 600s, interval, 128));
}
