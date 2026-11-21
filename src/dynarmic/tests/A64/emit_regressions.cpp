// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Emitter regressions found by tools/jitbench/fuzz: each instruction compiled in a normal block
// (Run) must give the result single-stepping gives, without the emitter throwing.

#include <csignal>
#include <cstdio>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <oaknut/oaknut.hpp>

#if defined(__unix__) || defined(__APPLE__)
#    include <sys/wait.h>
#    include <unistd.h>
#endif

#include "./testenv.h"
#include "common/uint128.h"
#include "common/x64/rdtsc.h"
#include "dynarmic/interface/A64/a64.h"

using namespace Dynarmic;
using namespace oaknut::util;

namespace {

class HaltOnSvcEnv final : public A64TestEnv {
public:
    A64::Jit* jit = nullptr;
    bool svc = false;
    void CallSVC(std::uint32_t) override {
        svc = true;
        jit->HaltExecution();
    }
};

template<typename Gen>
Vector RunV0(bool step, Gen&& gen) {
    HaltOnSvcEnv env;
    oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
    gen(code);
    code.SVC(0);
    A64::UserConfig config{};
    config.callbacks = &env;
    config.enable_cycle_counting = false;
    A64::Jit jit{config};
    env.jit = &jit;
    jit.SetPC(0);
    jit.SetVector(0, {0x0123456789ABCDEF, 0xFEDCBA9876543210});
    jit.SetVector(1, {0x1122334455667788, 0x99AABBCCDDEEFF00});
    if (step) {
        while (!env.svc) jit.Step();
    } else {
        jit.Run();
    }
    return jit.GetVector(0);
}

}  // namespace

TEST_CASE("A64: SM4E table address", "[a64][emit_regressions]") {
    // The S-box was addressed as an absolute disp32 memory operand: Xbyak error 15 ("can't use
    // 64bit disp") whenever the host image sits above 4 GiB (PIE, Windows).
    const auto gen = [](auto& code) { code.SM4E(V0.S4(), V1.S4()); };
    REQUIRE(RunV0(false, gen) == RunV0(true, gen));
}

