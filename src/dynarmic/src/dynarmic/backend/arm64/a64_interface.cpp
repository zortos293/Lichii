// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <vector>

#include <boost/icl/interval_set.hpp>
#include "common/assert.h"
#include "common/common_types.h"

#include "dynarmic/backend/arm64/a64_address_space.h"
#include "dynarmic/backend/arm64/jit_trace.h"
#include "dynarmic/backend/arm64/a64_jitstate.h"
#include "dynarmic/common/atomic.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opt_passes.h"

namespace Dynarmic::A64 {

using namespace Backend::Arm64;

/// The code, block map and links of one or more Jits (UserConfig::shared_code_cache). Code runs
/// while other Jits compile: lookups take `mutex` shared, emitting and invalidating take it
/// exclusively, and a link changes with one aligned store. A dropped block's code stays until a
/// full clear, which waits until no Jit runs code. Each Jit marks in its own `running` flag
/// whether it runs code, so entering and leaving touch no cache line other Jits write.
/// Each Jit keeps the blocks it looked up in a table of this size, so its dispatcher takes no lock for them.
constexpr std::size_t lookup_cache_size = 1 << 13;

constexpr std::size_t LookupCacheIndex(u64 descriptor) {
    return static_cast<std::size_t>((descriptor >> 2) ^ (descriptor >> 15) ^ (descriptor >> 37)) & (lookup_cache_size - 1);
}

struct SharedCodeCache final {
    struct Member {
        volatile u32* halt_reason;
        const std::atomic<bool>* running;
    };

