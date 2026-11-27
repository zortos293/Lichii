// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Several A64 Jits on one SharedCodeCache, each on its own thread, as ArmDynarmic64 runs the
// guest cores of one process. Runners compile the same and different blocks, link and return
// through them and take exclusives; a writer rewrites a function they call, invalidates it and
// asks for full clears meanwhile. The cache is small, so it also fills up and clears itself.

#include <array>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

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
using Vector = A64::Vector;

constexpr u64 CallerBase = 0x1000;
constexpr u64 MutableBase = 0x2000; // movz x5, #version; ret
constexpr u64 WriterBase = 0x3000;  // bl MutableBase; svc #0
constexpr u64 CounterAddress = 0x8000;
constexpr u64 FuncBase = 0x1000'0000; // func i at FuncBase + 16 * i: x0 += i & 0xFFFF; ret

constexpr size_t Runners = 4;
constexpr u64 Rounds = 4;
constexpr u64 Iterations = 20000;

constexpr u32 Movz(u32 d, u64 imm) { return 0xD2800000 | u32(imm & 0xFFFF) << 5 | d; }
constexpr u32 AddReg(u32 d, u32 n, u32 m) { return 0x8B000000 | m << 16 | n << 5 | d; }
constexpr u32 AddImm(u32 d, u32 n, u32 imm) { return 0x91000000 | imm << 10 | n << 5 | d; }
constexpr u32 SubsImm(u32 d, u32 n, u32 imm) { return 0xF1000000 | imm << 10 | n << 5 | d; }
constexpr u32 CmpReg(u32 n, u32 m) { return 0xEB000000 | m << 16 | n << 5 | 31; }
constexpr u32 Blr(u32 n) { return 0xD63F0000 | n << 5; }
constexpr u32 Bl(s64 offset) { return 0x94000000 | (u32(offset / 4) & 0x3FFFFFF); }
constexpr u32 BCond(s32 offset, u32 cond) { return 0x54000000 | (u32(offset / 4) & 0x7FFFF) << 5 | cond; }
constexpr u32 CbnzW(u32 t, s32 offset) { return 0x35000000 | (u32(offset / 4) & 0x7FFFF) << 5 | t; }
constexpr u32 Ldaxr(u32 t, u32 n) { return 0xC85FFC00 | n << 5 | t; }
constexpr u32 Stlxr(u32 s, u32 t, u32 n) { return 0xC800FC00 | s << 16 | n << 5 | t; }
constexpr u32 Svc(u32 imm) { return 0xD4000001 | imm << 5; }
constexpr u32 Ret = 0xD65F03C0;
constexpr u32 Nop = 0xD503201F;
constexpr u32 CondNe = 1;
constexpr u32 CondLo = 3;

/// Calls func x3, x3 + 16, ... x1 times. After each it calls the mutable function, fails when x5
/// is older than x6 (the version published before the run started) and adds one to the counter.
constexpr std::array<u32, 13> Caller{
    Blr(3),
    Bl(s64(MutableBase) - s64(CallerBase + 4)),
    CmpReg(5, 6),
    BCond(9 * 4, CondLo), // to svc #1
    Ldaxr(9, 8),
    AddImm(9, 9, 1),
    Stlxr(10, 9, 8),
    CbnzW(10, -3 * 4),
    AddImm(3, 3, 16),
    SubsImm(1, 1, 1),
    BCond(-10 * 4, CondNe),
    Svc(0),
    Svc(1),
};

/// Guest memory all Jits see: the mutable function's version and the counter.
struct Guest {
    std::atomic<u64> version{1};   ///< What the mutable function returns now
    std::atomic<u64> published{1}; ///< A version whose invalidation has finished
    std::atomic<u64> counter{0};
};

class Env final : public A64::UserCallbacks {
public:
    explicit Env(Guest& guest_) : guest{guest_} {}

    Guest& guest;
    A64::Jit* jit = nullptr;
    std::optional<u32> svc;
    A64::UserConfig::Counters counters;

