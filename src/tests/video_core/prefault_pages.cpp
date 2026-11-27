// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "common/thread_worker.h"
#include "video_core/texture_cache/prefault_pages.h"

using VideoCommon::PrefaultPages;

TEST_CASE("Concurrent prefault helpers claim every page exactly once", "[prefault_pages]") {
    std::vector<u8> bytes(541, 0x5a);
    std::vector<std::atomic<unsigned>> visits(bytes.size());
    std::vector<const u8*> pages;
    for (const u8& byte : bytes) {
        pages.push_back(&byte);
    }
    PrefaultPages work{std::move(pages)};
    const auto touch = [&] {
        for (;;) {
            const auto batch = work.NextBatch();
            if (batch.empty()) {
                return;
            }
            for (const u8* page : batch) {
                static_cast<void>(*static_cast<const volatile u8*>(page));
                ++visits[page - bytes.data()];
            }
            work.CompleteBatch(batch.size());
        }
    };
    std::vector<std::jthread> helpers;
    for (size_t i = 0; i < 3; ++i) {
        helpers.emplace_back(touch);
    }
    touch();
    work.Wait();
    helpers.clear();
    for (const auto& visit : visits) {
        REQUIRE(visit == 1);
    }
}

TEST_CASE("Prefault batches cover every page including the tail", "[prefault_pages]") {
    for (const size_t count : {0U, 1U, 63U, 64U, 65U, 300U, 540U, 541U}) {
        std::vector<u8> bytes(count, 0x5a);
        std::vector<const u8*> pages;
        for (const u8& byte : bytes) {
            pages.push_back(&byte);
        }
        PrefaultPages work{std::move(pages)};
        size_t visited = 0;
        for (;;) {
            const auto batch = work.NextBatch();
            if (batch.empty()) {
                break;
            }
            REQUIRE(batch.size() <= PrefaultPages::Batch);
            for (const u8* page : batch) {
                REQUIRE(page == &bytes[visited++]);
            }
            work.CompleteBatch(batch.size());
        }
        work.Wait();
        REQUIRE(visited == count);
        REQUIRE(work.NextBatch().empty());
    }
}

TEST_CASE("Prefault waits for claimed pages even when the cursor is exhausted", "[prefault_pages]") {
    using namespace std::chrono_literals;
    std::array<u8, 129> bytes{};
    std::vector<const u8*> pages;
    for (const u8& byte : bytes) {
        pages.push_back(&byte);
    }
    PrefaultPages work{std::move(pages)};
    // A helper has claimed this batch but has not read any of its pages yet.
    const auto held_batch = work.NextBatch();
    std::latch touched{1};
    auto caller = std::async(std::launch::async, [&] {
        work.Touch();
        touched.count_down();
        work.Wait();
    });
    touched.wait();
    const bool returned_early = caller.wait_for(20ms) == std::future_status::ready;
    for (const u8* page : held_batch) {
        static_cast<void>(*static_cast<const volatile u8*>(page));
    }
    work.CompleteBatch(held_batch.size());
    caller.get();
    REQUIRE(!returned_early);
    REQUIRE(work.NextBatch().empty());
}

TEST_CASE("Prefault can return before an admitted helper starts touching pages", "[prefault_pages]") {
    using namespace std::chrono_literals;
    Common::ThreadWorker pool{1, "PrefaultTest"};
    std::vector<u8> bytes(540 * 4096, 0xa5);
    std::vector<const u8*> pages;
    for (size_t offset = 0; offset < bytes.size(); offset += 4096) {
        pages.push_back(&bytes[offset]);
    }
    auto work = std::make_shared<PrefaultPages>(std::move(pages));
    std::weak_ptr<PrefaultPages> lifetime = work;
    std::latch entered{1};
    std::latch release{1};
    auto unblock = SCOPE_GUARD {
        release.count_down();
        pool.WaitForRequests();
    };
    REQUIRE(pool.TryQueueWork([work, &entered, &release] {
        entered.count_down();
        release.wait();
        work->Touch();
    }));
    entered.wait();
    auto caller = std::async(std::launch::async, [work] {
        work->Touch();
        work->Wait();
    });
    const bool completed = caller.wait_for(1s) == std::future_status::ready;
    if (!completed) {
        release.count_down();
        unblock.Cancel();
        pool.WaitForRequests();
    }
    caller.get();
    REQUIRE(completed);
    REQUIRE(std::all_of(bytes.begin(), bytes.end(), [](u8 byte) { return byte == 0xa5; }));
    work.reset();
    bytes.clear();
    bytes.shrink_to_fit();
    REQUIRE(!lifetime.expired());

    // A late no-op still occupies its pool slot. Repeated uploads cannot build an unbounded
    // backlog; a busy pool rejects them and the caller performs their page reads itself.
    for (size_t i = 0; i < 128; ++i) {
        u8 byte = 0x5a;
        auto next = std::make_shared<PrefaultPages>(std::vector<const u8*>{&byte});
        REQUIRE(!pool.TryQueueWork([next] { next->Touch(); }));
        next->Touch();
        next->Wait();
        REQUIRE(byte == 0x5a);
    }
    release.count_down();
    unblock.Cancel();
    pool.WaitForRequests();
    REQUIRE(lifetime.expired());
    REQUIRE(pool.TryQueueWork([] {}));
    pool.WaitForRequests();
}
