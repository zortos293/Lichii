// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "common/assert.h"
#include "common/common_types.h"
#include <oaknut/code_block.hpp>
#include <oaknut/oaknut.hpp>
#include "common/container/unordered_map.h"
#include "common/container/unordered_set.h"

#include "dynarmic/backend/arm64/block_table.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/fastmem.h"
#include "dynarmic/interface/halt_reason.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend::Arm64 {

class AddressSpace {
public:
    /// `regions`: whether the cache may be a row of regions (EmitRegionPrelude), and so larger
    /// than a direct branch reaches.
    AddressSpace(std::size_t code_cache_size, std::optional<std::uintptr_t> fastmem_arena, bool regions = false);
    virtual ~AddressSpace();

    virtual void GenerateIR(IR::Block& ir_block, IR::LocationDescriptor) const = 0;

    CodePtr Get(IR::LocationDescriptor descriptor);

    // Returns "most likely" LocationDescriptor assocated with the emitted code at that location
    std::optional<IR::LocationDescriptor> ReverseGetLocation(CodePtr host_pc);

    // Returns "most likely" entry_point associated with the emitted code at that location
    CodePtr ReverseGetEntryPoint(CodePtr host_pc);
    /// Where in a block's code the memory access with the given marker index is done as fastmem, if it is.
    /// The caller holds the cache's lock.
    std::optional<u64> FastmemPatchPc(CodePtr entry_point, unsigned access_index) const;

    CodePtr GetOrEmit(IR::LocationDescriptor descriptor);

    void InvalidateBasicBlocks(const ::Common::unordered_set<IR::LocationDescriptor>& descriptors);

    virtual void ClearCache();

    /// A direct branch reaches 128 MiB. A larger cache is a row of regions of this size, each
    /// with its own prelude at its start; a link to a block out of reach goes through a veneer
    /// at the end of the linking block's region.
    static constexpr std::size_t MaxRegionSize = 128 * 1024 * 1024;

    /// Totals since creation; the JIT adds their growth to UserConfig::counters.
    struct CompileStats {
        u64 blocks_compiled = 0;
        u64 compile_ns = 0;
        u64 cache_clears = 0;
        u64 full_clears = 0;
        u64 blocks_invalidated = 0;
        u64 invalidated_bytes = 0;
        u64 fastmem_recompiles = 0;
        u64 fastmem_stale_faults = 0;  ///< Fastmem faults in code that was already replaced
        u64 veneers = 0;  ///< Veneers made for links out of a direct branch's reach
        u64 unlinked_far_links = 0;  ///< Such links left to the dispatcher: no room for a veneer
    };
    /// Called for each block InvalidateBasicBlocks drops (its code stays until the cache is cleared).
    void SetBlockDroppedCallback(std::function<void(IR::LocationDescriptor)> callback) {
        block_dropped = std::move(callback);
    }
    const CompileStats& GetCompileStats() const {
        return compile_stats;
    }
    std::size_t GetUsedCodeBytes() const {
        return static_cast<std::size_t>(code.offset() - prelude_info.end_of_prelude);
    }
protected:
    virtual EmitConfig GetEmitConfig() = 0;
    virtual void RegisterNewBasicBlock(const IR::Block& block, const EmittedBlockInfo& block_info) = 0;

    void ProtectCodeMemory() {
#if defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT) || defined(__APPLE__) || defined(__OpenBSD__)
        mem.protect();
#endif
    }

    void UnprotectCodeMemory() {
#if defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT) || defined(__APPLE__) || defined(__OpenBSD__)
        mem.unprotect();
#endif
    }

    struct PreludeInfo;

    /// Makes room for a block, moving to the next region when this one is full. False when the
    /// whole cache is full: the caller clears it.
    bool PrepareSpace();
    /// Emits the prelude of a region after the first at the code pointer.
    virtual void EmitRegionPrelude(PreludeInfo&) {
        UNREACHABLE();
    }
    EmittedBlockInfo Emit(IR::Block ir_block);
    void Link(EmittedBlockInfo& block);
    /// `live`: other threads may run the block, and its instruction cache is up to date.
    void LinkBlockLinks(const CodePtr entry_point, IR::LocationDescriptor target_descriptor, const CodePtr target_ptr, const std::vector<BlockRelocation>& block_relocations_list, bool live);
    void RelinkForDescriptor(IR::LocationDescriptor target_descriptor, CodePtr target_ptr);