    std::optional<std::uint32_t> MemoryReadCode(u64 vaddr) override {
        if (vaddr - CallerBase < Caller.size() * 4) {
            return Caller[(vaddr - CallerBase) / 4];
        }
        if (vaddr == MutableBase) {
            return Movz(5, guest.version.load());
        }
        if (vaddr == MutableBase + 4) {
            return Ret;
        }
        if (vaddr == WriterBase) {
            return Bl(s64(MutableBase) - s64(WriterBase));
        }
        if (vaddr == WriterBase + 4) {
            return Svc(0);
        }
        if (vaddr >= FuncBase) {
            const u64 i = (vaddr - FuncBase) / 16;
            const std::array<u32, 4> func{Movz(2, i), AddReg(0, 0, 2), Ret, Nop};
            return func[(vaddr - FuncBase) / 4 % 4];
        }
        return Svc(2);
    }
    u64 MemoryRead(u64 vaddr, size_t size) override {
        return vaddr == CounterAddress && size == 8 ? guest.counter.load() : 0;
    }
    Vector MemoryRead128(u64) override { return {}; }
    void MemoryWrite(u64, u64, size_t) override { svc = 3; }
    void MemoryWrite128(u64, Vector) override { svc = 3; }
    bool MemoryWriteExclusive64(u64 vaddr, u64 value, u64 expected) override {
        return vaddr == CounterAddress && guest.counter.compare_exchange_strong(expected, value);
    }
    void CallSVC(std::uint32_t imm) override {
        svc = imm;
        jit->HaltExecution();
    }
    void ExceptionRaised(u64, A64::Exception) override {
        svc = 4;
        jit->HaltExecution();
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 0; }
    u64 GetCNTPCT() override { return 0; }
};

A64::UserConfig Config(Env& env, ExclusiveMonitor& monitor, std::shared_ptr<A64::SharedCodeCache> cache, u8 id) {
    A64::UserConfig config{};
    config.callbacks = &env;
    config.global_monitor = &monitor;
    config.processor_id = id;
    config.counters = &env.counters;
    config.shared_code_cache = std::move(cache);
    config.code_cache_size = 16 * 1024 * 1024;
    // ArmDynarmic64's multicore options: the ones a shared cache needs and those that change code.
    config.enable_cycle_counting = false;
    config.spill_to_free_gpr = true;
    config.scratch_copy_to_free_reg = true;
    config.elide_redundant_zext = true;
    config.fold_address_offsets = true;
    config.omit_block_frame = true;
    config.fast_dispatch_table_bits = 16;
    config.fast_dispatch_set_associative = true;
    config.rsb_size = 32;
    config.compact_block_exits = true;
    config.far_code = true;
    config.reuse_host_flags = true;
    config.halt_check_on_back_edges = true;
    config.host_call_return = true;
    return config;
}

/// Runs until the guest's svc: halts for cache invalidations and clears return early.
void RunToSvc(A64::Jit& jit, Env& env) {
    env.svc.reset();
    while (!env.svc) {
        jit.Run();
    }
}

/// What the Jits of a test counted, summed.
struct Totals {
    u64 clears = 0;
    u64 compiled = 0;
    u64 veneers = 0;
    u64 unlinked_far_links = 0;
};

