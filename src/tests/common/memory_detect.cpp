// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "common/memory_detect.h"

namespace {
// /proc/meminfo and /proc/self/status as Linux 6.x writes them (shortened).
constexpr std::string_view Meminfo = "MemTotal:       32562688 kB\n"
                                     "MemFree:        12734976 kB\n"
                                     "MemAvailable:   23068672 kB\n"
                                     "CommitLimit:    79195900 kB\n"
                                     "Committed_AS:   20971520 kB\n"
                                     "HugePages_Total:       0\n";
constexpr std::string_view Status = "Name:\tlichii\n"
                                    "VmPeak:\t533000000 kB\n"
                                    "VmData:\t 41943040 kB\n"
                                    "VmRSS:\t 13631488 kB\n"
                                    "RssAnon:\t  7864320 kB\n"
                                    "Threads:\t96\n";
constexpr u64 KiB = 1024;
} // namespace

TEST_CASE("ProcKilobytesField reads kB fields as bytes", "[common]") {
    CHECK(Common::ProcKilobytesField(Meminfo, "Committed_AS:") == 20971520 * KiB);
    CHECK(Common::ProcKilobytesField(Meminfo, "CommitLimit:") == 79195900 * KiB);
    CHECK(Common::ProcKilobytesField(Meminfo, "MemTotal:") == 32562688 * KiB);
    CHECK(Common::ProcKilobytesField(Status, "VmData:") == 41943040 * KiB);
    CHECK(Common::ProcKilobytesField(Status, "VmRSS:") == 13631488 * KiB);
    CHECK(Common::ProcKilobytesField(Status, "RssAnon:") == 7864320 * KiB);
}

TEST_CASE("ProcKilobytesField matches whole field names at the start of a line", "[common]") {
    CHECK(Common::ProcKilobytesField(Meminfo, "Free:") == 0);
    CHECK(Common::ProcKilobytesField(Meminfo, "MemFree:") == 12734976 * KiB);
    CHECK(Common::ProcKilobytesField(Meminfo, "Committed:") == 0);
}

TEST_CASE("ProcKilobytesField gives 0 for missing or malformed fields", "[common]") {
    CHECK(Common::ProcKilobytesField(Meminfo, "Shmem:") == 0);
    CHECK(Common::ProcKilobytesField("", "Committed_AS:") == 0);
    CHECK(Common::ProcKilobytesField("Committed_AS:\n", "Committed_AS:") == 0);
    CHECK(Common::ProcKilobytesField("Committed_AS: lots kB\n", "Committed_AS:") == 0);
    // The last line without its newline (a truncated read) still counts.
    CHECK(Common::ProcKilobytesField("CommitLimit:   1000 kB", "CommitLimit:") == 1000 * KiB);
}

#if defined(__linux__) && defined(__GLIBC__)
#include <cstdlib>
#include <cstring>
#include <vector>

TEST_CASE("TrimHeap hands freed heap pages back to the system", "[common]") {
    // Every other block freed: the free space sits between live blocks, below the top of the
    // heap, where glibc never gives it back on its own.
    constexpr size_t BlockSize = 64 * 1024 - 64;
    constexpr size_t Blocks = 2048; // ~128 MiB
    std::vector<void*> blocks(Blocks);
    for (void*& block : blocks) {
        block = std::malloc(BlockSize);
        REQUIRE(block != nullptr);
        std::memset(block, 1, BlockSize);
    }
    for (size_t i = 0; i < Blocks; i += 2) {
        std::free(blocks[i]);
        blocks[i] = nullptr;
    }
    const Common::HeapTrimResult result = Common::TrimHeap("the test");
    CHECK(result.resident_before > result.resident_after);
    // About half the blocks were freed; at least a quarter of their bytes must go back.
    CHECK(result.resident_before - result.resident_after >= Blocks / 2 * BlockSize / 4);
    for (void* block : blocks) {
        std::free(block);
    }
}
#endif