TEST_CASE("A64: inline CNTPCT applies live clock changes to compiled code",
          "[a64][inline_cntpct]") {
    constexpr u64 callback_ticks = 0x12345678'9abcdef0;
    struct CounterEnv final : A64TestEnv {
        A64::Jit* jit{};
        u64 last_read{};
        unsigned reads{};
        void CallSVC(u32) override { jit->HaltExecution(); }
        u64 GetCNTPCT() override {
            ++reads;
            return last_read = callback_ticks;
        }
    } env;

    oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
    oaknut::Label read;
    code.BL(read);
    code.SVC(0);
    code.l(read);
    code.ADD(X4, X2, X3);
    code.ADD(V4.D2(), V2.D2(), V3.D2());
    code.dw(0xd53be021);  // MRS X1, CNTPCT_EL0
    code.ADD(X5, X4, X3);
    code.ADD(V5.D2(), V4.D2(), V3.D2());
    code.RET();

    std::atomic<u64> factor{};
    std::atomic<u32> mode{};
    A64::UserConfig config{};
    config.callbacks = &env;
    config.wall_clock_cntpct = true;
    config.enable_cycle_counting = false;
    config.inline_cntpct_factor = &factor;
    config.inline_cntpct_mode = &mode;
    config.inline_cntpct_last = &env.last_read;
    config.inline_cntpct_spin_ticks = 52;
    config.omit_block_frame = config.host_call_return = true;
    A64::Jit jit{config};
    env.jit = &jit;

    const auto run = [&] {
        jit.SetPC(0);
        jit.SetRegister(2, 100);
        jit.SetRegister(3, 200);
        jit.SetVector(2, {10, 20});
        jit.SetVector(3, {30, 40});
        jit.Run();
        CHECK(jit.GetRegister(5) == 500);
        CHECK(jit.GetVector(5) == Vector{70, 100});
        CHECK(jit.GetRegister(1) == env.last_read);
        return jit.GetRegister(1);
    };

    for (const auto guest_hz : {19'200'000, 31'250'000}) {
        const u64 scale = Common::GetFixedPoint64Factor(guest_hz, 3'000'000'000);
        factor.store(scale, std::memory_order_relaxed);
        mode.store(0, std::memory_order_relaxed);
        const u64 before = Common::MultiplyHigh(Common::X64::FencedRDTSC(), scale);
        const u64 ticks = run();
        const u64 after = Common::MultiplyHigh(Common::X64::FencedRDTSC(), scale);
        CHECK(ticks >= before);
        CHECK(ticks <= after);

        const unsigned old_reads = env.reads;
        mode.store(1, std::memory_order_relaxed);
        CHECK(run() == callback_ticks);
        CHECK(env.reads == old_reads + 1);
    }
    CHECK(env.reads == 2);
}

#if defined(__unix__) || defined(__APPLE__)
namespace {

struct ChildResult {
    bool exited_normally;
    std::string stderr_text;
};

// Runs `body` in a child process (an emitter error ends the process) and collects its stderr.
template<typename Body>
ChildResult RunInChild(Body&& body) {
    int fds[2];
    REQUIRE(pipe(fds) == 0);
    std::fflush(nullptr);
    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        // Catch2's signal handlers and reporter belong to the parent.
        for (const int signal : {SIGABRT, SIGSEGV, SIGILL, SIGFPE, SIGBUS, SIGTERM}) {
            std::signal(signal, SIG_DFL);
        }
        close(fds[0]);
        dup2(fds[1], STDERR_FILENO);
        dup2(fds[1], STDOUT_FILENO);
        body();
        _exit(0);
    }
    close(fds[1]);
    std::string text;
    char buffer[256];
    for (ssize_t n; (n = read(fds[0], buffer, sizeof(buffer))) > 0;) {
        text.append(buffer, size_t(n));
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return {WIFEXITED(status) && WEXITSTATUS(status) == 0, text};
}

A64::UserConfig LichiiTerminalConfig(HaltOnSvcEnv& env, std::uint32_t code_cache_size) {
    A64::UserConfig config{};
    config.callbacks = &env;
    config.enable_cycle_counting = false;
    config.omit_block_frame = true;
    config.compact_block_exits = true;
    config.halt_check_on_back_edges = true;
    config.host_call_return = true;
    config.code_cache_size = code_cache_size;
    return config;
}

}  // namespace

TEST_CASE("A64: emitter errors name the block", "[a64][emit_regressions]") {
    // Xbyak reports errors (a full code buffer, an operand out of range) by throwing. Every
    // function on the emit path below A64EmitX64::Emit must let the exception through to Emit,
    // which names the guest PC: a noexcept frame in between (the terminal emitters, the register
    // allocator) ended the process in std::terminate without a word. A code cache just large
    // enough for the prelude makes the throw land in each part of a block in turn.
    HaltOnSvcEnv env;
    oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
    code.ADD(X0, X0, X2);
    code.MUL(X3, X0, X2);
    code.BR(X1);  // the indirect-branch-cache site
    code.SVC(0);

    // The smallest cache the Jit can be constructed with (its prelude).
    std::uint32_t lo = 64 * 1024, hi = 64 * 1024 * 1024;
    while (lo < hi) {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        const bool fits = RunInChild([&] {
            A64::Jit jit{LichiiTerminalConfig(env, mid)};
        }).exited_normally;
        if (fits) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    const std::uint32_t prelude = lo;

    size_t reported = 0;
    for (std::uint32_t extra = 0; extra < 160; ++extra) {
        const ChildResult result = RunInChild([&] {
            A64::Jit jit{LichiiTerminalConfig(env, prelude + extra)};
            env.jit = &jit;
            jit.SetPC(0);
            jit.SetRegister(1, 12);
            jit.Run();
        });
        INFO("code cache " << prelude << " + " << extra << " bytes; stderr: " << result.stderr_text);
        if (!result.exited_normally) {
            REQUIRE(result.stderr_text.find("emitting the A64 block at 0") != std::string::npos);
            ++reported;
        }
    }
    // The buffer did run out in the scanned range.
    REQUIRE(reported > 0);
}
#endif