Totals RunnersAndWriter() {
    Guest guest;
    ExclusiveMonitor monitor{Runners + 1};
    const auto cache = A64::MakeSharedCodeCache();

    std::vector<std::unique_ptr<Env>> envs;
    std::vector<std::unique_ptr<A64::Jit>> jits;
    for (size_t i = 0; i <= Runners; ++i) {
        envs.push_back(std::make_unique<Env>(guest));
        jits.push_back(std::make_unique<A64::Jit>(Config(*envs[i], monitor, cache, u8(i))));
        envs[i]->jit = jits[i].get();
    }

    std::atomic<size_t> finished{0};
    std::atomic<u64> bad_sums{0};
    std::atomic<u64> bad_svcs{0};
    std::vector<std::thread> threads;
    for (size_t r = 0; r < Runners; ++r) {
        threads.emplace_back([&, r] {
            A64::Jit& jit = *jits[r];
            Env& env = *envs[r];
            for (u64 round = 0; round < Rounds; ++round) {
                // Runner pairs share their functions; each round moves to new ones.
                const u64 start = (r % 2) * Iterations / 2 + round * Iterations;
                jit.SetRegister(0, 0);
                jit.SetRegister(1, Iterations);
                jit.SetRegister(3, FuncBase + 16 * start);
                jit.SetRegister(6, guest.published.load());
                jit.SetRegister(8, CounterAddress);
                jit.SetPC(CallerBase);
                RunToSvc(jit, env);
                u64 sum = 0;
                for (u64 i = start; i < start + Iterations; ++i) {
                    sum += i & 0xFFFF;
                }
                bad_sums += jit.GetRegister(0) != sum;
                bad_svcs += *env.svc != 0;
            }
            ++finished;
        });
    }

    u64 rewrites = 0;
    u64 stale = 0;
    threads.emplace_back([&] {
        A64::Jit& jit = *jits[Runners];
        Env& env = *envs[Runners];
        while (finished.load() < Runners && guest.version.load() < 0xFFFF) {
            const u64 version = guest.version.load() + 1;
            guest.version.store(version);
            jit.InvalidateCacheRange(MutableBase, 8);
            guest.published.store(version);
            if (++rewrites % 64 == 0) {
                jit.ClearCache();
            }
            jit.SetPC(WriterBase);
            RunToSvc(jit, env);
            stale += jit.GetRegister(5) != version;
            bad_svcs += *env.svc != 0;
        }
    });

    for (std::thread& thread : threads) {
        thread.join();
    }

    Totals totals;
    for (const auto& env : envs) {
        totals.clears += env->counters.cache_clears;
        totals.compiled += env->counters.blocks_compiled;
        totals.veneers += env->counters.far_link_veneers;
        totals.unlinked_far_links += env->counters.unlinked_far_links;
    }
    INFO("rewrites " << rewrites << ", blocks compiled " << totals.compiled << ", clears " << totals.clears);
    CHECK(bad_sums.load() == 0);
    CHECK(bad_svcs.load() == 0);
    CHECK(stale == 0);
    CHECK(guest.counter.load() == Runners * Rounds * Iterations);
    CHECK(rewrites > 0);
    CHECK(totals.clears > 0);
    return totals;
}

} // namespace

TEST_CASE("Jits on one code cache run while others compile, invalidate and clear", "[jit_shared_cache]") {
    RunnersAndWriter();
}

#if defined(__aarch64__)

#include <chrono>
#include <cstring>
#include <sys/mman.h>

namespace {

/// The arm64 backend's test setting: code cache regions of 2 MiB instead of 128 MiB, with links
/// counted as out of a branch's reach beyond that. Read when a code cache is made.
struct SmallRegions {
    SmallRegions() { setenv("DYNARMIC_ARM64_REGION_SIZE", "0x200000", 1); }
    ~SmallRegions() { unsetenv("DYNARMIC_ARM64_REGION_SIZE"); }
};

constexpr u32 LdrX(u32 t, u32 n) { return 0xF9400000 | n << 5 | t; }
constexpr u32 MovX(u32 d, u32 m) { return 0xAA0003E0 | m << 16 | d; }

constexpr u64 LoopBase = 0x1000;       // blr x3; add x3, x3, #16; subs x1, x1, #1; b.ne; svc #0
constexpr u64 Loop32Base = 0x1100;     // The same in steps of 32
constexpr u64 CommonBase = 0x4000;     // common j at CommonBase + 8 * j: add x0, x0, #inc; ret
constexpr u64 Commons = 8;
constexpr u64 CallerFuncBase = 0x100'0000; // func i at + 16 * i: mov x9, x30; bl common i % 8; mov x30, x9; ret
constexpr u64 LoaderFuncBase = 0x4000'0000; // func i at + 32 * i: ldr x2, [x7]; ldr x4, [x8]; x0 += x2 + x4; ret

constexpr std::array<u32, 5> Loop{
    Blr(3), AddImm(3, 3, 16), SubsImm(1, 1, 1), BCond(-3 * 4, CondNe), Svc(0),
};
constexpr std::array<u32, 5> Loop32{
    Blr(3), AddImm(3, 3, 32), SubsImm(1, 1, 1), BCond(-3 * 4, CondNe), Svc(0),
};

/// Guest code for the tests below. Data reads that miss the fastmem arena return `callback_value`.
class CodeEnv final : public A64::UserCallbacks {
public:
    A64::Jit* jit = nullptr;
    std::optional<u32> svc;
    A64::UserConfig::Counters counters;
    std::atomic<u32>* common_increment = nullptr;
    u64 callback_value = 0;
    std::atomic<u64>* callback_reads = nullptr;

