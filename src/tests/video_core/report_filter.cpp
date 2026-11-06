// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "video_core/report_filter.h"

namespace {
namespace Filter = VideoCore::ReportFilter;

// Each case uses its own addresses: the tables are shared by the whole test binary.

TEST_CASE("A forwarded report is pending until its write is finished", "[video_core]") {
    constexpr u64 address = 0x10000000;
    REQUIRE_FALSE(Filter::Pending(address, 4));
    REQUIRE_FALSE(Filter::Issue(address));
    REQUIRE(Filter::Pending(address, 4));
    REQUIRE(Filter::Pending(address - 0x20, 0x40));
    Filter::Done(address);
    REQUIRE_FALSE(Filter::Pending(address, 4));
}

TEST_CASE("Reads on other blocks do not wait for the report", "[video_core]") {
    constexpr u64 address = 0x20000100;
    REQUIRE_FALSE(Filter::Issue(address));
    REQUIRE_FALSE(Filter::Pending(address + 0x10, 0x10));
    REQUIRE_FALSE(Filter::Pending(address - 0x10, 0x10));
    REQUIRE_FALSE(Filter::Pending(address, 0));
    Filter::Done(address);
}

TEST_CASE("A report that is not 16-byte aligned counts both blocks", "[video_core]") {
    constexpr u64 address = 0x30000008;
    REQUIRE_FALSE(Filter::Issue(address));
    REQUIRE(Filter::Pending(0x30000000, 4));
    REQUIRE(Filter::Pending(0x30000010, 4));
    Filter::Done(address);
    REQUIRE_FALSE(Filter::Pending(0x30000000, 0x20));
}

TEST_CASE("Reports on one block are all counted", "[video_core]") {
    constexpr u64 address = 0x40000000;
    (void)Filter::Issue(address);
    (void)Filter::Issue(address);
    Filter::Done(address);
    REQUIRE(Filter::Pending(address, 4));
    Filter::Done(address);
    REQUIRE_FALSE(Filter::Pending(address, 4));
}

TEST_CASE("A block a read found pending is hot for the next report", "[video_core]") {
    constexpr u64 address = 0x50000000;
    (void)Filter::Issue(address);
    Filter::MarkHot(address, 4);
    Filter::Done(address);
    REQUIRE(Filter::Issue(address));
    Filter::Done(address);
    // Nothing pending: nothing becomes hot.
    constexpr u64 quiet = 0x50001000;
    Filter::MarkHot(quiet, 4);
    REQUIRE_FALSE(Filter::Issue(quiet));
    Filter::Done(quiet);
}

TEST_CASE("Finishing a report wakes a waiting reader", "[video_core]") {
    constexpr u64 address = 0x60000000;
    (void)Filter::Issue(address);
    std::thread writer([] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        Filter::Done(address);
    });
    Filter::g_waiting.store(true, std::memory_order_seq_cst);
    bool finished = false;
    {
        std::unique_lock lock{Filter::g_wait_mutex};
        const auto start = std::chrono::steady_clock::now();
        while (!(finished = !Filter::Pending(address, 4)) &&
               std::chrono::steady_clock::now() - start < std::chrono::seconds{5}) {
            Filter::g_wait_cv.wait_for(lock, std::chrono::milliseconds{1});
        }
    }
    Filter::g_waiting.store(false, std::memory_order_relaxed);
    writer.join();
    REQUIRE(finished);
}

TEST_CASE("A new session starts without pending reports or a lost filter", "[video_core]") {
    constexpr u64 address = 0x70000000;
    (void)Filter::Issue(address);
    Filter::MarkHot(address, 4);
    Filter::g_gave_up.store(true, std::memory_order_relaxed);
    REQUIRE(Filter::Pending(address, 4));
    Filter::Reset();
    REQUIRE_FALSE(Filter::Pending(address, 4));
    REQUIRE_FALSE(Filter::g_gave_up.load(std::memory_order_relaxed));
    REQUIRE_FALSE(Filter::Issue(address));
    Filter::Done(address);
}
} // namespace
