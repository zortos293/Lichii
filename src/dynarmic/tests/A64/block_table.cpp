// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "dynarmic/backend/arm64/block_table.h"

using namespace Dynarmic::Backend::Arm64;

namespace {
// An entry point that names its key and a version, so a reader can tell a right answer from a wrong one.
CodePtr EntryPoint(u64 key, u64 version) {
    return reinterpret_cast<CodePtr>((key << 16) | (version & 0xffff) | 1);
}
}  // namespace

TEST_CASE("A64: the block table keeps blocks across growth, drops and re-inserts", "[a64]") {
    BlockTable table;
    constexpr u64 count = 300'000;  // several times the first table
    for (u64 key = 0; key < count; ++key) {
        REQUIRE(table.Insert(key * 4, EntryPoint(key, 0)));
    }
    REQUIRE(!table.Insert(8, EntryPoint(2, 1)));
    for (u64 key = 0; key < count; ++key) {
        REQUIRE(table.Find(key * 4) == EntryPoint(key, 0));
    }
    REQUIRE(table.Find(count * 4) == nullptr);
    for (u64 key = 0; key < count; key += 2) {
        REQUIRE(table.Erase(key * 4) == EntryPoint(key, 0));
    }
    REQUIRE(table.Erase(0) == nullptr);
    for (u64 key = 0; key < count; ++key) {
        REQUIRE(table.Find(key * 4) == (key % 2 ? EntryPoint(key, 0) : nullptr));
    }
    for (u64 key = 0; key < count; key += 2) {
        REQUIRE(table.Insert(key * 4, EntryPoint(key, 1)));
    }
    // More keys than before: grows again, without the dropped entries.
    for (u64 key = count; key < 3 * count; ++key) {
        REQUIRE(table.Insert(key * 4, EntryPoint(key, 0)));
    }
    for (u64 key = 0; key < 3 * count; ++key) {
        REQUIRE(table.Find(key * 4) == EntryPoint(key, key < count && key % 2 == 0 ? 1 : 0));
    }
    table.Clear();
    REQUIRE(table.Find(4) == nullptr);
    REQUIRE(table.Insert(4, EntryPoint(1, 2)));
    REQUIRE(table.Find(4) == EntryPoint(1, 2));
}

TEST_CASE("A64: the block table is read while it is written", "[a64]") {
    BlockTable table;
    constexpr u64 count = 200'000;
    std::atomic<bool> stop{false};
    std::atomic<u64> wrong{0};
    std::atomic<u64> found{0};
    std::vector<std::thread> readers;
    for (int reader = 0; reader < 4; ++reader) {
        readers.emplace_back([&, reader] {
            u64 state = 0x9e3779b97f4a7c15ULL * (reader + 1);
            u64 hits = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                state = state * 6364136223846793005ULL + 1442695040888963407ULL;
                const u64 key = (state >> 20) % count;
                const CodePtr entry_point = table.Find(key * 4);
                if (entry_point == nullptr) {
                    continue;
                }
                // Whatever version, it must be an entry point of this key.
                if ((reinterpret_cast<u64>(entry_point) >> 16) != key) {
                    wrong.fetch_add(1);
                }
                ++hits;
            }
            found.fetch_add(hits);
        });
    }
    // One writer, as under the cache's lock: insert all (the table grows under the readers), then drop and
    // re-insert with new versions.
    for (u64 key = 0; key < count; ++key) {
        REQUIRE(table.Insert(key * 4, EntryPoint(key, 0)));
    }
    for (u64 version = 1; version <= 4; ++version) {
        for (u64 key = 0; key < count; ++key) {
            REQUIRE(table.Erase(key * 4) == EntryPoint(key, version - 1));
            REQUIRE(table.Insert(key * 4, EntryPoint(key, version)));
        }
    }
    stop.store(true);
    for (std::thread& reader : readers) {
        reader.join();
    }
    REQUIRE(wrong.load() == 0);
    REQUIRE(found.load() > 0);
    for (u64 key = 0; key < count; ++key) {
        REQUIRE(table.Find(key * 4) == EntryPoint(key, 4));
    }
}