    /// Builds the cache for the first Jit; later Jits must emulate the same process.
    void Attach(Member member, const UserConfig& jit_conf, A64AddressSpace::LookupBlock lookup_block) {
        std::unique_lock lock{mutex};
        if (!address_space) {
            conf.emplace(jit_conf);
            conf->shared_code_cache.reset();  // the cache must not own itself
            address_space.emplace(*conf, lookup_block);
            if (JitTrace::Enabled()) {
                JitTrace::Write(JitTrace::Type::Header, 0, jit_conf.shared_code_cache ? 1 : 0, conf->code_cache_size, 0);
            }
            fastmem_fault = [this](u64 host_pc) {
                // Faults come from JIT code, which never runs while its thread holds `mutex`.
                // A fault drops at most the one block it happened in. That block's code stays and stays correct
                // (its fastmem accesses take the slow path), its links are gone, and slot_drops makes every
                // Jit look the block up again, so no Jit has to be halted for it. Halting all of them for each
                // of the several hundred thousand faults of a load made loads slower than with private caches.
                const auto begin = std::chrono::steady_clock::now();
                std::unique_lock fault_lock{mutex};
                const auto locked = std::chrono::steady_clock::now();
                const Backend::FakeCall result = address_space->HandleFastmemFault(host_pc);
                fault_count.fetch_add(1, std::memory_order_relaxed);
                fault_lock_wait_ns.fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(locked - begin).count()), std::memory_order_relaxed);
                fault_handler_ns.fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count()), std::memory_order_relaxed);
                return result;
            };
            address_space->SetFastmemCallback(fastmem_fault);
            address_space->SetBlockDroppedCallback([this](IR::LocationDescriptor descriptor) {
                slot_drops[LookupCacheIndex(descriptor.Value())].fetch_add(1, std::memory_order_release);
                drops.fetch_add(1, std::memory_order_release);
            });
        } else {
            ASSERT(jit_conf.page_table == conf->page_table && jit_conf.fastmem_pointer == conf->fastmem_pointer);
            ASSERT(jit_conf.enable_cycle_counting == conf->enable_cycle_counting && jit_conf.optimizations == conf->optimizations);
        }
        members.push_back(member);
    }

    void Detach(Member member) {
        std::unique_lock lock{mutex};
        std::erase_if(members, [&](const Member& other) { return other.halt_reason == member.halt_reason; });
    }

    /// Marks a Jit as running code; waits out a requested full clear first. The flag is stored
    /// before pending_clear is read and TryClear reads the flags after pending_clear is set (all
    /// sequentially consistent), so either the Jit sees the clear or the clear sees the Jit.
    ///
    /// False if the Jit was asked to halt for another reason while it waited: it is then not running, and its
    /// caller returns that halt. Another Jit can keep the clear waiting for as long as a callback of its guest
    /// code takes, and a pause, an interrupt or the debugger must not wait with it.
    bool Enter(std::atomic<bool>& running, UserConfig::Counters* counters, const volatile u32* halt_reason) {
        for (u32 waits = 0;; ++waits) {
            running.store(true);
            if (!pending_clear.load()) {
                return true;
            }
            running.store(false);
            TryClear(counters);
            if (!pending_clear.load()) {
                continue;
            }
            if ((Atomic::Load(halt_reason) & ~static_cast<u32>(HaltReason::CacheInvalidation)) != 0) {
                return false;
            }
            // The other Jits were halted and leave their code within a block or two; one in a callback can
            // take long, so stop burning the core after a moment.
            if (waits < 64) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds{100});
            }
        }
    }

    void Leave(std::atomic<bool>& running, UserConfig::Counters* counters) {
        running.store(false);
        if (pending_clear.load()) {
            TryClear(counters);
        }
    }

    void RequestClearLocked() {
        pending_clear.store(true);
        HaltAllLocked();
    }

    /// Blocks are gone: Jits must leave the code, which drops their return stack buffers, and
    /// forget their looked up blocks. `entering` is not halted: it is about to run and does so
    /// before it runs.
    void DroppedBlocksLocked(const volatile u32* entering = nullptr) {
        epoch.fetch_add(1);
        HaltAllLocked(entering);
    }

    /// Drops the blocks of invalidated guest code. With one Jit nothing runs the code; with more,
    /// links change at once and the dropped code stays until a full clear, so a Jit still in it
    /// finishes as before the invalidation.
    void ApplyRangesLocked(const volatile u32* entering = nullptr) {
        if (pending_ranges.empty()) {
            return;
        }
        if (address_space->InvalidateCacheRanges(pending_ranges)) {
            DroppedBlocksLocked(entering);
        }
        pending_ranges.clear();
        has_pending_ranges.store(false, std::memory_order_release);
    }

    void HaltAllLocked(const volatile u32* except = nullptr) {
        for (const Member& member : members) {
            if (member.halt_reason != except) {
                Atomic::Or(member.halt_reason, static_cast<u32>(HaltReason::CacheInvalidation));
            }
        }
    }

    void ClearLocked(UserConfig::Counters* counters, bool full) {
        if (counters) {
            ++counters->cache_clears;
            counters->full_clears += full;
        }
        if (JitTrace::Enabled()) {
            JitTrace::Write(JitTrace::Type::Clear, 0, full ? 1 : 0, 0, 0);
        }
        address_space->ClearCache();
        pending_ranges.clear();
        has_pending_ranges.store(false, std::memory_order_release);
        epoch.fetch_add(1);
        clear_is_full = false;
        pending_clear.store(false);
        used_code_bytes.store(0, std::memory_order_relaxed);
    }

    /// Adds to `counters` what all Jits' code lost since any Jit last asked.
    void TakeDroppedStats(UserConfig::Counters& counters) {
        std::shared_lock lock{mutex};
        std::scoped_lock stats_lock{stats_mutex};
        const auto& stats = address_space->GetCompileStats();
        counters.blocks_invalidated += stats.blocks_invalidated - counted.blocks_invalidated;
        counters.invalidated_bytes += stats.invalidated_bytes - counted.invalidated_bytes;
        counters.fastmem_recompiles += stats.fastmem_recompiles - counted.fastmem_recompiles;
        counters.fastmem_stale_faults += stats.fastmem_stale_faults - counted.fastmem_stale_faults;
        counters.fastmem_faults += fault_count.exchange(0, std::memory_order_relaxed);
        counters.fastmem_handler_ns += fault_handler_ns.exchange(0, std::memory_order_relaxed);
        counters.fastmem_lock_wait_ns += fault_lock_wait_ns.exchange(0, std::memory_order_relaxed);
        counted = stats;
    }

    std::shared_mutex mutex;
    std::optional<UserConfig> conf;
    std::optional<A64AddressSpace> address_space;
    std::vector<Member> members;
    /// Bumped when blocks are dropped or cleared.
    std::atomic<u64> epoch{0};
    /// Blocks dropped per lookup table slot: a looked up block is good while its slot's count is the one it was
    /// stored with. Written under `mutex` held exclusively, read without it.
    std::array<std::atomic<u32>, lookup_cache_size> slot_drops{};
    /// All blocks dropped so far: tells a Jit that there are new dropped-block statistics to take.
    std::atomic<u64> drops{0};
    /// Diagnostics: fastmem faults (handled on the exception thread) and Jits translating right now.
    std::atomic<u64> fault_count{0};
    std::atomic<u64> fault_handler_ns{0};
    std::atomic<u64> fault_lock_wait_ns{0};
    std::atomic<u32> translating{0};
    /// The fastmem fault handler given to the address space; Jit::DebugFastmemFault calls it too.
    std::function<Backend::FakeCall(u64)> fastmem_fault;
    /// Bumped by every range invalidation: a block translated across one is translated again.
    std::atomic<u64> invalidations{0};
    std::atomic<bool> pending_clear{false};
    bool clear_is_full = false;  ///< The pending clear is for a full cache
    boost::icl::interval_set<u64> pending_ranges;
    /// Whether pending_ranges has any: read by lookups, which take no lock.
    std::atomic<bool> has_pending_ranges{false};
    /// The last range asked to be invalidated, and how many blocks the cache had emitted then (under `mutex`).
    std::optional<boost::icl::discrete_interval<u64>> last_range;
    u64 last_range_emitted = 0;
    /// Blocks emitted into the cache so far (under `mutex`).
    u64 emitted_blocks = 0;
    std::atomic<std::size_t> used_code_bytes{0};
    u64 counted_veneers = 0;  ///< Under `mutex`, as is the next
    u64 counted_unlinked_far_links = 0;
    std::mutex stats_mutex;
    AddressSpace::CompileStats counted;  ///< Under `stats_mutex`

