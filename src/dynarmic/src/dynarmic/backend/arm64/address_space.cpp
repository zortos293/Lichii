// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <cstdio>

#include "dynarmic/backend/arm64/jit_trace.h"
#include <cstdlib>

#include <algorithm>
#include <bit>
#include <chrono>
#include <mutex>

#include "dynarmic/backend/arm64/a64_address_space.h"
#include "dynarmic/backend/arm64/a64_jitstate.h"
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/devirtualize.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/stack_layout.h"
#include "dynarmic/common/cast_util.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/llvm_disassemble.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend::Arm64 {

FastmemManager::FastmemManager(ExceptionHandler& eh, std::optional<std::uintptr_t> arena)
    : exception_handler(eh)
{
    if (!arena) {
        records = std::make_shared<Records>();
        return;
    }
    static std::mutex registry_mutex;
    static ::Common::unordered_map<std::uintptr_t, std::weak_ptr<Records>> registry;
    std::scoped_lock lock{registry_mutex};
    auto& entry = registry[*arena];
    records = entry.lock();
    if (!records) {
        records = std::make_shared<Records>();
        entry = records;
    }
}

namespace {

std::size_t RegionSizeSetting() {
    const char* const value = std::getenv("DYNARMIC_ARM64_REGION_SIZE");
    const unsigned long long bytes = value ? std::strtoull(value, nullptr, 0) : 0;
    // At least a prelude and the megabyte kept free for the next block.
    return bytes >= 2 * 1024 * 1024 && bytes < AddressSpace::MaxRegionSize ? static_cast<std::size_t>(bytes) & ~std::size_t{0xFFFF} : AddressSpace::MaxRegionSize;
}

}  // namespace

AddressSpace::AddressSpace(std::size_t code_cache_size, std::optional<std::uintptr_t> fastmem_arena, bool use_regions)
    : ir_block{IR::LocationDescriptor{0}}
    , code_cache_size(code_cache_size)
    , region_size(RegionSizeSetting())
    , mem(code_cache_size)
    , code(mem.ptr(), mem.ptr())
    , fastmem_manager(exception_handler, fastmem_arena)
{
    // Whole regions only, but for a cache smaller than one.
    ASSERT((use_regions || code_cache_size <= MaxRegionSize) && "code_cache_size > 128 MiB not supported");
    const std::size_t region_count = use_regions ? std::max<std::size_t>(1, code_cache_size / region_size) : 1;
    regions.resize(region_count);
    for (std::size_t i = 0; i < region_count; ++i) {
        Region& region = regions[i];
        region.begin = static_cast<std::ptrdiff_t>(i * region_size);
        region.end = static_cast<std::ptrdiff_t>(region_count == 1 ? code_cache_size : (i + 1) * region_size);
        region.code_end = region.begin;
        region.veneer_begin = region.end;
    }

    exception_handler.Register(mem, code_cache_size);
    exception_handler.SetFastmemCallback([this](u64 host_pc) {
        return FastmemCallback(host_pc);
    });
}

AddressSpace::~AddressSpace() = default;

CodePtr AddressSpace::Get(IR::LocationDescriptor descriptor) {
    // Safe on any thread without the cache's lock.
    return block_entries.Find(descriptor.Value());
}

std::optional<IR::LocationDescriptor> AddressSpace::ReverseGetLocation(CodePtr host_pc) {
    if (auto iter = reverse_block_entries.upper_bound(host_pc); iter != reverse_block_entries.begin()) {
        // upper_bound locates the first value greater than host_pc, so we need to decrement
        --iter;
        return iter->second;
    }
    return std::nullopt;
}

CodePtr AddressSpace::ReverseGetEntryPoint(CodePtr host_pc) {
    if (auto iter = reverse_block_entries.upper_bound(host_pc); iter != reverse_block_entries.begin()) {
        // upper_bound locates the first value greater than host_pc, so we need to decrement
        --iter;
        return iter->first;
    }
    return nullptr;
}

CodePtr AddressSpace::GetOrEmit(IR::LocationDescriptor descriptor) {
    if (CodePtr block_entry = Get(descriptor)) {
        return block_entry;
    }
    const auto begin = std::chrono::steady_clock::now();
    GenerateIR(ir_block, descriptor);
    const EmittedBlockInfo block_info = Emit(std::move(ir_block));
    ++compile_stats.blocks_compiled;
    compile_stats.compile_ns += static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin)
            .count());
    return block_info.entry_point;
}

