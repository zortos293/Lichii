// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Replays a DYNARMIC_JIT_TRACE recording (backend/arm64/jit_trace.h) through the arm64 code cache, with no
// emulator and no guest code run: every recorded dispatcher lookup, compile, fastmem fault and invalidation is
// done again, on one thread per recorded Jit, and the cache's own counters say what it cost. A benchmark for
// ideas about the cache (locking, lookups, fault handling); it cannot show the kernel's cost of a fault or how
// fast the emitted code runs.
//
//   DYNARMIC_JIT_REPLAY=<file> ./dynarmic_tests "[jit_replay]"
//   DYNARMIC_JIT_REPLAY_SHARED=0|1   one cache for all Jits or one each (default: as recorded)
//   DYNARMIC_JIT_REPLAY_MIB=<n>      code cache size (default: as recorded)
//   DYNARMIC_JIT_REPLAY_SPEED=<x>    recorded time divided by x between operations; 0 (default) = no waiting
//   DYNARMIC_JIT_REPLAY_UNTIL_S=<s>  only the first s seconds of the recording

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "dynarmic/backend/arm64/jit_trace.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

using namespace Dynarmic;
using Backend::Arm64::JitTrace::Record;
using Backend::Arm64::JitTrace::Type;

namespace {

struct Op {
    u64 time_ns;
    u64 a;
    u64 b;
    Type type;
    u16 flags;
    u8 jit;
};

class ReplayEnv final : public A64::UserCallbacks {
public:
    explicit ReplayEnv(const std::unordered_map<u64, u32>& code_) : code{code_} {}

    std::optional<std::uint32_t> MemoryReadCode(u64 vaddr) override {
        const auto word = code.find(vaddr);
        return word != code.end() ? std::optional<std::uint32_t>{word->second} : std::nullopt;
    }
    // No guest code runs, so nothing else is called.
    u64 MemoryRead(u64, size_t) override { return 0; }
    A64::Vector MemoryRead128(u64) override { return {}; }
    void MemoryWrite(u64, std::uint64_t, size_t) override {}
    void MemoryWrite128(u64, A64::Vector) override {}
    bool MemoryWriteExclusive8(u64, std::uint8_t, std::uint8_t) override { return true; }
    bool MemoryWriteExclusive16(u64, std::uint16_t, std::uint16_t) override { return true; }
    bool MemoryWriteExclusive32(u64, std::uint32_t, std::uint32_t) override { return true; }
    bool MemoryWriteExclusive64(u64, std::uint64_t, std::uint64_t) override { return true; }
    bool MemoryWriteExclusive128(u64, A64::Vector, A64::Vector) override { return true; }
    void CallSVC(std::uint32_t) override {}
    void ExceptionRaised(u64, A64::Exception) override {}
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1'000'000; }
    std::uint64_t GetCNTPCT() override { return 0; }

private:
    const std::unordered_map<u64, u32>& code;
};

// The entry points each location's block has had, as the replay saw them: a stale fault needs the one before.
struct Entries {
    struct Pair {
        void* current = nullptr;
        void* previous = nullptr;
    };
    static constexpr size_t stripes = 256;
    std::array<std::mutex, stripes> mutexes;
    std::array<std::unordered_map<u64, Pair>, stripes> maps;

    static size_t Stripe(u64 key) {
        return (key ^ (key >> 17) ^ (key >> 31)) % stripes;
    }
    void Saw(u64 key, void* entry_point) {
        const size_t stripe = Stripe(key);
        std::scoped_lock lock{mutexes[stripe]};
        Pair& pair = maps[stripe][key];
        if (pair.current != entry_point) {
            pair.previous = pair.current;
            pair.current = entry_point;
        }
    }
    void* Previous(u64 key) {
        const size_t stripe = Stripe(key);
        std::scoped_lock lock{mutexes[stripe]};
        const auto pair = maps[stripe].find(key);
        return pair != maps[stripe].end() ? pair->second.previous : nullptr;
    }
};

double EnvNumber(const char* name, double fallback) {
    const char* const value = std::getenv(name);
    return value && *value ? std::strtod(value, nullptr) : fallback;
}

}  // namespace