private:
    void TryClear(UserConfig::Counters* counters) {
        std::unique_lock lock{mutex};
        if (pending_clear.load() && std::none_of(members.begin(), members.end(), [](const Member& member) { return member.running->load(); })) {
            ClearLocked(counters, clear_is_full);
        }
    }
};

/// ISB: discards instructions this core fetched before code memory was rewritten by another core.
/// (Inline assembly as in oaknut's CodeBlock::invalidate.)
inline void InstructionSynchronizationBarrier() {
    __asm__ volatile("isb" ::: "memory");
}

std::shared_ptr<SharedCodeCache> MakeSharedCodeCache() {
    return std::make_shared<SharedCodeCache>();
}

struct Jit::Impl final {
    Impl(Jit*, A64::UserConfig conf_)
            : conf(conf_)
            , cache(conf.shared_code_cache ? conf.shared_code_cache : MakeSharedCodeCache()) {
        cache->Attach({&halt_reason, &running}, conf, &LookupBlockThunk);
        epoch = cache->epoch.load();
        InitJitStatePointers();
    }

    ~Impl() {
        cache->Detach({&halt_reason, &running});
    }

    HaltReason Run() {
        ASSERT(!is_executing);
        const CodePtr entry_point = Enter(current_state.GetLocationDescriptor());
        if (!entry_point) {
            return HaltedWhileWaiting();
        }
        is_executing = true;
        const HaltReason hr = cache->address_space->RunCode(entry_point, current_state, &halt_reason);
        is_executing = false;
        cache->Leave(running, conf.counters);
        UpdateCounters();
        return hr;
    }

    HaltReason Step() {
        ASSERT(!is_executing);
        const CodePtr entry_point = Enter(A64::LocationDescriptor{current_state.GetLocationDescriptor()}.SetSingleStepping(true));
        if (!entry_point) {
            return HaltedWhileWaiting();
        }
        is_executing = true;
        const HaltReason hr = cache->address_space->StepCode(entry_point, current_state, &halt_reason);
        is_executing = false;
        cache->Leave(running, conf.counters);
        UpdateCounters();
        return hr;
    }

    void ClearCache() {
        std::unique_lock lock{cache->mutex};
        cache->RequestClearLocked();
    }

    void* DebugLookupBlock(u64 location_descriptor) {
        const CodePtr entry_point = Enter(IR::LocationDescriptor{location_descriptor});
        if (!entry_point) {
            return nullptr;
        }
        cache->Leave(running, conf.counters);
        UpdateCounters();
        return entry_point;
    }