std::optional<u64> AddressSpace::FastmemPatchPc(CodePtr entry_point, unsigned access_index) const {
    const auto block_info = block_infos.find(entry_point);
    if (block_info == block_infos.end()) {
        return std::nullopt;
    }
    for (const auto& [offset, patch] : block_info->second.fastmem_patch_info) {
        if (std::get<1>(patch.marker) == access_index) {
            return std::bit_cast<u64>(entry_point + offset);
        }
    }
    return std::nullopt;
}

void AddressSpace::InvalidateBasicBlocks(const ::Common::unordered_set<IR::LocationDescriptor>& descriptors) {
    UnprotectCodeMemory();

    for (const auto& descriptor : descriptors) {
        const CodePtr entry_point = block_entries.Find(descriptor.Value());
        if (entry_point == nullptr) {
            continue;
        }

        // Unlink before removal because InvalidateBasicBlocks can be called within a fastmem callback,
        // and the currently executing block may have references to itself which need to be unlinked.
        RelinkForDescriptor(descriptor, nullptr);

        ++compile_stats.blocks_invalidated;
        if (const auto info = block_infos.find(entry_point); info != block_infos.end()) {
            compile_stats.invalidated_bytes += info->second.size;
        }
        block_entries.Erase(descriptor.Value());
        if (block_dropped) {
            block_dropped(descriptor);
        }
    }

    ProtectCodeMemory();
}

void AddressSpace::ClearCache() {
    ++compile_stats.cache_clears;
    ++cache_generation;
    block_entries.Clear();
    reverse_block_entries.clear();
    block_infos.clear();
    block_references.clear();
    for (Region& region : regions) {
        region.code_end = region.begin;
        region.veneer_begin = region.end;
        region.veneers.clear();
    }
    current_region = 0;
    code.set_offset(prelude_info.end_of_prelude);
}

bool AddressSpace::PrepareSpace() {
    constexpr std::ptrdiff_t minimum_remaining = 1024 * 1024;
    while (regions[current_region].veneer_begin - static_cast<std::ptrdiff_t>(code.offset()) < minimum_remaining) {
        if (current_region + 1 == regions.size()) {
            return false;
        }
        regions[current_region].code_end = static_cast<std::ptrdiff_t>(code.offset());
        Region& region = regions[++current_region];
        if (region.has_prelude) {
            code.set_offset(region.prelude.end_of_prelude);
            continue;
        }
        code.set_offset(region.begin);
        EmitRegionPrelude(region.prelude);
        region.has_prelude = true;
    }
    return true;
}

std::size_t AddressSpace::RegionOf(CodePtr ptr) const {
    const auto offset = static_cast<std::size_t>(ptr - std::bit_cast<CodePtr>(mem.ptr()));
    return std::min(offset / region_size, regions.size() - 1);
}

const AddressSpace::PreludeInfo& AddressSpace::PreludeOf(CodePtr ptr) const {
    const std::size_t region = RegionOf(ptr);
    return region == 0 ? prelude_info : regions[region].prelude;
}

namespace {

// Other threads may run the code that links are written into. An aligned word is written at
// once, and a branch or NOP may be replaced by another while it is executed (Arm ARM B2.2.5).
void StoreInstruction(CodePtr at, u32 instruction) {
#ifdef _MSC_VER
    *reinterpret_cast<volatile u32*>(at) = instruction;
#else
    __atomic_store_n(reinterpret_cast<u32*>(at), instruction, __ATOMIC_RELAXED);
#endif
}

u32 LoadInstruction(CodePtr at) {
#ifdef _MSC_VER
    return *reinterpret_cast<volatile u32*>(at);
#else
    return __atomic_load_n(reinterpret_cast<u32*>(at), __ATOMIC_RELAXED);
#endif
}

void StorePointer(CodePtr at, u64 value) {
    ASSERT(std::bit_cast<u64>(at) % sizeof(u64) == 0);
#ifdef _MSC_VER
    *reinterpret_cast<volatile u64*>(at) = value;
#else
    __atomic_store_n(reinterpret_cast<u64*>(at), value, __ATOMIC_RELAXED);
#endif
}

constexpr u32 nop_instruction = 0xD503201F;

bool BranchReaches(CodePtr from, CodePtr to, std::size_t reach) {
    const std::ptrdiff_t distance = to - from;
    return distance >= -static_cast<std::ptrdiff_t>(reach) && distance < static_cast<std::ptrdiff_t>(reach);
}

u32 BranchInstruction(CodePtr from, CodePtr to) {
    return 0x14000000 | (static_cast<u32>((to - from) >> 2) & 0x03FFFFFF);
}

}  // namespace

