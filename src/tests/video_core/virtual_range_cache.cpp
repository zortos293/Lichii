// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <optional>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/virtual_range_cache.h"

namespace {

using VideoCommon::VirtualRangeCache;

constexpr GPUVAddr BASE = 0x100000;
constexpr GPUVAddr HOLE = 0x10000;
constexpr DAddr JUMP = 0x1000000;

/// GPU memory that is contiguous, except for a device address jump HOLE into a split range.
struct FakeMemory {
    bool split{};
    mutable int walks{};

    size_t GetID() const {
        return 1;
    }

    std::vector<std::pair<GPUVAddr, size_t>> GetSubmappedRange(GPUVAddr gpu_addr,
                                                               size_t size) const {
        ++walks;
        if (split && gpu_addr < BASE + HOLE && gpu_addr + size > BASE + HOLE) {
            const size_t first = BASE + HOLE - gpu_addr;
            return {{gpu_addr, first}, {BASE + HOLE, size - first}};
        }
        return {{gpu_addr, size}};
    }

    std::optional<DAddr> GpuToCpuAddress(GPUVAddr gpu_addr) const {
        const DAddr jump = split && gpu_addr >= BASE + HOLE ? JUMP : 0;
        return DAddr{gpu_addr + 0x1000 + jump};
    }
};

} // Anonymous namespace

TEST_CASE("VirtualRangeCache walks a contiguous address once for its longest size",
          "[video_core][virtual_range_cache]") {
    FakeMemory memory;
    VirtualRangeCache cache;
    REQUIRE(cache.Query(memory, BASE, 0x4000)->size() == 1);
    REQUIRE(memory.walks == 1);
    // Longer: walked again, and kept.
    REQUIRE(cache.Query(memory, BASE, 0x200000)->size() == 1);
    REQUIRE(memory.walks == 2);
    // Alternating sizes up to the longest walk nothing.
    for (int i = 0; i < 16; ++i) {
        REQUIRE(cache.Query(memory, BASE, 0x4000)->size() == 1);
        REQUIRE(cache.Query(memory, BASE, 0x200000)->size() == 1);
        REQUIRE(cache.Query(memory, BASE, 0x1000)->size() == 1);
    }
    REQUIRE(memory.walks == 2);
}

TEST_CASE("VirtualRangeCache walks again for ranges of a split address",
          "[video_core][virtual_range_cache]") {
    FakeMemory memory{.split = true};
    VirtualRangeCache cache;
    REQUIRE(cache.Query(memory, BASE, HOLE * 2)->size() == 2);
    // A prefix of a split range may be whole: the list of the longer range doesn't answer it.
    REQUIRE(cache.Query(memory, BASE, HOLE / 2)->size() == 1);
    REQUIRE(memory.walks == 2);
    REQUIRE(cache.Query(memory, BASE, HOLE * 2)->size() == 2);
    REQUIRE(memory.walks == 3);
}

TEST_CASE("VirtualRangeCache forgets a kept range when its tail is unmapped",
          "[video_core][virtual_range_cache]") {
    FakeMemory memory;
    VirtualRangeCache cache;
    REQUIRE(cache.Query(memory, BASE, 0x200000)->size() == 1);
    cache.Unmap(memory.GetID(), BASE + 0x100000, 0x1000);
    memory.split = true;
    REQUIRE(cache.Query(memory, BASE, HOLE * 2)->size() == 2);
    REQUIRE(memory.walks == 2);
}

TEST_CASE("VirtualRangeCache answers ranges inside a linear span without walking",
          "[video_core][virtual_range_cache]") {
    FakeMemory memory;
    VirtualRangeCache cache;
    REQUIRE(cache.Query(memory, BASE, 0x200000)->size() == 1);
    REQUIRE(memory.walks == 1);
    // Other starts and sizes inside the walked range come from its span.
    const auto* segments = cache.Query(memory, BASE + 0x3000, 0x8000);
    REQUIRE(segments->size() == 1);
    REQUIRE(segments->front().gpu_addr == BASE + 0x3000);
    REQUIRE(segments->front().device_addr == BASE + 0x3000 + 0x1000);
    REQUIRE(segments->front().size == 0x8000);
    REQUIRE(memory.walks == 1);
    // An unmap inside the span drops it.
    cache.Unmap(memory.GetID(), BASE + 0x100000, 0x1000);
    REQUIRE(cache.Query(memory, BASE + 0x3000, 0x8000)->size() == 1);
    REQUIRE(memory.walks == 2);
}