    bool DebugFastmemFault(u64 location_descriptor, unsigned access_index, void* stale_entry_point) {
        std::optional<u64> host_pc;
        {
            std::shared_lock lock{cache->mutex};
            const CodePtr entry_point = stale_entry_point ? static_cast<CodePtr>(stale_entry_point)
                                                          : cache->address_space->Get(IR::LocationDescriptor{location_descriptor});
            if (entry_point) {
                host_pc = cache->address_space->FastmemPatchPc(entry_point, access_index);
            }
        }
        if (!host_pc) {
            return false;
        }
        cache->fastmem_fault(*host_pc);
        return true;
    }

    void InvalidateCacheRange(std::uint64_t start_address, std::size_t length) {
        std::unique_lock lock{cache->mutex};
        const auto range = boost::icl::discrete_interval<u64>::closed(start_address, start_address + length - 1);
        // A program invalidates a range on every core's Jit. On one cache that is the same request several
        // times over, and each would halt every Jit again. Nothing is left to drop or to halt for if the range
        // still waits to be applied (the Jits are halted and apply it before they run on), or if it was the
        // last one asked for and no block has been emitted since (so none can lie in it). A block being
        // translated right now may still have read the old code, so translations are told in any case.
        const u64 emitted = cache->emitted_blocks;
        if (boost::icl::contains(cache->pending_ranges, range) ||
            (cache->last_range && *cache->last_range == range && cache->last_range_emitted == emitted)) {
            cache->invalidations.fetch_add(1);
            return;
        }
        cache->last_range = range;
        cache->last_range_emitted = emitted;
        cache->pending_ranges.add(range);
        cache->has_pending_ranges.store(true, std::memory_order_release);
        if (JitTrace::Enabled()) {
            JitTrace::Write(JitTrace::Type::Invalidate, trace_id, 0, start_address, length);
        }
        cache->invalidations.fetch_add(1);
        cache->HaltAllLocked();
        if (conf.counters) {
            ++conf.counters->invalidation_requests;
        }
    }

    void Reset() {
        current_state = {};
        InitJitStatePointers();
    }

    void HaltExecution(HaltReason hr) {
        Atomic::Or(&halt_reason, static_cast<u32>(hr));
    }

    void ClearHalt(HaltReason hr) {
        Atomic::And(&halt_reason, ~static_cast<u32>(hr));
    }

    std::uint64_t PC() const {
        return current_state.pc;
    }

    void SetPC(std::uint64_t value) {
        current_state.pc = value;
    }

    std::uint64_t SP() const {
        return current_state.sp;
    }

    void SetSP(std::uint64_t value) {
        current_state.sp = value;
    }

    std::array<std::uint64_t, 31>& Regs() {
        return current_state.reg;
    }

    const std::array<std::uint64_t, 31>& Regs() const {
        return current_state.reg;
    }

    std::array<std::uint64_t, 64>& VecRegs() {
        return current_state.vec;
    }

    const std::array<std::uint64_t, 64>& VecRegs() const {
        return current_state.vec;
    }

    std::uint32_t Fpcr() const {
        return current_state.fpcr;
    }

    void SetFpcr(std::uint32_t value) {
        current_state.fpcr = value;
    }

    std::uint32_t Fpsr() const {
        return current_state.fpsr;
    }

    void SetFpsr(std::uint32_t value) {
        current_state.fpsr = value;
    }

    std::uint32_t Pstate() const {
        return current_state.cpsr_nzcv;
    }

    void SetPstate(std::uint32_t value) {
        current_state.cpsr_nzcv = value;
    }

    void ClearExclusiveState() {
        current_state.exclusive_state = false;
    }

    bool IsExecuting() const {
        return is_executing;
    }

    std::string Disassemble() const {
        return {};
    }

private:
    /// Blocks this Jit looked up, so the dispatcher takes no lock for them.
    struct LookedUpBlock {
        u64 descriptor = ~u64{0};
        CodePtr entry_point = nullptr;
        u32 slot_drops = 0;  ///< The cache's slot_drops of this slot when the block was looked up
    };

    /// What Run and Step return when the Jit was halted while it waited for a cache clear: the halt reasons,
    /// taken as the code's own exit takes them. No guest code ran.
    HaltReason HaltedWhileWaiting() {
        UpdateCounters();
        return static_cast<HaltReason>(Atomic::Exchange(&halt_reason, 0));
    }