CodePtr AddressSpace::GetVeneer(std::size_t region_index, IR::LocationDescriptor target_descriptor, CodePtr target_ptr) {
    Region& region = regions[region_index];
    const CodePtr base = std::bit_cast<CodePtr>(mem.ptr());
    constexpr std::ptrdiff_t veneer_size = 16;

    if (const auto iter = region.veneers.find(target_descriptor); iter != region.veneers.end()) {
        StorePointer(iter->second + 8, std::bit_cast<u64>(target_ptr));
        return iter->second;
    }

    const std::ptrdiff_t code_end = region_index == current_region ? static_cast<std::ptrdiff_t>(code.offset()) : region.code_end;
    if (region.veneer_begin - veneer_size < code_end) {
        ++compile_stats.unlinked_far_links;
        return nullptr;
    }
    region.veneer_begin -= veneer_size;
    const CodePtr veneer = base + region.veneer_begin;
    StoreInstruction(veneer, 0x58000050);  // LDR X16, #8
    StoreInstruction(veneer + 4, 0xD61F0200);  // BR X16
    StorePointer(veneer + 8, std::bit_cast<u64>(target_ptr));
    mem.invalidate(reinterpret_cast<u32*>(veneer), 8);
    region.veneers.emplace(target_descriptor, veneer);
    ++compile_stats.veneers;
    return veneer;
}

EmittedBlockInfo AddressSpace::Emit(IR::Block block) {
    if (!PrepareSpace()) {
        ++compile_stats.full_clears;
        ClearCache();
        ASSERT(PrepareSpace());
    }

    UnprotectCodeMemory();

    auto emit_config = GetEmitConfig();
    emit_config.cache_generation = cache_generation;
    EmittedBlockInfo block_info = EmitArm64(code, std::move(block), emit_config, fastmem_manager);

    ASSERT(block_entries.Find(block.Location().Value()) == nullptr);
    ASSERT(reverse_block_entries.insert({block_info.entry_point, block.Location()}).second);
    ASSERT(block_infos.insert({block_info.entry_point, block_info}).second);

    Link(block_info);
    // Before other threads can reach the block: they may take a link to it, or find it in the block table
    // (which they read without the cache's lock), at once. Its own links to itself are made by the relink below.
    mem.invalidate(reinterpret_cast<u32*>(block_info.entry_point), block_info.size);
    ASSERT(block_entries.Insert(block.Location().Value(), block_info.entry_point));
    RelinkForDescriptor(block.Location(), block_info.entry_point);
    ProtectCodeMemory();

    RegisterNewBasicBlock(block, block_info);

    return block_info;
}