    std::size_t RegionOf(CodePtr ptr) const;
    const PreludeInfo& PreludeOf(CodePtr ptr) const;
    /// The veneer in `region` that jumps to `target_ptr`, or nullptr when the region has no room.
    CodePtr GetVeneer(std::size_t region, IR::LocationDescriptor target_descriptor, CodePtr target_ptr);

    FakeCall FastmemCallback(u64 host_pc);

    IR::Block ir_block;
    const std::size_t code_cache_size;
    /// MaxRegionSize, or less for tests (DYNARMIC_ARM64_REGION_SIZE): links then count as out of
    /// reach beyond it too.
    const std::size_t region_size;
    u64 cache_generation = 0;
    oaknut::CodeBlock mem;
    oaknut::CodeGenerator code;

    // A IR::LocationDescriptor will have one current CodePtr.
    // However, there can be multiple other CodePtrs which are older, previously invalidated blocks.
    std::map<CodePtr, IR::LocationDescriptor> reverse_block_entries;
    BlockTable block_entries;  ///< Find needs no lock; see block_table.h
    std::function<void(IR::LocationDescriptor)> block_dropped;
    ::Common::unordered_map<CodePtr, EmittedBlockInfo> block_infos;
    ::Common::unordered_map<IR::LocationDescriptor, ::Common::unordered_set<CodePtr>> block_references;

    ExceptionHandler exception_handler;
    FastmemManager fastmem_manager;

    CompileStats compile_stats;

    struct PreludeInfo {
        std::ptrdiff_t end_of_prelude;

        using RunCodeFuncType = HaltReason (*)(CodePtr entry_point, void* jit_state, volatile u32* halt_reason);
        RunCodeFuncType run_code;
        RunCodeFuncType step_code;
        void* return_to_dispatcher;
        void* return_from_run_code;

        void* read_memory_8;
        void* read_memory_16;
        void* read_memory_32;
        void* read_memory_64;
        void* read_memory_128;
        void* wrapped_read_memory_8;
        void* wrapped_read_memory_16;
        void* wrapped_read_memory_32;
        void* wrapped_read_memory_64;
        void* wrapped_read_memory_128;
        void* exclusive_read_memory_8;
        void* exclusive_read_memory_16;
        void* exclusive_read_memory_32;
        void* exclusive_read_memory_64;
        void* exclusive_read_memory_128;
        void* write_memory_8;
        void* write_memory_16;
        void* write_memory_32;
        void* write_memory_64;
        void* write_memory_128;
        void* wrapped_write_memory_8;
        void* wrapped_write_memory_16;
        void* wrapped_write_memory_32;
        void* wrapped_write_memory_64;
        void* wrapped_write_memory_128;
        void* exclusive_write_memory_8;
        void* exclusive_write_memory_16;
        void* exclusive_write_memory_32;
        void* exclusive_write_memory_64;
        void* exclusive_write_memory_128;

        void* call_svc;
        void* exception_raised;
        void* dc_raised;
        void* ic_raised;
        void* isb_raised;

        void* get_cntpct;
        void* add_ticks;
        void* get_ticks_remaining;
    } prelude_info;

    struct Region {
        PreludeInfo prelude;  ///< Regions after the first; the first has prelude_info
        bool has_prelude = false;
        std::ptrdiff_t begin = 0;
        std::ptrdiff_t end = 0;
        std::ptrdiff_t code_end = 0;  ///< Where its blocks end, once emission has moved on
        std::ptrdiff_t veneer_begin = 0;  ///< Veneers grow down from `end`
        ::Common::unordered_map<IR::LocationDescriptor, CodePtr> veneers;
    };
    std::vector<Region> regions;
    std::size_t current_region = 0;
};

}  // namespace Dynarmic::Backend::Arm64