    /// Waits out a full clear, applies invalidations and returns the block to enter. The Jit is
    /// marked running from here on. Returns nullptr, with the Jit not running, if it was halted for
    /// another reason while it waited for a clear.
    CodePtr Enter(IR::LocationDescriptor descriptor) {
        while (true) {
            // Cleared first: a clear or invalidation requested after this halts the Jit again.
            ClearHalt(HaltReason::CacheInvalidation);
            if (!cache->Enter(running, conf.counters, &halt_reason)) {
                return nullptr;
            }
            if (cache->invalidations.load() != applied_invalidations) {
                std::unique_lock lock{cache->mutex};
                cache->ApplyRangesLocked(&halt_reason);
                applied_invalidations = cache->invalidations.load();
            }
            const CodePtr entry_point = GetBlock(descriptor);
            if (entry_point != cache->address_space->ReturnFromRunCode()) {
                // Code memory is reused after a full clear, and other cores may have emitted and invalidated
                // new code over it while this core was out of the JIT. Their invalidation is complete (their
                // sys_icache_invalidate ends with DSB) and ordered before this point by the cache's locks and
                // atomics, but this core may still hold instructions fetched from the old contents: a context
                // synchronization event discards them. Entries are per Run or Step, not per block, so it is cheap.
                InstructionSynchronizationBarrier();
                return entry_point;
            }
            // The cache is full and other Jits run its code: wait for the clear.
            cache->Leave(running, conf.counters);
        }
    }

    void InitJitStatePointers() {
        current_state.callbacks = std::bit_cast<u64>(conf.callbacks);
        current_state.user_config = std::bit_cast<u64>(&conf);
        current_state.jit_impl = std::bit_cast<u64>(this);
        current_state.tpidr_el0 = std::bit_cast<u64>(conf.tpidr_el0);
        current_state.tpidrro_el0 = std::bit_cast<u64>(conf.tpidrro_el0);
    }

    static CodePtr LookupBlockThunk(void* jit_impl, A64JitState& jit_state) {
        return static_cast<Jit::Impl*>(jit_impl)->GetBlock(jit_state.GetLocationDescriptor());
    }

    void ForgetLookedUpBlocks() {
        epoch = cache->epoch.load();
        std::fill(lookup_cache.begin(), lookup_cache.end(), LookedUpBlock{});
    }

