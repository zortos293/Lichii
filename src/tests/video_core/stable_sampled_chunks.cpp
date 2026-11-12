// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <memory>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "video_core/stable_sampled_chunks.h"

namespace {
using namespace VideoCommon::StableSampled;
using Cache = ChunkCache<U64>;
} // namespace

TEST_CASE("Sampled chunks require matching layout slot, epoch and resolved IDs", "[video_core]") {
    Cache cache;
    REQUIRE(cache.Ensure(2));
    std::array<U64, 3> keys{1, 2, 3}, values{10, 20, 30};
    REQUIRE(cache.Find(0, 1, keys) == nullptr);
    cache.Store(0, 1, keys, values, false);
    REQUIRE(cache.Find(0, 1, keys) != nullptr);
    REQUIRE(cache.Find(0, 1, keys)->values[2] == 30);
    REQUIRE(cache.Find(1, 1, keys) == nullptr);
    REQUIRE(cache.Find(0, 2, keys) == nullptr);
    REQUIRE(cache.Find(0, 1, std::span{keys}.first(2)) == nullptr);
    keys[1] = 9;
    REQUIRE(cache.Find(0, 1, keys) == nullptr);
    cache.Store(1, 2, keys, values, true);
    REQUIRE(cache.Find(1, 2, keys) != nullptr);
    cache.Clear(1);
    REQUIRE(cache.Find(1, 2, keys) == nullptr);
}

TEST_CASE("Sampled chunks reject invalid lengths and keep pipeline caches separate",
          "[video_core]") {
    Cache first, second;
    REQUIRE_FALSE(first.Ensure(0));
    REQUIRE_FALSE(first.Ensure(257));
    REQUIRE(first.Ensure(1));
    REQUIRE(second.Ensure(1));
    std::array<U64, ChunkSize + 1> keys{}, values{};
    first.Store(0, 1, std::span{keys}.first(ChunkSize), std::span{values}.first(ChunkSize), false);
    REQUIRE(first.Find(0, 1, std::span{keys}.first(ChunkSize)) != nullptr);
    REQUIRE(second.Find(0, 1, std::span{keys}.first(ChunkSize)) == nullptr);
    REQUIRE(first.Find(0, 1, keys) == nullptr);
    REQUIRE(first.Find(0, 1, {}) == nullptr);
    first.Clear(0);
    first.Store(0, 1, keys, values, false);
    REQUIRE(first.Find(0, 1, std::span{keys}.first(ChunkSize)) == nullptr);
}

TEST_CASE("Sampled chunk allocation obeys and releases its global budget", "[video_core]") {
    const auto before = ReservedBytes().load();
    bool exhausted = false;
    {
        std::vector<std::unique_ptr<Cache>> caches;
        for (unsigned i = 0; i < 128; ++i) {
            auto cache = std::make_unique<Cache>();
            if (!cache->Ensure(256)) {
                exhausted = true;
                break;
            }
            REQUIRE(ReservedBytes().load() <= Budget);
            caches.push_back(std::move(cache));
        }
        REQUIRE(exhausted);
    }
    REQUIRE(ReservedBytes().load() == before);
}

TEST_CASE("Resource epoch changes invalidate sampled chunks before ID reuse", "[video_core]") {
    Cache cache;
    REQUIRE(cache.Ensure(1));
    const auto old = Epoch();
    const std::array<U64, 1> key{42}, value{99};
    cache.Store(0, old, key, value, false);
    REQUIRE(cache.Find(0, old, key) != nullptr);
    InvalidateResources();
    REQUIRE(Epoch() != old);
    REQUIRE(cache.Find(0, Epoch(), key) == nullptr);
}