TEST_CASE("A64: replay a recorded JIT cache trace", "[.][jit_replay]") {
    const char* const path = std::getenv("DYNARMIC_JIT_REPLAY");
    if (!path || !*path) {
        SKIP("set DYNARMIC_JIT_REPLAY to a DYNARMIC_JIT_TRACE recording");
    }
    std::FILE* const file = std::fopen(path, "rb");
    REQUIRE(file != nullptr);

    // Read: operations per Jit, the faults, and every guest instruction any block was translated from.
    const u64 until_ns = static_cast<u64>(EnvNumber("DYNARMIC_JIT_REPLAY_UNTIL_S", 1e9) * 1e9);
    std::unordered_map<u64, u32> code;
    std::array<std::vector<Op>, 256> by_jit;
    u64 fault_count = 0;
    std::unordered_map<u64, u8> last_compiler;  // location -> the Jit that compiled it last (for private caches)
    u64 recorded_cache_size = 0;
    bool recorded_shared = false;
    u64 compiles = 0, last_time = 0;
    std::vector<std::pair<u64, u64>> words;
    for (Record record; std::fread(&record, sizeof(record), 1, file) == 1;) {
        const Type type = static_cast<Type>(record.type);
        if (type == Type::Compile) {
            words.resize(record.count);
            if (record.count && std::fread(words.data(), sizeof(words.front()), record.count, file) != record.count) {
                break;
            }
            for (const auto& [vaddr, word] : words) {
                code.emplace(vaddr, static_cast<u32>(word));
            }
            last_compiler[record.a] = record.jit;
            ++compiles;
            continue;
        }
        if (record.time_ns > until_ns) {
            continue;
        }
        last_time = std::max(last_time, record.time_ns);
        const Op op{record.time_ns, record.a, record.b, type, record.flags, record.jit};
        switch (type) {
        case Type::Header:
            if (recorded_cache_size == 0) {
                recorded_cache_size = record.a;
                recorded_shared = record.flags & 1;
            }
            break;
        case Type::Lookup:
        case Type::Invalidate:
            by_jit[record.jit].push_back(op);
            break;
        case Type::Fault: {
            Op fault = op;
            // Recorded without its Jit: with private caches, the Jit that compiled the block last.
            const auto compiler = last_compiler.find(record.a);
            fault.jit = compiler != last_compiler.end() ? compiler->second : 0;
            by_jit[fault.jit].push_back(fault);
            ++fault_count;
            break;
        }
        default:
            break;
        }
    }
    std::fclose(file);

    // A fault is replayed by the Jit that compiled its block, in the order of the recording: on the real machine
    // that core waits while the exception thread handles it, and the block must have been compiled first.
    std::vector<u8> jit_ids;
    u64 lookups = 0;
    for (size_t jit = 0; jit < by_jit.size(); ++jit) {
        if (!by_jit[jit].empty()) {
            std::stable_sort(by_jit[jit].begin(), by_jit[jit].end(), [](const Op& x, const Op& y) { return x.time_ns < y.time_ns; });
            jit_ids.push_back(static_cast<u8>(jit));
            lookups += static_cast<u64>(std::count_if(by_jit[jit].begin(), by_jit[jit].end(), [](const Op& op) { return op.type == Type::Lookup; }));
        }
    }
    REQUIRE(!jit_ids.empty());

    const bool shared = EnvNumber("DYNARMIC_JIT_REPLAY_SHARED", recorded_shared ? 1 : 0) != 0;
    const u64 cache_mib = static_cast<u64>(EnvNumber("DYNARMIC_JIT_REPLAY_MIB", static_cast<double>(recorded_cache_size >> 20)));
    const double speed = EnvNumber("DYNARMIC_JIT_REPLAY_SPEED", 0);
    std::printf("trace %s: %.1f s recorded, %zu Jits, %llu lookups, %llu compiles, %zu faults, %zu instructions; recorded %s %llu MiB\n",
                path, static_cast<double>(last_time) / 1e9, jit_ids.size(), static_cast<unsigned long long>(lookups),
                static_cast<unsigned long long>(compiles), static_cast<size_t>(fault_count), code.size(), recorded_shared ? "shared" : "private",
                static_cast<unsigned long long>(recorded_cache_size >> 20));

    // Jits configured as the emulator's (fastmem with recompiling, the unsafe options of its default accuracy).
    ReplayEnv env{code};
    ExclusiveMonitor monitor{jit_ids.size()};
    std::array<u64, 256> tpidr{}, tpidrro{};
    std::vector<A64::UserConfig::Counters> counters(jit_ids.size());
    std::vector<std::unique_ptr<A64::Jit>> jits;
    std::array<A64::Jit*, 256> jit_of{};
    const auto shared_cache = shared ? A64::MakeSharedCodeCache() : nullptr;
    for (size_t index = 0; index < jit_ids.size(); ++index) {
        A64::UserConfig conf;
        conf.callbacks = &env;
        conf.processor_id = index;
        conf.global_monitor = &monitor;
        conf.tpidr_el0 = &tpidr[index];
        conf.tpidrro_el0 = &tpidrro[index];
        conf.dczid_el0 = 4;
        conf.ctr_el0 = 0x8444c004;
        conf.fastmem_pointer = std::uintptr_t{0x200000000000};  // never dereferenced: no code runs
        conf.fastmem_address_space_bits = 64;
        conf.silently_mirror_fastmem = false;
        conf.fastmem_exclusive_access = true;
        conf.recompile_on_exclusive_fastmem_failure = true;
        conf.define_unpredictable_behaviour = true;
        conf.wall_clock_cntpct = true;
        conf.enable_cycle_counting = false;
        conf.unsafe_optimizations = true;
        conf.optimizations |= OptimizationFlag::Unsafe_UnfuseFMA;
        conf.optimizations |= OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        conf.code_cache_size = static_cast<std::uint32_t>(cache_mib << 20);
        conf.shared_code_cache = shared_cache;
        conf.counters = &counters[index];
        jits.push_back(std::make_unique<A64::Jit>(conf));
        jit_of[jit_ids[index]] = jits.back().get();
    }

    Entries entries;
    const auto entry_key = [shared](u8 jit, u64 descriptor) { return shared ? descriptor : descriptor ^ (u64{jit} << 56); };
    std::atomic<u64> faults_done{0}, faults_missed{0};
    std::atomic<u64> busy_ns{0};
    const auto start = std::chrono::steady_clock::now();
    const auto wait_for = [&](u64 time_ns) {
        if (speed <= 0) {
            return;
        }
        const auto due = start + std::chrono::nanoseconds{static_cast<u64>(static_cast<double>(time_ns) / speed)};
        if (due - std::chrono::steady_clock::now() > std::chrono::microseconds{50}) {
            std::this_thread::sleep_until(due);
        }
    };

    std::vector<std::thread> threads;
    for (const u8 id : jit_ids) {
        threads.emplace_back([&, id] {
            A64::Jit& jit = *jit_of[id];
            u64 busy = 0;
            for (const Op& op : by_jit[id]) {
                wait_for(op.time_ns);
                const auto begin = std::chrono::steady_clock::now();
                if (op.type == Type::Lookup) {
                    entries.Saw(entry_key(id, op.a), jit.DebugLookupBlock(op.a));
                } else if (op.type == Type::Fault) {
                    void* const stale = (op.flags & 1) ? entries.Previous(entry_key(id, op.a)) : nullptr;
                    const bool done = (!(op.flags & 1) || stale) && jit.DebugFastmemFault(op.a, static_cast<unsigned>(op.b), stale);
                    (done ? faults_done : faults_missed).fetch_add(1, std::memory_order_relaxed);
                } else {
                    jit.InvalidateCacheRange(op.a, op.b);
                }
                busy += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
            }
            busy_ns.fetch_add(busy);
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    const double wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    A64::UserConfig::Counters sum;
    for (const auto& c : counters) {
        sum.blocks_compiled += c.blocks_compiled;
        sum.compile_ns += c.compile_ns;
        sum.block_lookups += c.block_lookups;
        sum.lookup_hits += c.lookup_hits;
        sum.lookup_lock_wait_ns += c.lookup_lock_wait_ns;
        sum.emit_lock_wait_ns += c.emit_lock_wait_ns;
        sum.emit_lock_hold_ns += c.emit_lock_hold_ns;
        sum.discarded_translations += c.discarded_translations;
        sum.compiles_with_company += c.compiles_with_company;
        sum.fastmem_recompiles += c.fastmem_recompiles;
        sum.fastmem_stale_faults += c.fastmem_stale_faults;
        sum.fastmem_handler_ns += c.fastmem_handler_ns;
        sum.fastmem_lock_wait_ns += c.fastmem_lock_wait_ns;
        sum.cache_clears += c.cache_clears;
        sum.far_link_veneers += c.far_link_veneers;
    }
    const auto ms = [](u64 ns) { return static_cast<double>(ns) / 1e6; };
    std::printf("replay %s %llu MiB, speed %g: wall %.2f s, threads busy %.2f s\n", shared ? "shared" : "private",
                static_cast<unsigned long long>(cache_mib), speed, wall_s, static_cast<double>(busy_ns.load()) / 1e9);
    std::printf("  blocks %llu, compile %.0f ms (emit hold %.0f ms), lookups %llu (+%llu own-table hits), discarded %llu, with company %llu\n",
                static_cast<unsigned long long>(sum.blocks_compiled), ms(sum.compile_ns), ms(sum.emit_lock_hold_ns),
                static_cast<unsigned long long>(sum.block_lookups), static_cast<unsigned long long>(sum.lookup_hits),
                static_cast<unsigned long long>(sum.discarded_translations), static_cast<unsigned long long>(sum.compiles_with_company));
    std::printf("  lock waits: lookup %.0f ms, emit %.0f ms, fault handler %.0f ms (handler total %.0f ms)\n",
                ms(sum.lookup_lock_wait_ns), ms(sum.emit_lock_wait_ns), ms(sum.fastmem_lock_wait_ns), ms(sum.fastmem_handler_ns));
    std::printf("  faults replayed %llu, not replayable %llu; recompiles %llu, stale %llu; clears %llu, veneers %llu\n",
                static_cast<unsigned long long>(faults_done.load()), static_cast<unsigned long long>(faults_missed.load()),
                static_cast<unsigned long long>(sum.fastmem_recompiles), static_cast<unsigned long long>(sum.fastmem_stale_faults),
                static_cast<unsigned long long>(sum.cache_clears), static_cast<unsigned long long>(sum.far_link_veneers));
    REQUIRE(sum.blocks_compiled > 0);
}