    CodePtr GetBlock(IR::LocationDescriptor descriptor) {
        const std::size_t slot = LookupCacheIndex(descriptor.Value());
        LookedUpBlock& looked_up = lookup_cache[slot];
        const std::atomic<u32>& slot_drops = cache->slot_drops[slot];
        {
            const u64 now = cache->epoch.load(std::memory_order_acquire);
            if (now != epoch) {
                // Blocks were dropped. A link or return may still lead into one until this Jit
                // leaves the code (it has been halted), as before the invalidation.
                ForgetLookedUpBlocks();
            } else if (looked_up.descriptor == descriptor.Value() && looked_up.slot_drops == slot_drops.load(std::memory_order_acquire)) {
                if (conf.counters) {
                    ++conf.counters->lookup_hits;
                }
                return looked_up.entry_point;
            }
        }
        if (conf.counters) {
            ++conf.counters->block_lookups;
        }
        const auto since = [](std::chrono::steady_clock::time_point from) {
            return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - from).count());
        };
        {
            // No lock: the block map is read lock-free (block_table.h). A block dropped or an invalidation
            // queued right after this lookup is handled as it was when the lookup held the lock and released it
            // just before: this Jit has been halted for an invalidation, and slot_drops, read before the lookup,
            // makes it look a dropped block up again.
            const u32 drops_before = slot_drops.load(std::memory_order_acquire);
            if (!cache->has_pending_ranges.load(std::memory_order_acquire)) {
                if (const CodePtr entry_point = cache->address_space->Get(descriptor)) {
                    if (cache->epoch.load() == epoch) {
                        looked_up = {descriptor.Value(), entry_point, drops_before};
                    }
                    if (JitTrace::Enabled()) {
                        JitTrace::Write(JitTrace::Type::Lookup, trace_id, 0, descriptor.Value(), 0);
                    }
                    return entry_point;
                }
            }
        }

        const auto begin = std::chrono::steady_clock::now();
        while (true) {
            // Translated without the lock; an invalidation meanwhile may have changed the code.
            const u64 invalidations = cache->invalidations.load();
            const auto translate_begin = std::chrono::steady_clock::now();
            const bool has_company = cache->translating.fetch_add(1, std::memory_order_relaxed) != 0;
            ir_block.Reset(descriptor);
            const bool tracing = JitTrace::Enabled();
            traced_code.clear();
            const auto get_code = [this, tracing](u64 vaddr) {
                const std::optional<u32> word = conf.callbacks->MemoryReadCode(vaddr);
                if (tracing && word) {
                    traced_code.emplace_back(vaddr, u64{*word});
                }
                return word;
            };
            A64::Translate(ir_block, A64::LocationDescriptor{descriptor}, get_code, {conf.define_unpredictable_behaviour, conf.wall_clock_cntpct});
            Optimization::Optimize(ir_block, conf, {});
            cache->translating.fetch_sub(1, std::memory_order_relaxed);
            if (conf.counters) {
                conf.counters->compiles_with_company += has_company;
            }

            const auto wait_begin = std::chrono::steady_clock::now();
            std::unique_lock lock{cache->mutex};
            const auto hold_begin = std::chrono::steady_clock::now();
            if (conf.counters) {
                conf.counters->emit_lock_wait_ns += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(hold_begin - wait_begin).count());
            }
            cache->ApplyRangesLocked();
            if (const CodePtr entry_point = cache->address_space->Get(descriptor)) {
                if (tracing) {
                    JitTrace::Write(JitTrace::Type::Lookup, trace_id, 2, descriptor.Value(), 0);
                }
                if (conf.counters) {
                    ++conf.counters->discarded_translations;
                    conf.counters->discarded_translate_ns += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(wait_begin - translate_begin).count());
                }
                return entry_point;
            }
            if (invalidations != cache->invalidations.load()) {
                continue;
            }

            if (cache->pending_clear.load() || !cache->address_space->PrepareSpace()) {
                cache->clear_is_full |= !cache->pending_clear.load();
                if (cache->members.size() > 1) {
                    // Other Jits may run the code: halt all, and clear once none runs.
                    cache->RequestClearLocked();
                    return cache->address_space->ReturnFromRunCode();
                }
                // Nothing else runs the code, and this Jit is in the dispatcher: return stack
                // buffer entries of the old generation no longer match.
                cache->ClearLocked(conf.counters, cache->clear_is_full);
                ForgetLookedUpBlocks();
                ASSERT(cache->address_space->PrepareSpace());
            }

            const CodePtr entry_point = cache->address_space->EmitBlock(std::move(ir_block)).entry_point;
            ++cache->emitted_blocks;
            if (tracing) {
                JitTrace::Write(JitTrace::Type::Compile, trace_id, 0, descriptor.Value(), 0, &traced_code);
                JitTrace::Write(JitTrace::Type::Lookup, trace_id, 1, descriptor.Value(), 0);
            }
            cache->used_code_bytes.store(cache->address_space->GetUsedCodeBytes(), std::memory_order_relaxed);
            if (conf.counters) {
                const auto& stats = cache->address_space->GetCompileStats();
                conf.counters->far_link_veneers += stats.veneers - cache->counted_veneers;
                conf.counters->unlinked_far_links += stats.unlinked_far_links - cache->counted_unlinked_far_links;
                cache->counted_veneers = stats.veneers;
                cache->counted_unlinked_far_links = stats.unlinked_far_links;
            }
            if (cache->epoch.load() == epoch) {
                looked_up = {descriptor.Value(), entry_point, slot_drops.load(std::memory_order_acquire)};
            }
            if (conf.counters) {
                conf.counters->emit_lock_hold_ns += since(hold_begin);
                ++conf.counters->blocks_compiled;
                conf.counters->compile_ns += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
            }
            return entry_point;
        }
    }

    // What all Jits of the cache lost goes to the first Jit that runs after it; JITs on the same
    // host thread may share one Counters.
    void UpdateCounters() {
        if (!conf.counters) {
            return;
        }
        if (const u64 now = cache->drops.load(std::memory_order_acquire); now != counted_drops) {
            cache->TakeDroppedStats(*conf.counters);
            counted_drops = now;
        }
        conf.counters->code_bytes = cache->used_code_bytes.load(std::memory_order_relaxed);
    }

    /// The order the Jits were made in: names this Jit in a DYNARMIC_JIT_TRACE recording.
    const u32 trace_id = [] { static std::atomic<u32> next{0}; return next.fetch_add(1); }();
    A64::UserConfig conf;
    A64JitState current_state{};
    std::shared_ptr<SharedCodeCache> cache;
    IR::Block ir_block{IR::LocationDescriptor{0}};
    std::vector<LookedUpBlock> lookup_cache = std::vector<LookedUpBlock>(lookup_cache_size);
    /// DYNARMIC_JIT_TRACE: the guest instructions the block being translated was read from.
    std::vector<std::pair<u64, u64>> traced_code;
    /// The cache's epoch `lookup_cache` is valid for.
    u64 epoch = 0;
    /// The cache's `drops` whose dropped blocks are counted.
    u64 counted_drops = 0;
    /// The cache's invalidations this Jit has seen applied.
    u64 applied_invalidations = 0;

    volatile u32 halt_reason = 0;

    bool is_executing = false;
    /// Whether this Jit runs code of the cache (SharedCodeCache::Enter).
    std::atomic<bool> running{false};
};

