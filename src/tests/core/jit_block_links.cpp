// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// What the JIT keeps for each compiled block after emitting it: the links to other blocks, patched
// when their targets are compiled or invalidated, and the fastmem access sites a fault is matched
// against.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#if defined(__linux__) || defined(__APPLE__)
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

#include "common/common_types.h"

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/exclusive_monitor.h"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

namespace {

using namespace Dynarmic;

constexpr u32 Svc0 = 0xD4000001;

constexpr u32 AddImmediate(u32 rd, u32 rn, u32 imm) {
    return 0x91000000 | (imm << 10) | (rn << 5) | rd;
}

constexpr u32 Branch(u64 from, u64 to) {
    return 0x14000000 | (static_cast<u32>((to - from) >> 2) & 0x03FF'FFFF);
}

class LinkEnv final : public A64::UserCallbacks {
public:
    A64::Jit* jit = nullptr;
    std::map<u64, u32> code;
    u64 fallback_reads = 0;

    std::optional<std::uint32_t> MemoryReadCode(u64 vaddr) override {
        const auto it = code.find(vaddr);
        return it != code.end() ? it->second : Svc0;
    }
    u64 MemoryRead(u64 vaddr, size_t) override {
        ++fallback_reads;
        return 0xF000'0000'0000'0000 | vaddr;
    }
    A64::Vector MemoryRead128(u64) override { return {}; }
    void MemoryWrite(u64, u64, size_t) override {}
    void MemoryWrite128(u64, A64::Vector) override {}
    void CallSVC(std::uint32_t) override { jit->HaltExecution(); }
    void ExceptionRaised(u64, A64::Exception) override {}
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 0; }
    u64 GetCNTPCT() override { return 0; }
};

} // Anonymous namespace

TEST_CASE("JIT: a block linked to another follows the target when it is recompiled",
          "[jit][jit_block_links]") {
    LinkEnv env;
    ExclusiveMonitor monitor{1};
    A64::UserConfig config{};
    config.callbacks = &env;
    config.global_monitor = &monitor;
    config.enable_cycle_counting = false;
    A64::Jit jit{config};
    env.jit = &jit;

    // A at 0x1000 adds 1 to x0 and branches to B at 0x2000, which adds to x1 and stops.
    env.code[0x1000] = AddImmediate(0, 0, 1);
    env.code[0x1004] = Branch(0x1004, 0x2000);
    env.code[0x2000] = AddImmediate(1, 1, 2);
    const auto run = [&jit] {
        jit.SetRegister(0, 0);
        jit.SetRegister(1, 0);
        jit.SetPC(0x1000);
        jit.Run();
        return std::array{jit.GetRegister(0), jit.GetRegister(1)};
    };
    // The first run compiles A, then B, which links A to it; the second runs through that link.
    REQUIRE(run() == std::array<u64, 2>{1, 2});
    REQUIRE(run() == std::array<u64, 2>{1, 2});

    // A new B: the old one is invalidated and A unlinked from it, then linked to the new one.
    for (const u32 imm : {5u, 7u, 9u}) {
        env.code[0x2000] = AddImmediate(1, 1, imm);
        jit.InvalidateCacheRange(0x2000, 4);
        INFO("B adds " << imm);
        REQUIRE(run() == std::array<u64, 2>{1, imm});
        REQUIRE(run() == std::array<u64, 2>{1, imm});
    }
}

#if defined(__linux__) || defined(__APPLE__)

constexpr const char* FaultTestName =
    "JIT: a fastmem fault resumes at the access that faulted among several in a block";
#ifdef __linux__
constexpr const char* FaultChildVariable = "LICHII_JIT_BLOCK_LINKS_FAULT_CHILD";
#endif

constexpr u32 LoadX(u32 rt, u32 rn) {
    return 0xF9400000 | (rn << 5) | rt;
}

TEST_CASE(FaultTestName, "[jit][jit_block_links]") {
#ifdef __linux__
    if (!std::getenv(FaultChildVariable)) {
        // dynarmic installs its SIGSEGV handler once per process, and Catch replaces the process's
        // handler around each test case: after another case made a JIT, the fault would reach
        // Catch. Run this one in a fresh process, as "JIT codegen options: fastmem faults" does.
        char* argv[] = {const_cast<char*>("/proc/self/exe"), const_cast<char*>(FaultTestName),
                        nullptr};
        REQUIRE(setenv(FaultChildVariable, "1", 1) == 0);
        pid_t pid{};
        const int spawned = posix_spawn(&pid, "/proc/self/exe", nullptr, nullptr, argv, environ);
        unsetenv(FaultChildVariable);
        REQUIRE(spawned == 0);
        int status = 0;
        REQUIRE(waitpid(pid, &status, 0) == pid);
        REQUIRE(WIFEXITED(status));
        REQUIRE(WEXITSTATUS(status) == 0);
        return;
    }
#endif
    const long page = sysconf(_SC_PAGESIZE);
    REQUIRE(page > 0);
    const size_t page_size = static_cast<size_t>(page);
    const size_t arena_size = page_size * 4;
    void* const map = mmap(nullptr, arena_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(map != MAP_FAILED);
    auto* const arena = static_cast<u64*>(map);
    for (size_t i = 0; i < arena_size / sizeof(u64); ++i) {
        arena[i] = 0xA000'0000'0000'0000 | (i * sizeof(u64));
    }
    // Four loads, one per page; page 2 cannot be read, so the load into it takes the fallback.
    const std::array<u64, 4> addresses{0x10, page_size + 0x20, page_size * 2 + 0x30, page_size * 3 + 0x40};
    REQUIRE(mprotect(static_cast<std::byte*>(map) + page_size * 2, page_size, PROT_NONE) == 0);

    for (size_t faulting = 0; faulting < addresses.size(); ++faulting) {
        LinkEnv env;
        ExclusiveMonitor monitor{1};
        A64::UserConfig config{};
        config.callbacks = &env;
        config.global_monitor = &monitor;
        config.enable_cycle_counting = false;
        config.fastmem_pointer = reinterpret_cast<uintptr_t>(map);
        config.fastmem_address_space_bits = 64;
        config.silently_mirror_fastmem = false;
        config.recompile_on_fastmem_failure = false;
        A64::Jit jit{config};
        env.jit = &jit;

        // The load in position `faulting` reads the protected page; the others read the arena.
        std::array<u64, 4> loaded = addresses;
        std::swap(loaded[faulting], loaded[2]);
        for (u32 i = 0; i < 4; ++i) {
            env.code[0x1000 + i * 4] = LoadX(2 + i, 10 + i);
            jit.SetRegister(10 + i, loaded[i]);
        }
        jit.SetPC(0x1000);
        jit.Run();

        INFO("fault at load " << faulting);
        for (u32 i = 0; i < 4; ++i) {
            const u64 expected = i == faulting ? 0xF000'0000'0000'0000 | loaded[i]
                                               : 0xA000'0000'0000'0000 | loaded[i];
            CHECK(jit.GetRegister(2 + i) == expected);
        }
        CHECK(env.fallback_reads == 1);
    }
    munmap(map, arena_size);
}

#endif