    std::optional<std::uint32_t> MemoryReadCode(u64 vaddr) override {
        if (vaddr - LoopBase < Loop.size() * 4) {
            return Loop[(vaddr - LoopBase) / 4];
        }
        if (vaddr - Loop32Base < Loop32.size() * 4) {
            return Loop32[(vaddr - Loop32Base) / 4];
        }
        if (vaddr - CommonBase < Commons * 8) {
            return (vaddr & 4) ? Ret : AddImm(0, 0, common_increment->load());
        }
        if (vaddr >= LoaderFuncBase) {
            const std::array<u32, 8> func{LdrX(2, 7), LdrX(4, 8), AddReg(0, 0, 2), AddReg(0, 0, 4), Ret, Nop, Nop, Nop};
            return func[(vaddr - LoaderFuncBase) / 4 % 8];
        }
        if (vaddr >= CallerFuncBase) {
            const u64 i = (vaddr - CallerFuncBase) / 16;
            const u64 bl_address = CallerFuncBase + 16 * i + 4;
            const std::array<u32, 4> func{MovX(9, 30), Bl(s64(CommonBase + 8 * (i % Commons)) - s64(bl_address)), MovX(30, 9), Ret};
            return func[(vaddr - CallerFuncBase) / 4 % 4];
        }
        return Svc(2);
    }
    u64 MemoryRead(u64, size_t) override {
        if (callback_reads) {
            ++*callback_reads;
        }
        return callback_value;
    }
    Vector MemoryRead128(u64) override { return {}; }
    void MemoryWrite(u64, u64, size_t) override { svc = 3; }
    void MemoryWrite128(u64, Vector) override { svc = 3; }
    void CallSVC(std::uint32_t imm) override {
        svc = imm;
        jit->HaltExecution();
    }
    void ExceptionRaised(u64, A64::Exception) override {
        svc = 4;
        jit->HaltExecution();
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 0; }
    u64 GetCNTPCT() override { return 0; }
};

/// Calls `count` functions of 16 or 32 bytes from `first` on and returns x0, or ~0 if that failed.
u64 CallFunctions(A64::Jit& jit, CodeEnv& env, u64 first, u64 count, u64 stride) {
    jit.SetRegister(0, 0);
    jit.SetRegister(1, count);
    jit.SetRegister(3, first);
    jit.SetPC(stride == 16 ? LoopBase : Loop32Base);
    env.svc.reset();
    while (!env.svc) {
        jit.Run();
    }
    return *env.svc == 0 ? jit.GetRegister(0) : ~u64{0};
}

} // namespace