Jit::Jit(UserConfig conf)
        : impl{std::make_unique<Jit::Impl>(this, conf)} {
}

Jit::~Jit() = default;

HaltReason Jit::Run() {
    return impl->Run();
}

HaltReason Jit::Step() {
    return impl->Step();
}

void Jit::ClearCache() {
    impl->ClearCache();
}

void Jit::InvalidateCacheRange(std::uint64_t start_address, std::size_t length) {
    impl->InvalidateCacheRange(start_address, length);
}

void* Jit::DebugLookupBlock(std::uint64_t location_descriptor) {
    return impl->DebugLookupBlock(location_descriptor);
}

bool Jit::DebugFastmemFault(std::uint64_t location_descriptor, unsigned access_index, void* stale_entry_point) {
    return impl->DebugFastmemFault(location_descriptor, access_index, stale_entry_point);
}

void Jit::Reset() {
    impl->Reset();
}

void Jit::HaltExecution(HaltReason hr) {
    impl->HaltExecution(hr);
}

void Jit::ClearHalt(HaltReason hr) {
    impl->ClearHalt(hr);
}

std::uint64_t Jit::GetSP() const {
    return impl->SP();
}

void Jit::SetSP(std::uint64_t value) {
    impl->SetSP(value);
}

std::uint64_t Jit::GetPC() const {
    return impl->PC();
}

void Jit::SetPC(std::uint64_t value) {
    impl->SetPC(value);
}

std::uint64_t Jit::GetRegister(std::size_t index) const {
    return impl->Regs()[index];
}

void Jit::SetRegister(size_t index, std::uint64_t value) {
    impl->Regs()[index] = value;
}

std::array<std::uint64_t, 31> Jit::GetRegisters() const {
    return impl->Regs();
}

void Jit::SetRegisters(const std::array<std::uint64_t, 31>& value) {
    impl->Regs() = value;
}

Vector Jit::GetVector(std::size_t index) const {
    auto& vec = impl->VecRegs();
    return {vec[index * 2], vec[index * 2 + 1]};
}

void Jit::SetVector(std::size_t index, Vector value) {
    auto& vec = impl->VecRegs();
    vec[index * 2] = value[0];
    vec[index * 2 + 1] = value[1];
}

std::array<Vector, 32> Jit::GetVectors() const {
    std::array<Vector, 32> ret;
    std::memcpy(ret.data(), impl->VecRegs().data(), sizeof(ret));
    return ret;
}

void Jit::SetVectors(const std::array<Vector, 32>& value) {
    std::memcpy(impl->VecRegs().data(), value.data(), sizeof(value));
}

std::uint32_t Jit::GetFpcr() const {
    return impl->Fpcr();
}

void Jit::SetFpcr(std::uint32_t value) {
    impl->SetFpcr(value);
}

std::uint32_t Jit::GetFpsr() const {
    return impl->Fpsr();
}

void Jit::SetFpsr(std::uint32_t value) {
    impl->SetFpsr(value);
}

std::uint32_t Jit::GetPstate() const {
    return impl->Pstate();
}

void Jit::SetPstate(std::uint32_t value) {
    impl->SetPstate(value);
}

void Jit::ClearExclusiveState() {
    impl->ClearExclusiveState();
}

bool Jit::IsExecuting() const {
    return impl->IsExecuting();
}

std::string Jit::Disassemble() const {
    return impl->Disassemble();
}

}  // namespace Dynarmic::A64
