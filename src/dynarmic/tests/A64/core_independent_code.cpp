// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// UserConfig::core_independent_code: per-Jit values (callbacks, config, TPIDR pointers,
// exclusive-monitor slots, the LSE fallback context) come from the JIT state at run time. The
// paths that use them run with the option on and off on two processors sharing one global
// monitor and must end in the same state.

#include <array>
#include <cstring>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <oaknut/oaknut.hpp>

#include "common/common_types.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/exclusive_monitor.h"

using namespace Dynarmic;
using namespace oaknut::util;

namespace {

class Env final : public A64::UserCallbacks {
public:
    std::array<u8, 0x10000> mem{};
    A64::Jit* jit = nullptr;
    u64 svcs = 0;
    u64 exceptions = 0;

    template<typename T>
    T Rd(u64 a) {
        T v;
        std::memcpy(&v, mem.data() + a, sizeof v);
        return v;
    }
    template<typename T>
    void Wr(u64 a, T v) { std::memcpy(mem.data() + a, &v, sizeof v); }

    std::optional<u32> MemoryReadCode(u64 a) override { return Rd<u32>(a); }
    u64 MemoryRead(u64 a, size_t s) override {
        return s == 8 ? Rd<u64>(a) : s == 4 ? Rd<u32>(a) : s == 2 ? Rd<u16>(a) : Rd<u8>(a);
    }
    A64::Vector MemoryRead128(u64 a) override { return Rd<A64::Vector>(a); }
    void MemoryWrite(u64 a, u64 v, size_t s) override {
        if (s == 8) Wr<u64>(a, v);
        else if (s == 4) Wr<u32>(a, u32(v));
        else if (s == 2) Wr<u16>(a, u16(v));
        else Wr<u8>(a, u8(v));
    }
    void MemoryWrite128(u64 a, A64::Vector v) override { Wr(a, v); }
    bool MemoryWriteExclusive8(u64 a, u8 v, u8) override { return Wr(a, v), true; }
    bool MemoryWriteExclusive16(u64 a, u16 v, u16) override { return Wr(a, v), true; }
    bool MemoryWriteExclusive32(u64 a, u32 v, u32) override { return Wr(a, v), true; }
    bool MemoryWriteExclusive64(u64 a, u64 v, u64) override { return Wr(a, v), true; }
    bool MemoryWriteExclusive128(u64 a, A64::Vector v, A64::Vector) override { return Wr(a, v), true; }
    void CallSVC(u32 swi) override {
        svcs += swi;
        if (swi == 0) jit->HaltExecution();
    }
    void ExceptionRaised(u64, A64::Exception) override { ++exceptions; }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1ull << 40; }
    u64 GetCNTPCT() override { return 0; }
};

struct Result {
    std::array<u64, 31> regs;
    u64 tpidr;
    u64 svcs, exceptions;
    u32 word;
    bool operator==(const Result&) const = default;
};

Result Run(bool core_independent) {
    ExclusiveMonitor monitor{2};
    std::array<u64, 2> tpidr{0x1111, 0x2222};
    std::array<u64, 2> tpidrro{0xAAAA, 0xBBBB};
    Result result{};
    // Processor 1 runs the program; processor 0 only holds a monitor slot.
    Env env;
    std::vector<u32> prog;
    oaknut::VectorCodeGenerator code{prog, nullptr};
    oaknut::Label loop;
    code.MOVZ(X20, 0x8000);
    code.MOVZ(X19, 20);
    code.MOVZ(X3, 1);
    code.l(loop);
    code.MRS(X0, oaknut::SystemReg::TPIDRRO_EL0);
    code.MRS(X1, oaknut::SystemReg::TPIDR_EL0);
    code.ADD(X1, X1, X0);
    code.MSR(oaknut::SystemReg::TPIDR_EL0, X1);
    code.LDAXR(W4, X20);  // exclusive pair through the monitor
    code.ADD(W4, W4, 1);
    code.STLXR(W5, W4, X20);
    code.LDADD(W3, W6, X20);  // LSE atomic
    code.STR(X6, X20, 8);
    code.SVC(2);
    code.YIELD();
    code.SUBS(X19, X19, 1);
    code.B(NE, loop);
    code.SVC(0);
    std::memcpy(env.mem.data(), prog.data(), prog.size() * 4);

    A64::UserConfig config{};
    config.callbacks = &env;
    config.enable_cycle_counting = false;
    config.global_monitor = &monitor;
    config.processor_id = 1;
    config.tpidr_el0 = &tpidr[1];
    config.tpidrro_el0 = &tpidrro[1];
    config.hook_hint_instructions = true;
    config.core_independent_code = core_independent;
    A64::Jit jit{config};
    env.jit = &jit;
    jit.SetPC(0);
    jit.SetSP(0x4000);
    jit.Run();
    for (size_t i = 0; i < 31; ++i) {
        result.regs[i] = jit.GetRegister(i);
    }
    result.tpidr = tpidr[1];
    result.svcs = env.svcs;
    result.exceptions = env.exceptions;
    result.word = env.Rd<u32>(0x8000);
    return result;
}

}  // namespace

TEST_CASE("A64: core_independent_code", "[a64][core_independent_code]") {
    {
        const Result off = Run(false);
        const Result on = Run(true);
        REQUIRE(on == off);
        REQUIRE(on.word == 40);
        REQUIRE(on.tpidr == 0x2222 + 20 * 0xBBBB);
        REQUIRE(on.svcs == 40);
        REQUIRE(on.exceptions == 20);
    }
}
