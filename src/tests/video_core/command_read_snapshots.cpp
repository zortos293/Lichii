// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <thread>
#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/command_read_snapshots.h"
#include "video_core/command_read_scope.h"

using VideoCommon::CommandReadSnapshots;

TEST_CASE("Command snapshots require complete coverage and reject invalidated capture tokens",
          "[command_prefetch]") {
    CommandReadSnapshots cache;
    cache.Learn(0x2000, 8192);
    std::array<u8, 4096> page;
    page.fill(0x12);
    std::vector<CommandReadSnapshots::Token> tokens;
    cache.Candidates(0, 0, [&](auto token) { tokens.push_back(token); return true; });
    REQUIRE(tokens.size() == 2);
    REQUIRE(cache.Store(tokens[0], page));
    REQUIRE(cache.Covers(0x2004, 4));
    REQUIRE_FALSE(cache.Covers(0x2fff, 2));
    page.fill(0x34);
    REQUIRE(cache.Store(tokens[1], page));
    std::array<u8, 2> out{};
    REQUIRE(cache.Covers(0x2fff, out.size()));
    cache.Copy(0x2fff, out);
    REQUIRE(out[0] == 0x12);
    REQUIRE(out[1] == 0x34);
    cache.Invalidate(0x2fff, 2);
    REQUIRE_FALSE(cache.Covers(0x2004, 4));
    REQUIRE_FALSE(cache.Covers(0x3004, 4));
    REQUIRE_FALSE(cache.Store(tokens[0], page));
    REQUIRE_FALSE(cache.Store(tokens[1], page));
}

TEST_CASE("Command snapshots bound selection and invalidate reused pages and deletion epochs",
          "[command_prefetch]") {
    CommandReadSnapshots cache;
    cache.Learn(0x1000, CommandReadSnapshots::Capacity * 4096);
    std::vector<CommandReadSnapshots::Token> first;
    cache.Candidates(0x1000, 4096, [&](auto token) { first.push_back(token); return true; });
    REQUIRE(first.size() == CommandReadSnapshots::BatchPages);
    REQUIRE(std::none_of(first.begin(), first.end(), [](auto token) { return token.page == 0x1000; }));
    const auto stale = first.front();
    std::array<u8, 4096> page{};
    cache.InvalidateAll();
    REQUIRE_FALSE(cache.Store(stale, page));
    // FIFO eviction followed by re-learning cannot make an old capture token current again.
    cache.Learn(0x1000000, CommandReadSnapshots::Capacity * 4096);
    cache.Learn(stale.page, 4096);
    REQUIRE_FALSE(cache.Store(stale, page));
    REQUIRE(cache.counters.evicted == CommandReadSnapshots::Capacity + 1);
}

TEST_CASE("Pending command snapshots suppress duplicates without reviving an older generation",
          "[command_prefetch]") {
    CommandReadSnapshots cache;
    cache.Learn(0x2000, 4096);
    CommandReadSnapshots::Token old{};
    cache.Candidates(0, 0, [&](auto token) { old = token; return true; });
    cache.BeginPending(old, 10);
    REQUIRE(cache.PendingTick(0x2004, 4) == 10);
    size_t duplicates = 0;
    cache.Candidates(0, 0, [&](auto) { ++duplicates; return true; });
    REQUIRE(duplicates == 0);
    cache.Invalidate(0x2010, 4);
    REQUIRE(cache.PendingTick(0x2004, 4) == 0);
    CommandReadSnapshots::Token current{};
    cache.Candidates(0, 0, [&](auto token) { current = token; return true; });
    cache.BeginPending(current, 11);
    REQUIRE_FALSE(cache.EndPending(old));
    REQUIRE(cache.PendingTick(0x2004, 4) == 11);
    std::array<u8, 4096> page{};
    REQUIRE_FALSE(cache.Store(old, page, true));
    REQUIRE(cache.EndPending(current));
    REQUIRE(cache.PendingTick(0x2004, 4) == 0);
    REQUIRE(cache.Store(current, page, true));
    std::array<u8, 4> out{};
    REQUIRE(cache.Copy(0x2004, out));
}

TEST_CASE("Command read scope restores nesting and stays on its owning thread",
          "[command_prefetch][command_read_scope]") {
    using VideoCommon::CommandReadScope;
    REQUIRE_FALSE(CommandReadScope::IsActive());
    {
        const CommandReadScope outer;
        REQUIRE(CommandReadScope::IsActive());
        {
            const CommandReadScope inner;
            REQUIRE(CommandReadScope::IsActive());
        }
        REQUIRE(CommandReadScope::IsActive());
        bool child_before = true;
        bool child_during = false;
        bool child_after = true;
        std::thread child{[&] {
            child_before = CommandReadScope::IsActive();
            {
                const CommandReadScope scope;
                child_during = CommandReadScope::IsActive();
            }
            child_after = CommandReadScope::IsActive();
        }};
        child.join();
        REQUIRE_FALSE(child_before);
        REQUIRE(child_during);
        REQUIRE_FALSE(child_after);
        REQUIRE(CommandReadScope::IsActive());
    }
    REQUIRE_FALSE(CommandReadScope::IsActive());
}