void AddressSpace::Link(EmittedBlockInfo& block_info) {
    using namespace oaknut;
    using namespace oaknut::util;

    const PreludeInfo& prelude = PreludeOf(block_info.entry_point);

    for (auto [ptr_offset, target] : block_info.relocations) {
        CodeGenerator c{mem.ptr(), mem.ptr()};
        c.set_xptr(reinterpret_cast<u32*>(block_info.entry_point + ptr_offset));

        switch (target) {
        case LinkTarget::ReturnToDispatcher:
            c.B(prelude.return_to_dispatcher);
            break;
        case LinkTarget::ReturnFromRunCode:
            c.B(prelude.return_from_run_code);
            break;
        // { this, vaddr, size }
        case LinkTarget::ReadMemory8:
            c.BL(prelude.read_memory_8);
            break;
        case LinkTarget::ReadMemory16:
            c.BL(prelude.read_memory_16);
            break;
        case LinkTarget::ReadMemory32:
            c.BL(prelude.read_memory_32);
            break;
        case LinkTarget::ReadMemory64:
            c.BL(prelude.read_memory_64);
            break;
        case LinkTarget::ReadMemory128:
            c.BL(prelude.read_memory_128);
            break;
        // { this, vaddr, size }
        case LinkTarget::WrappedReadMemory8:
            c.BL(prelude.wrapped_read_memory_8);
            break;
        case LinkTarget::WrappedReadMemory16:
            c.BL(prelude.wrapped_read_memory_16);
            break;
        case LinkTarget::WrappedReadMemory32:
            c.BL(prelude.wrapped_read_memory_32);
            break;
        case LinkTarget::WrappedReadMemory64:
            c.BL(prelude.wrapped_read_memory_64);
            break;
        case LinkTarget::WrappedReadMemory128:
            c.BL(prelude.wrapped_read_memory_128);
            break;
        //
        case LinkTarget::ExclusiveReadMemory8:
            c.BL(prelude.exclusive_read_memory_8);
            break;
        case LinkTarget::ExclusiveReadMemory16:
            c.BL(prelude.exclusive_read_memory_16);
            break;
        case LinkTarget::ExclusiveReadMemory32:
            c.BL(prelude.exclusive_read_memory_32);
            break;
        case LinkTarget::ExclusiveReadMemory64:
            c.BL(prelude.exclusive_read_memory_64);
            break;
        case LinkTarget::ExclusiveReadMemory128:
            c.BL(prelude.exclusive_read_memory_128);
            break;
        // { this, vaddr, value, size }
        case LinkTarget::WriteMemory8:
            c.BL(prelude.write_memory_8);
            break;
        case LinkTarget::WriteMemory16:
            c.BL(prelude.write_memory_16);
            break;
        case LinkTarget::WriteMemory32:
            c.BL(prelude.write_memory_32);
            break;
        case LinkTarget::WriteMemory64:
            c.BL(prelude.write_memory_64);
            break;
        case LinkTarget::WriteMemory128:
            c.BL(prelude.write_memory_128);
            break;
        //
        case LinkTarget::WrappedWriteMemory8:
            c.BL(prelude.wrapped_write_memory_8);
            break;
        case LinkTarget::WrappedWriteMemory16:
            c.BL(prelude.wrapped_write_memory_16);
            break;
        case LinkTarget::WrappedWriteMemory32:
            c.BL(prelude.wrapped_write_memory_32);
            break;
        case LinkTarget::WrappedWriteMemory64:
            c.BL(prelude.wrapped_write_memory_64);
            break;
        case LinkTarget::WrappedWriteMemory128:
            c.BL(prelude.wrapped_write_memory_128);
            break;
        //
        case LinkTarget::ExclusiveWriteMemory8:
            c.BL(prelude.exclusive_write_memory_8);
            break;
        case LinkTarget::ExclusiveWriteMemory16:
            c.BL(prelude.exclusive_write_memory_16);
            break;
        case LinkTarget::ExclusiveWriteMemory32:
            c.BL(prelude.exclusive_write_memory_32);
            break;
        case LinkTarget::ExclusiveWriteMemory64:
            c.BL(prelude.exclusive_write_memory_64);
            break;
        case LinkTarget::ExclusiveWriteMemory128:
            c.BL(prelude.exclusive_write_memory_128);
            break;
        case LinkTarget::CallSVC:
            c.BL(prelude.call_svc);
            break;
        case LinkTarget::ExceptionRaised:
            c.BL(prelude.exception_raised);
            break;
        case LinkTarget::InstructionSynchronizationBarrierRaised:
            c.BL(prelude.isb_raised);
            break;
        case LinkTarget::InstructionCacheOperationRaised:
            c.BL(prelude.ic_raised);
            break;
        case LinkTarget::DataCacheOperationRaised:
            c.BL(prelude.dc_raised);
            break;
        case LinkTarget::GetCNTPCT:
            c.BL(prelude.get_cntpct);
            break;
        case LinkTarget::AddTicks:
            c.BL(prelude.add_ticks);
            break;
        case LinkTarget::GetTicksRemaining:
            c.BL(prelude.get_ticks_remaining);
            break;
        default:
            UNREACHABLE();
        }
    }

    for (auto [target_descriptor, list] : block_info.block_relocations) {
        block_references[target_descriptor].insert(block_info.entry_point);
        LinkBlockLinks(block_info.entry_point, target_descriptor, Get(target_descriptor), list, false);
    }
}