TEST_CASE("Jits on one code cache of several regions link through veneers", "[jit_shared_cache]") {
    const SmallRegions small_regions;
    constexpr size_t Cores = 4;
    constexpr u64 Functions = 8000;
    std::atomic<u32> increment{1};
    std::atomic<u64> bad_sums{0};
    std::atomic<size_t> finished{0};
    const auto cache = A64::MakeSharedCodeCache();
    std::vector<std::unique_ptr<CodeEnv>> envs;
    std::vector<std::unique_ptr<A64::Jit>> jits;
    for (size_t i = 0; i <= Cores; ++i) {
        envs.push_back(std::make_unique<CodeEnv>());
        envs[i]->common_increment = &increment;
        A64::UserConfig config{};
        config.callbacks = envs[i].get();
        config.counters = &envs[i]->counters;
        config.processor_id = u8(i);
        config.shared_code_cache = cache;
        config.code_cache_size = 16 * 1024 * 1024;
        config.enable_cycle_counting = false;
        jits.push_back(std::make_unique<A64::Jit>(config));
        envs[i]->jit = jits[i].get();
    }

    std::vector<std::thread> threads;
    for (size_t core = 0; core < Cores; ++core) {
        threads.emplace_back([&, core] {
            // More functions than the cache holds.
            for (u64 round = 0; round < 2 * Rounds; ++round) {
                // Core pairs run the same functions at the same time, then all run the first ones.
                const u64 first = CallerFuncBase + 16 * Functions * (round + core % 2);
                bad_sums += CallFunctions(*jits[core], *envs[core], first, Functions, 16) != Functions;
                bad_sums += CallFunctions(*jits[core], *envs[core], CallerFuncBase, Functions, 16) != Functions;
            }
            ++finished;
        });
    }
    // The common functions are dropped and compiled again meanwhile: their callers' links and
    // veneers follow. Not more often or for longer: each time relinks every caller.
    u64 invalidations = 0;
    threads.emplace_back([&] {
        while (finished.load() < Cores && invalidations < 24) {
            jits[Cores]->InvalidateCacheRange(CommonBase, Commons * 8);
            ++invalidations;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    for (std::thread& thread : threads) {
        thread.join();
    }

    Totals totals;
    for (const auto& env : envs) {
        totals.clears += env->counters.cache_clears;
        totals.compiled += env->counters.blocks_compiled;
        totals.veneers += env->counters.far_link_veneers;
        totals.unlinked_far_links += env->counters.unlinked_far_links;
    }
    INFO("invalidations " << invalidations << ", blocks compiled " << totals.compiled << ", clears " << totals.clears << ", veneers " << totals.veneers);
    CHECK(bad_sums.load() == 0);
    CHECK(totals.veneers > 0);
    CHECK(totals.unlinked_far_links == 0);
    CHECK(totals.clears > 0);
}

TEST_CASE("Blocks out of a branch's reach link through veneers that follow their target", "[jit_shared_cache]") {
    const SmallRegions small_regions;
    std::atomic<u32> increment{1};
    CodeEnv env;
    env.common_increment = &increment;
    A64::UserConfig config{};
    config.callbacks = &env;
    config.counters = &env.counters;
    config.code_cache_size = 16 * 1024 * 1024;
    config.enable_cycle_counting = false;
    A64::Jit jit{config};
    env.jit = &jit;

    // Enough functions for several regions, each calling a common one compiled first.
    constexpr u64 Functions = 20000;
    CHECK(CallFunctions(jit, env, CallerFuncBase, Functions, 16) == Functions);
    const u64 veneers = env.counters.far_link_veneers;
    INFO("veneers " << veneers << ", code bytes " << env.counters.code_bytes << ", clears " << env.counters.cache_clears);
    CHECK(veneers > 0);
    CHECK(env.counters.unlinked_far_links == 0);
    CHECK(env.counters.cache_clears == 0);
    // All linked now.
    CHECK(CallFunctions(jit, env, CallerFuncBase, Functions, 16) == Functions);

    // New common functions, in another region: the veneers lead to them, and their old
    // neighbours get veneers too.
    increment = 2;
    jit.InvalidateCacheRange(CommonBase, Commons * 8);
    CHECK(CallFunctions(jit, env, CallerFuncBase, Functions, 16) == 2 * Functions);
    CHECK(env.counters.far_link_veneers > veneers);
    CHECK(env.counters.unlinked_far_links == 0);
    CHECK(env.counters.blocks_invalidated >= Commons);

    // More code than the cache holds: it clears and starts over in the first region.
    CHECK(CallFunctions(jit, env, CallerFuncBase, 8 * Functions, 16) == 2 * 8 * Functions);
    CHECK(env.counters.cache_clears > 0);
    CHECK(env.counters.full_clears > 0);
    CHECK(CallFunctions(jit, env, CallerFuncBase, Functions, 16) == 2 * Functions);
}

TEST_CASE("Jits on one code cache take fastmem faults in each other's blocks", "[jit_shared_cache]") {
    // A fastmem arena of 256 KiB whose upper half is not mapped: reads there fault, take the
    // callback and have their block compiled again without fastmem.
    constexpr size_t ArenaBits = 18;
    constexpr size_t ArenaSize = size_t{1} << ArenaBits;
    constexpr u64 MappedAddress = 0x100;
    constexpr u64 UnmappedAddress = ArenaSize / 2 + 0x100;
    constexpr u64 MappedValue = 5;
    constexpr u64 CallbackValue = 3;
    void* const arena = mmap(nullptr, ArenaSize, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    REQUIRE(arena != MAP_FAILED);
    REQUIRE(mprotect(arena, ArenaSize / 2, PROT_READ | PROT_WRITE) == 0);
    std::memcpy(static_cast<char*>(arena) + MappedAddress, &MappedValue, sizeof(MappedValue));

    constexpr size_t Cores = 4;
    constexpr u64 FunctionsPerRound = 1500;
    std::atomic<u32> increment{1};
    std::atomic<u64> callback_reads{0};
    std::atomic<u64> bad_sums{0};
    const auto cache = A64::MakeSharedCodeCache();
    std::vector<std::unique_ptr<CodeEnv>> envs;
    std::vector<std::unique_ptr<A64::Jit>> jits;
    for (size_t i = 0; i < Cores; ++i) {
        envs.push_back(std::make_unique<CodeEnv>());
        envs[i]->common_increment = &increment;
        envs[i]->callback_value = CallbackValue;
        envs[i]->callback_reads = &callback_reads;
        A64::UserConfig config{};
        config.callbacks = envs[i].get();
        config.counters = &envs[i]->counters;
        config.processor_id = u8(i);
        config.shared_code_cache = cache;
        config.code_cache_size = 16 * 1024 * 1024;
        config.enable_cycle_counting = false;
        config.fastmem_pointer = reinterpret_cast<uintptr_t>(arena);
        config.fastmem_address_space_bits = ArenaBits;
        config.silently_mirror_fastmem = false;
        config.recompile_on_fastmem_failure = true;
        jits.push_back(std::make_unique<A64::Jit>(config));
        envs[i]->jit = jits[i].get();
    }

    std::vector<std::thread> threads;
    for (size_t core = 0; core < Cores; ++core) {
        threads.emplace_back([&, core] {
            A64::Jit& jit = *jits[core];
            jit.SetRegister(7, UnmappedAddress);
            jit.SetRegister(8, MappedAddress);
            for (u64 round = 0; round < Rounds; ++round) {
                // Core pairs run the same functions at the same time, and each twice.
                const u64 first = LoaderFuncBase + 32 * FunctionsPerRound * (2 * round + core % 2);
                for (int pass = 0; pass < 2; ++pass) {
                    const u64 sum = CallFunctions(jit, *envs[core], first, FunctionsPerRound, 32);
                    bad_sums += sum != FunctionsPerRound * (MappedValue + CallbackValue);
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    u64 recompiles = 0;
    for (const auto& env : envs) {
        recompiles += env->counters.fastmem_recompiles;
    }
    INFO("fastmem recompiles " << recompiles << ", callback reads " << callback_reads.load());
    CHECK(bad_sums.load() == 0);
    CHECK(callback_reads.load() == Cores * Rounds * 2 * FunctionsPerRound);
    CHECK(recompiles >= 2 * Rounds * FunctionsPerRound);
    jits.clear();
    munmap(arena, ArenaSize);
}

// Hidden: compiles 300 MiB of code. Run with "[jit_large_cache]".
namespace {

constexpr u64 BlockedBase = 0x4000;  // svc #5 (its callback waits to be released); svc #0
constexpr u64 PlainBase = 0x5000;    // svc #0
constexpr u64 PatchedBase = 0x6000;  // movz x5, #value; svc #0

/// Guest code whose svc #5 stays in its callback until released, as a slow service call does.
class WaitEnv final : public A64::UserCallbacks {
public:
    A64::Jit* jit = nullptr;
    std::optional<u32> svc;
    A64::UserConfig::Counters counters;
    std::atomic<bool> in_callback{false};
    std::atomic<bool> release{false};
    std::atomic<u64> patched_value{1};

    std::optional<std::uint32_t> MemoryReadCode(u64 vaddr) override {
        switch (vaddr) {
        case BlockedBase: return Svc(5);
        case PatchedBase: return Movz(5, patched_value.load());
        default: return Svc(0);
        }
    }
    u64 MemoryRead(u64, size_t) override { return 0; }
    Vector MemoryRead128(u64) override { return {}; }
    void MemoryWrite(u64, u64, size_t) override {}
    void MemoryWrite128(u64, Vector) override {}
    void CallSVC(std::uint32_t imm) override {
        if (imm == 5) {
            in_callback.store(true);
            while (!release.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
        svc = imm;
        jit->HaltExecution();
    }
    void ExceptionRaised(u64, A64::Exception) override {
        svc = 4;
        jit->HaltExecution();
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 0; }
    u64 GetCNTPCT() override { return 0; }
};

A64::UserConfig WaitConfig(WaitEnv& env, ExclusiveMonitor& monitor, std::shared_ptr<A64::SharedCodeCache> cache, u8 id) {
    A64::UserConfig config{};
    config.callbacks = &env;
    config.global_monitor = &monitor;
    config.processor_id = id;
    config.counters = &env.counters;
    config.shared_code_cache = std::move(cache);
    config.code_cache_size = 16 * 1024 * 1024;
    config.enable_cycle_counting = false;
    return config;
}

template <typename Condition>
bool WaitFor(Condition condition, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

} // namespace

TEST_CASE("A Jit waiting for a cache clear returns for another halt", "[jit_shared_cache]") {
    ExclusiveMonitor monitor{2};
    const auto cache = A64::MakeSharedCodeCache();
    WaitEnv blocked_env, waiting_env;
    A64::Jit blocked{WaitConfig(blocked_env, monitor, cache, 0)};
    A64::Jit waiting{WaitConfig(waiting_env, monitor, cache, 1)};
    blocked_env.jit = &blocked;
    waiting_env.jit = &waiting;

    // One Jit sits in a callback of its guest code: it runs code, so a full clear has to wait for it.
    blocked.SetPC(BlockedBase);
    std::thread blocked_thread{[&] {
        while (!blocked_env.svc) {
            blocked.Run();
        }
    }};
    REQUIRE(WaitFor([&] { return blocked_env.in_callback.load(); }, std::chrono::seconds{10}));
    waiting.ClearCache();

    // The other Jit waits for that clear. Asked to halt for another reason, it has to return without running.
    waiting.SetPC(PlainBase);
    std::atomic<bool> returned{false};
    HaltReason reason{};
    std::thread waiting_thread{[&] {
        reason = waiting.Run();
        returned.store(true);
    }};
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    CHECK(!returned.load());
    waiting.HaltExecution(HaltReason::UserDefined1);
    const bool answered = WaitFor([&] { return returned.load(); }, std::chrono::seconds{3});
    blocked_env.release.store(true);  // also when it failed: nothing may stay blocked
    waiting_thread.join();
    blocked_thread.join();
    REQUIRE(answered);
    CHECK(Has(reason, HaltReason::UserDefined1));
    CHECK(!waiting_env.svc);
    CHECK(waiting.GetPC() == PlainBase);

    // With the first Jit out of its code the clear happens, and the second runs as usual.
    while (!waiting_env.svc) {
        waiting.Run();
    }
    CHECK(*waiting_env.svc == 0);
    CHECK(blocked_env.counters.cache_clears + waiting_env.counters.cache_clears == 1);
}

TEST_CASE("One invalidation asked of every Jit of a code cache counts once", "[jit_shared_cache]") {
    constexpr size_t Jits = 6;
    ExclusiveMonitor monitor{Jits};
    const auto cache = A64::MakeSharedCodeCache();
    std::array<WaitEnv, Jits> envs;
    std::vector<std::unique_ptr<A64::Jit>> jits;
    for (size_t i = 0; i < Jits; ++i) {
        jits.push_back(std::make_unique<A64::Jit>(WaitConfig(envs[i], monitor, cache, static_cast<u8>(i))));
        envs[i].jit = jits[i].get();
    }
    const auto run = [&](size_t i) {
        envs[i].svc.reset();
        jits[i]->SetPC(PatchedBase);
        while (!envs[i].svc) {
            jits[i]->Run();
        }
        return jits[i]->GetRegister(5);
    };
    const auto requests = [&] {
        u64 sum = 0;
        for (const WaitEnv& env : envs) {
            sum += env.counters.invalidation_requests;
        }
        return sum;
    };
    const auto invalidate_on_all = [&] {
        for (const auto& jit : jits) {
            jit->InvalidateCacheRange(PatchedBase, 4);
        }
    };

    REQUIRE(run(0) == 1);
    // The program patches the code and tells every core's Jit.
    envs[0].patched_value = 2;
    for (WaitEnv& env : envs) {
        env.patched_value = 2;
    }
    invalidate_on_all();
    CHECK(requests() == 1);
    for (size_t i = 0; i < Jits; ++i) {
        CHECK(run(i) == 2);
    }
    // The same range again after its block was compiled anew: a new request, once.
    for (WaitEnv& env : envs) {
        env.patched_value = 3;
    }
    invalidate_on_all();
    CHECK(requests() == 2);
    for (size_t i = 0; i < Jits; ++i) {
        CHECK(run(i) == 3);
    }
    // Twice with no block compiled in between (patched and patched back): nothing to drop the second time,
    // and the code that runs is the latest.
    for (WaitEnv& env : envs) {
        env.patched_value = 4;
    }
    invalidate_on_all();
    for (WaitEnv& env : envs) {
        env.patched_value = 5;
    }
    invalidate_on_all();
    CHECK(requests() == 3);
    for (size_t i = 0; i < Jits; ++i) {
        CHECK(run(i) == 5);
    }
}

TEST_CASE("A code cache of three 128 MiB regions links across them", "[.][jit_large_cache]") {
    std::atomic<u32> increment{1};
    CodeEnv env;
    env.common_increment = &increment;
    A64::UserConfig config{};
    config.callbacks = &env;
    config.counters = &env.counters;
    config.code_cache_size = 384 * 1024 * 1024;
    config.enable_cycle_counting = false;
    A64::Jit jit{config};
    env.jit = &jit;

    // In chunks, until the code is in the third region.
    constexpr u64 Chunk = 100000;
    u64 functions = 0;
    while (env.counters.code_bytes < 300ull * 1024 * 1024) {
        REQUIRE(functions < 40 * Chunk);
        REQUIRE(CallFunctions(jit, env, CallerFuncBase + 16 * functions, Chunk, 16) == Chunk);
        functions += Chunk;
    }
    INFO("functions " << functions << ", veneers " << env.counters.far_link_veneers << ", code bytes " << env.counters.code_bytes);
    CHECK(env.counters.cache_clears == 0);
    CHECK(env.counters.far_link_veneers > 0);
    CHECK(env.counters.unlinked_far_links == 0);
    // Every function again, linked, then with new common functions in the third region.
    CHECK(CallFunctions(jit, env, CallerFuncBase, functions, 16) == functions);
    increment = 2;
    jit.InvalidateCacheRange(CommonBase, Commons * 8);
    CHECK(CallFunctions(jit, env, CallerFuncBase, functions, 16) == 2 * functions);
    CHECK(env.counters.unlinked_far_links == 0);
    CHECK(env.counters.cache_clears == 0);
}

#endif