void AddressSpace::LinkBlockLinks(const CodePtr entry_point, IR::LocationDescriptor target_descriptor, const CodePtr target_ptr, const std::vector<BlockRelocation>& block_relocations_list, bool live) {
    for (auto [ptr_offset, type] : block_relocations_list) {
        const CodePtr at = entry_point + ptr_offset;

        switch (type) {
        case BlockRelocationType::Branch: {
            u32 instruction = nop_instruction;
            if (target_ptr) {
                if (BranchReaches(at, target_ptr, region_size)) {
                    instruction = BranchInstruction(at, target_ptr);
                } else if (const CodePtr veneer = GetVeneer(RegionOf(entry_point), target_descriptor, target_ptr)) {
                    instruction = BranchInstruction(at, veneer);
                }
            }
            if (!live) {
                // A new block: its code is invalidated as a whole.
                StoreInstruction(at, instruction);
            } else if (LoadInstruction(at) != instruction) {
                StoreInstruction(at, instruction);
                mem.invalidate(reinterpret_cast<u32*>(at), sizeof(u32));
            }
            break;
        }
        case BlockRelocationType::MoveToScratch1:
            // The literal the block loads into Xscratch1.
            StorePointer(at, std::bit_cast<u64>(target_ptr ? static_cast<void*>(target_ptr) : PreludeOf(entry_point).return_to_dispatcher));
            break;
        default:
            UNREACHABLE();
        }
    }
}

void AddressSpace::RelinkForDescriptor(IR::LocationDescriptor target_descriptor, CodePtr target_ptr) {
    for (auto code_ptr : block_references[target_descriptor]) {
        if (auto block_iter = block_infos.find(code_ptr); block_iter != block_infos.end()) {
            const EmittedBlockInfo& block_info = block_iter->second;

            if (auto relocation_iter = block_info.block_relocations.find(target_descriptor); relocation_iter != block_info.block_relocations.end()) {
                LinkBlockLinks(block_info.entry_point, target_descriptor, target_ptr, relocation_iter->second, true);
            }
        }
    }
}

FakeCall AddressSpace::FastmemCallback(u64 host_pc) {
    {
        const auto host_ptr = std::bit_cast<CodePtr>(host_pc);

        const auto entry_point = ReverseGetEntryPoint(host_ptr);
        if (!entry_point) {
            goto fail;
        }

        const auto block_info = block_infos.find(entry_point);
        if (block_info == block_infos.end()) {
            goto fail;
        }

        const auto patch_entry = block_info->second.fastmem_patch_info.find(host_ptr - entry_point);
        if (patch_entry == block_info->second.fastmem_patch_info.end()) {
            goto fail;
        }

        const auto fc = patch_entry->second.fc;

        if (patch_entry->second.recompile) {
            const auto marker = patch_entry->second.marker;
            const auto descriptor = std::get<0>(marker);
            // Dropped code stays until the cache is cleared: the faulting block finishes its run, and with a
            // shared cache another Jit can still be in it or return into it. A fault there takes the slow path
            // like any other. The access is marked in any case, so that one pass through a block marks all of
            // its faulting accesses. The block that now stands for the descriptor is dropped only if it still
            // has this access as fastmem: it is the faulting block itself, or it was emitted before the mark.
            const CodePtr current = block_entries.Find(descriptor.Value());
            const bool is_current = current == entry_point;
            const bool was_marked = !fastmem_manager.ShouldFastmem(marker);
            fastmem_manager.MarkDoNotFastmem(marker);
            const bool drops = is_current || (!was_marked && current != nullptr);
            if (JitTrace::Enabled()) {
                JitTrace::Write(JitTrace::Type::Fault, 0, static_cast<u16>((is_current ? 0 : 1) | (drops ? 2 : 0)),
                                descriptor.Value(), std::get<1>(marker));
            }
            if (drops) {
                ++compile_stats.fastmem_recompiles;
                InvalidateBasicBlocks({descriptor});
            } else {
                ++compile_stats.fastmem_stale_faults;
            }
        }

        return fc;
    }

fail:
    fmt::print("dynarmic: Segfault happened within JITted code at host_pc = {:016x}\n"
        "Segfault wasn't at a fastmem patch location!\n", host_pc);
    UNREACHABLE();
}

}  // namespace Dynarmic::Backend::Arm64
