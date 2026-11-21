// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dynarmic/backend/x64/code_image.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <type_traits>

#ifdef _WIN32
#    include <windows.h>
#else
#    include <cerrno>

#    include <dlfcn.h>
#    include <link.h>
#    include <sys/mman.h>
#    include <unistd.h>
#endif

#include <boost/icl/interval_set.hpp>

#include "dynarmic/backend/x64/a64_emit_x64.h"
#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/common/hash.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::X64 {

namespace Image {

size_t PatchSiteSize(EmitX64::PatchKind kind, bool compact_block_exits) {
    // The sizes A64EmitX64::EmitPatch* pad their sites to.
    switch (kind) {
    case EmitX64::PatchKind::Jg:
    case EmitX64::PatchKind::Jz:
        return compact_block_exits ? 6 : 23;
    case EmitX64::PatchKind::Jmp:
        return compact_block_exits ? 5 : 22;
    case EmitX64::PatchKind::MovRcx:
        return 10;
    }
    return 0;
}

namespace {

/// Anchor for finding the module that holds this code.
void ModuleAnchor() {}

FileHash HashFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::vector<char> chunk(size_t(1) << 20);
    Common::Hash128 hash;
    while (file) {
        file.read(chunk.data(), std::streamsize(chunk.size()));
        const auto got = static_cast<size_t>(file.gcount());
        if (got == 0) {
            break;
        }
        hash.Update({reinterpret_cast<const u8*>(chunk.data()), got});
    }
    return hash.Finish();
}

#ifdef _WIN32
HostModule FindHostModule() {
    HostModule module;
    HMODULE handle = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleAnchor), &handle)) {
        return module;
    }
    const auto* const base = reinterpret_cast<const u8*>(handle);
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(handle, path.data(), DWORD(path.size()));
        if (length == 0) {
            return module;
        }
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    module.file_hash = HashFile(std::filesystem::path{path});
    if (module.file_hash == FileHash{}) {
        return module;
    }
    module.base = base;
    module.size = nt->OptionalHeader.SizeOfImage;
    return module;
}
#else
HostModule FindHostModule() {
    struct Search {
        const void* anchor;
        HostModule module;
        std::string path;
    } search{reinterpret_cast<const void*>(&ModuleAnchor), {}, {}};
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) -> int {
            auto& s = *static_cast<Search*>(data);
            uintptr_t low = std::numeric_limits<uintptr_t>::max();
            uintptr_t high = 0;
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const auto& ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_LOAD) {
                    continue;
                }
                low = std::min<uintptr_t>(low, info->dlpi_addr + ph.p_vaddr);
                high = std::max<uintptr_t>(high, info->dlpi_addr + ph.p_vaddr + ph.p_memsz);
            }
            const auto anchor = reinterpret_cast<uintptr_t>(s.anchor);
            if (anchor < low || anchor >= high) {
                return 0;
            }
            s.module.base = reinterpret_cast<const u8*>(low);
            s.module.size = high - low;
            s.path = info->dlpi_name && info->dlpi_name[0] ? info->dlpi_name : "/proc/self/exe";
            return 1;
        },
        &search);
    if (search.module.base) {
        search.module.file_hash = HashFile(std::filesystem::path{search.path});
        if (search.module.file_hash == FileHash{}) {
            search.module = {};
        }
    }
    return search.module;
}
#endif

}  // namespace

const HostModule& GetHostModule() {
    static const HostModule module = FindHostModule();
    return module;
}

bool IsMappedHostAddress(u64 address) {
    // Below the first 64 KiB nothing is mapped on either OS, and above the canonical user range
    // a value is not a user-space address.
    if (address < 0x10000 || address >= (u64(1) << 47)) {
        return false;
    }
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0) {
        return false;
    }
    return info.State != MEM_FREE;
#else
    static const u64 page_size = u64(sysconf(_SC_PAGESIZE));
    unsigned char resident = 0;
    void* const page = reinterpret_cast<void*>(address & ~(page_size - 1));
    return mincore(page, 1, &resident) == 0 || errno != ENOMEM;
#endif
}

u64 CodegenHash(const A64::UserConfig& conf) {
    std::vector<u8> bytes;
    const auto put = [&bytes](auto value) {
        static_assert(std::is_trivially_copyable_v<decltype(value)>);
        const auto* const p = reinterpret_cast<const u8*>(&value);
        bytes.insert(bytes.end(), p, p + sizeof(value));
    };
    // Every field of A64::UserConfig except code_image/code_image_verify, in declaration order.
    // A new UserConfig field that changes generated code must be added here.
    put(conf.fastmem_pointer.has_value());
    put(conf.callbacks != nullptr);
    put(conf.global_monitor != nullptr);
    put(conf.tpidrro_el0 != nullptr);
    put(conf.tpidr_el0 != nullptr);
    put(conf.page_table != nullptr);
    put(conf.optimizations);
    put(conf.page_table_address_space_bits);
    put(conf.page_table_pointer_mask);
    put(conf.page_table_log2_stride);
    put(conf.page_table_marked_bit.has_value());
    put(conf.page_table_marked_bit.value_or(0));
    put(conf.page_table_sign_extension.has_value());
    put(conf.page_table_sign_extension.value_or(0));
    put(conf.cntfrq_el0);
    put(conf.ctr_el0);
    put(conf.dczid_el0);
    put(conf.fastmem_address_space_bits);
    put(conf.code_cache_size);
    put(conf.detect_misaligned_access_via_page_table);
    put(conf.unsafe_optimizations);
    put(conf.hook_data_cache_operations);
    put(conf.hook_isb);
    put(conf.hook_hint_instructions);
    put(conf.silently_mirror_page_table);
    put(conf.absolute_offset_page_table);
    put(conf.only_detect_misalignment_via_page_table_on_page_boundary);
    put(conf.recompile_on_fastmem_failure);
    put(conf.silently_mirror_fastmem);
    put(conf.fastmem_exclusive_access);
    put(conf.recompile_on_exclusive_fastmem_failure);
    put(conf.spill_to_free_gpr);
    put(conf.scratch_copy_to_free_reg);
    put(conf.fold_address_offsets);
    put(conf.elide_redundant_zext);
    put(conf.omit_block_frame);
    put(conf.halt_check_on_back_edges);
    put(conf.rsb_size);
    put(conf.compact_block_exits);
    put(conf.far_code);
    put(conf.fast_dispatch_table_bits);
    put(conf.fast_dispatch_set_associative);
    put(conf.reuse_host_flags);
    put(conf.host_call_return);
    put(conf.core_independent_code);
    put(conf.define_unpredictable_behaviour);
    put(conf.wall_clock_cntpct);
    put(conf.inline_cntpct_factor != nullptr);
    put(conf.inline_cntpct_mode != nullptr);
    put(conf.inline_cntpct_last != nullptr);
    put(conf.inline_cntpct_spin_ticks);
    put(conf.check_halt_on_memory_access);
    put(conf.enable_cycle_counting);
    put(conf.very_verbose_debugging_output);
    return Common::Hash128::Of(bytes)[0];
}

namespace {

constexpr std::array<char, 8> magic{'D', 'Y', 'N', 'C', 'I', 'M', 'G', '\n'};
constexpr std::array<char, 8> chunk_magic{'D', 'Y', 'N', 'C', 'B', 'L', 'K', '\n'};
/// Bump on any change of the serialised form or of what a record means.
constexpr u32 format_version = 2;

class Writer {
public:
    template<typename T>
    void Put(T value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const size_t at = out.size();
        out.resize(at + sizeof(T));
        std::memcpy(out.data() + at, &value, sizeof(T));
    }
    void PutBytes(std::span<const u8> bytes) {
        Put(u32(bytes.size()));
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    template<typename T, typename F>
    void PutVector(const std::vector<T>& values, F&& put_one) {
        Put(u32(values.size()));
        for (const T& value : values) {
            put_one(value);
        }
    }
    void PutTarget(const Target& target) {
        Put(u8(target.kind));
        Put(target.a);
        Put(target.b);
        Put(target.addend);
    }
    std::vector<u8> out;
};

class Reader {
public:
    explicit Reader(std::span<const u8> data)
            : data(data) {}

    template<typename T>
    T Get() {
        static_assert(std::is_trivially_copyable_v<T>);
        T value{};
        if (data.size() - pos < sizeof(T)) {
            ok = false;
            return value;
        }
        std::memcpy(&value, data.data() + pos, sizeof(T));
        pos += sizeof(T);
        return value;
    }
    /// A count of items of at least `item_size` bytes each that the rest of the data can hold.
    u32 GetCount(size_t item_size) {
        const u32 count = Get<u32>();
        if (!ok || u64(count) * item_size > data.size() - pos) {
            ok = false;
            return 0;
        }
        return count;
    }
    std::vector<u8> GetBytes() {
        const u32 size = GetCount(1);
        std::vector<u8> bytes(data.begin() + std::ptrdiff_t(pos), data.begin() + std::ptrdiff_t(pos + size));
        pos += size;
        return bytes;
    }
    template<typename T, typename F>
    std::vector<T> GetVector(size_t item_size, F&& get_one) {
        const u32 count = GetCount(item_size);
        std::vector<T> values;
        values.reserve(count);
        for (u32 i = 0; i < count && ok; ++i) {
            values.push_back(get_one());
        }
        return values;
    }
    template<typename E>
    E GetEnum(u8 limit) {
        const u8 raw = Get<u8>();
        if (raw > limit) {
            ok = false;
        }
        return E(raw);
    }
    Target GetTarget() {
        Target target;
        target.kind = GetEnum<TargetKind>(u8(TargetKind::ConfigPointer));
        target.a = Get<u64>();
        target.b = Get<u64>();
        target.addend = Get<s64>();
        return target;
    }

    std::span<const u8> data;
    size_t pos = 0;
    bool ok = true;
};

constexpr size_t target_size = 1 + 8 + 8 + 8;

void PutBlock(Writer& w, u64 codegen_hash, const Block& block) {
    w.Put(codegen_hash);
    w.Put(block.location);
    w.Put(block.guest_pc);
    w.Put(block.guest_end);
    w.PutVector(block.guest_code, [&](u32 word) { w.Put(word); });
    w.PutBytes(block.near_code);
    w.PutBytes(block.far_code);
    w.PutVector(block.relocations, [&](const Relocation& r) {
        w.Put(u8(r.segment));
        w.Put(u8(r.field));
        w.Put(r.end_delta);
        w.Put(r.offset);
        w.PutTarget(r.target);
    });
    w.PutVector(block.patch_sites, [&](const PatchSite& p) {
        w.Put(u8(p.segment));
        w.Put(u8(p.kind));
        w.Put(p.offset);
        w.Put(p.target);
    });
    w.PutVector(block.indirect_branch_sites, [&](const IndirectBranchSite& s) {
        w.Put(u8(s.segment));
        w.Put(s.offset);
    });
    w.PutVector(block.exit_stubs, [&](const ExitStub& s) {
        w.Put(u8(s.segment));
        w.Put(s.offset);
        w.Put(s.target);
    });
    w.PutVector(block.fastmem_patches, [&](const FastmemPatch& f) {
        w.Put(u8(f.rip_segment));
        w.Put(f.rip);
        w.Put(u8(f.resume_segment));
        w.Put(f.resume);
        w.PutTarget(f.callback);
        w.Put(f.inst_name);
        w.Put(u8(f.recompile));
        w.Put(f.location);
    });
}

std::unique_ptr<Block> GetBlock(Reader& r, u64& codegen_hash) {
    auto block = std::make_unique<Block>();
    codegen_hash = r.Get<u64>();
    block->location = r.Get<u64>();
    block->guest_pc = r.Get<u64>();
    block->guest_end = r.Get<u64>();
    block->guest_code = r.GetVector<u32>(4, [&] { return r.Get<u32>(); });
    block->near_code = r.GetBytes();
    block->far_code = r.GetBytes();
    block->relocations = r.GetVector<Relocation>(7 + target_size, [&] {
        Relocation rel;
        rel.segment = r.GetEnum<Segment>(u8(Segment::Far));
        rel.field = r.GetEnum<X64FieldKind>(u8(X64FieldKind::Imm64));
        rel.end_delta = r.Get<u8>();
        rel.offset = r.Get<u32>();
        rel.target = r.GetTarget();
        return rel;
    });
    block->patch_sites = r.GetVector<PatchSite>(14, [&] {
        PatchSite p;
        p.segment = r.GetEnum<Segment>(u8(Segment::Far));
        p.kind = r.GetEnum<EmitX64::PatchKind>(u8(EmitX64::PatchKind::MovRcx));
        p.offset = r.Get<u32>();
        p.target = r.Get<u64>();
        return p;
    });
    block->indirect_branch_sites = r.GetVector<IndirectBranchSite>(5, [&] {
        IndirectBranchSite s;
        s.segment = r.GetEnum<Segment>(u8(Segment::Far));
        s.offset = r.Get<u32>();
        return s;
    });
    block->exit_stubs = r.GetVector<ExitStub>(13, [&] {
        ExitStub s;
        s.segment = r.GetEnum<Segment>(u8(Segment::Far));
        s.offset = r.Get<u32>();
        s.target = r.Get<u64>();
        return s;
    });
    block->fastmem_patches = r.GetVector<FastmemPatch>(23 + target_size, [&] {
        FastmemPatch f;
        f.rip_segment = r.GetEnum<Segment>(u8(Segment::Far));
        f.rip = r.Get<u32>();
        f.resume_segment = r.GetEnum<Segment>(u8(Segment::Far));
        f.resume = r.Get<u32>();
        f.callback = r.GetTarget();
        f.inst_name = r.Get<u32>();
        f.recompile = r.Get<u8>() != 0;
        f.location = r.Get<u64>();
        return f;
    });
    return block;
}

/// Structural checks a loaded record must pass before anything uses its offsets.
bool BlockIsConsistent(const Block& block) {
    const auto in_segment = [&](Segment segment, u64 offset, u64 size) {
        const u64 limit = segment == Segment::Near ? block.near_code.size() : block.far_code.size();
        return offset <= limit && size <= limit - offset;
    };
    if (block.near_code.empty() || block.guest_end < block.guest_pc || (block.guest_end - block.guest_pc) / 4 != block.guest_code.size()) {
        return false;
    }
    for (const Relocation& r : block.relocations) {
        const u64 size = r.field == X64FieldKind::Imm64 ? 8 : 4;
        if (r.field == X64FieldKind::None || r.field == X64FieldKind::Rel8 || !in_segment(r.segment, r.offset, size)) {
            return false;
        }
        if (r.field != X64FieldKind::Imm64 && r.end_delta < 4) {
            return false;
        }
        if (r.target.kind == TargetKind::IndirectBranchEntry && r.target.a >= block.indirect_branch_sites.size()) {
            return false;
        }
    }
    for (const PatchSite& p : block.patch_sites) {
        if (!in_segment(p.segment, p.offset, 23)) {
            if (!in_segment(p.segment, p.offset, PatchSiteSize(p.kind, true))) {
                return false;
            }
        }
    }
    for (const IndirectBranchSite& s : block.indirect_branch_sites) {
        if (!in_segment(s.segment, s.offset, 5)) {
            return false;
        }
    }
    for (const ExitStub& s : block.exit_stubs) {
        if (!in_segment(s.segment, s.offset, 1)) {
            return false;
        }
    }
    for (const FastmemPatch& f : block.fastmem_patches) {
        if (!in_segment(f.rip_segment, f.rip, 1) || !in_segment(f.resume_segment, f.resume, 0)) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<u8> SerializeHeader(std::span<const u8> frontend_key, u64 host_features) {
    Writer w;
    for (const char c : magic) {
        w.Put(c);
    }
    w.Put(format_version);
    w.Put(host_features);
    const FileHash file_hash = GetHostModule().file_hash;
    w.Put(file_hash[0]);
    w.Put(file_hash[1]);
    w.PutBytes(frontend_key);
    w.Put(Common::Hash128::Of(w.out)[0]);
    return std::move(w.out);
}

std::vector<u8> SerializeChunk(const std::vector<std::pair<u64, const Block*>>& blocks) {
    if (blocks.empty()) {
        return {};
    }
    Writer w;
    size_t estimate = 0;
    for (const auto& [codegen_hash, block] : blocks) {
        estimate += 128 + block->near_code.size() + block->far_code.size() + block->relocations.size() * 32;
    }
    w.out.reserve(estimate);
    for (const char c : chunk_magic) {
        w.Put(c);
    }
    w.Put(u64(blocks.size()));
    const size_t length_at = w.out.size();
    w.Put(u64(0));
    const size_t body_at = w.out.size();
    for (const auto& [codegen_hash, block] : blocks) {
        PutBlock(w, codegen_hash, *block);
    }
    const u64 length = w.out.size() - body_at;
    std::memcpy(w.out.data() + length_at, &length, sizeof(length));
    w.Put(Common::Hash128::Of(std::span{w.out}.subspan(body_at))[0]);
    return std::move(w.out);
}

std::optional<std::vector<std::pair<u64, std::unique_ptr<Block>>>> DeserializeBlocks(
    std::span<const u8> data, std::span<const u8> frontend_key, u64 host_features, size_t* consumed) {
    Reader r(data);
    for (const char c : magic) {
        if (r.Get<char>() != c) {
            return std::nullopt;
        }
    }
    const FileHash file_hash = GetHostModule().file_hash;
    if (r.Get<u32>() != format_version || r.Get<u64>() != host_features || r.Get<u64>() != file_hash[0] || r.Get<u64>() != file_hash[1] || file_hash == FileHash{}) {
        return std::nullopt;
    }
    const std::vector<u8> key = r.GetBytes();
    const size_t header_size = r.pos;
    const u64 header_checksum = r.Get<u64>();
    if (!r.ok || !std::ranges::equal(key, frontend_key) || header_checksum != Common::Hash128::Of(data.first(header_size))[0]) {
        return std::nullopt;
    }

    // Chunks in write order; a later record of a location replaces an earlier one. A damaged or
    // cut-off chunk (a write that did not finish) ends the image: everything before it is whole.
    std::vector<std::pair<u64, std::unique_ptr<Block>>> blocks;
    while (r.pos < data.size()) {
        Reader chunk = r;
        bool magic_ok = true;
        for (const char c : chunk_magic) {
            magic_ok &= chunk.Get<char>() == c;
        }
        const u64 count = chunk.Get<u64>();
        const u64 length = chunk.Get<u64>();
        if (!chunk.ok || !magic_ok || length + sizeof(u64) > data.size() - chunk.pos || count > length) {
            break;
        }
        const auto body = data.subspan(chunk.pos, length);
        u64 checksum;
        std::memcpy(&checksum, data.data() + chunk.pos + length, sizeof(checksum));
        if (checksum != Common::Hash128::Of(body)[0]) {
            break;
        }
        Reader br(body);
        std::vector<std::pair<u64, std::unique_ptr<Block>>> chunk_blocks;
        chunk_blocks.reserve(count);
        for (u64 i = 0; i < count; ++i) {
            u64 codegen_hash = 0;
            auto block = GetBlock(br, codegen_hash);
            if (!br.ok || !BlockIsConsistent(*block)) {
                break;
            }
            chunk_blocks.emplace_back(codegen_hash, std::move(block));
        }
        if (!br.ok || chunk_blocks.size() != count || br.pos != body.size()) {
            break;
        }
        std::ranges::move(chunk_blocks, std::back_inserter(blocks));
        r.pos = chunk.pos + length + sizeof(u64);
    }
    if (consumed) {
        *consumed = r.pos;
    }
    return blocks;
}

}  // namespace Image

namespace {

u64 PackFallbackKey(const std::tuple<bool, size_t, int, int>& key) {
    const auto [ordered, bitsize, a, b] = key;
    return (u64(ordered) << 48) | (u64(bitsize & 0xFFFF) << 32) | (u64(u16(a)) << 16) | u64(u16(b));
}

template<typename T>
T ReadUnaligned(const u8* p) {
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}

template<typename T>
void WriteUnaligned(u8* p, T value) {
    std::memcpy(p, &value, sizeof(T));
}

}  // namespace

CodeImageLink::CodeImageLink(std::shared_ptr<A64::CodeImage> image, const A64::UserConfig& conf, BlockOfCode& code, A64EmitX64& emitter)
        : image(std::move(image)), conf(conf), code(code), emitter(emitter), codegen_hash(Image::CodegenHash(conf)) {}

void CodeImageLink::BuildPreludeSymbols() {
    if (prelude_symbols_built) {
        return;
    }
    prelude_symbols_built = true;
    const auto add = [this](const void* address, Image::PreludeSymbol symbol, u64 key) {
        if (address) {
            prelude_symbols.emplace(address, std::pair{u64(symbol), key});
            prelude_addresses.emplace(std::pair{u64(symbol), key}, static_cast<const u8*>(address));
        }
    };
    const auto& returns = code.GetReturnFromRunCodeAddresses();
    for (size_t i = 0; i < returns.size(); ++i) {
        add(returns[i], Image::PreludeSymbol::ReturnFromRunCode, i);
    }
    add(emitter.terminal_handler_pop_rsb_hint, Image::PreludeSymbol::PopRSBHint, 0);
    add(emitter.terminal_handler_fast_dispatch_hint, Image::PreludeSymbol::FastDispatchHint, 0);
    add(emitter.cntpct_fallback, Image::PreludeSymbol::CntpctFallback, 0);
    add(reinterpret_cast<const void*>(emitter.fast_dispatch_table_lookup), Image::PreludeSymbol::FastDispatchLookup, 0);
    add(reinterpret_cast<const void*>(emitter.memory_read_128), Image::PreludeSymbol::MemoryRead128, 0);
    add(reinterpret_cast<const void*>(emitter.memory_write_128), Image::PreludeSymbol::MemoryWrite128, 0);
    add(reinterpret_cast<const void*>(emitter.memory_exclusive_write_128), Image::PreludeSymbol::MemoryExclusiveWrite128, 0);
    for (const auto& [key, fn] : emitter.read_fallbacks) {
        add(reinterpret_cast<const void*>(fn), Image::PreludeSymbol::ReadFallback, PackFallbackKey(key));
    }
    for (const auto& [key, fn] : emitter.write_fallbacks) {
        add(reinterpret_cast<const void*>(fn), Image::PreludeSymbol::WriteFallback, PackFallbackKey(key));
    }
    for (const auto& [key, fn] : emitter.exclusive_write_fallbacks) {
        add(reinterpret_cast<const void*>(fn), Image::PreludeSymbol::ExclusiveWriteFallback, PackFallbackKey(key));
    }
}

const u8* CodeImageLink::ResolvePrelude(u64 symbol, u64 key) const {
    const auto it = prelude_addresses.find({symbol, key});
    return it == prelude_addresses.end() ? nullptr : it->second;
}

std::optional<Image::Target> CodeImageLink::Classify(const u8* target, const Segments& segments, [[maybe_unused]] const BlockCapture& capture) {
    using Image::Target;
    using Image::TargetKind;
    if (target >= segments.near_begin && target < segments.near_end) {
        return Target{TargetKind::SelfNear, u64(target - segments.near_begin), 0, 0};
    }
    if (segments.far_begin && target >= segments.far_begin && target < segments.far_end) {
        return Target{TargetKind::SelfFar, u64(target - segments.far_begin), 0, 0};
    }
    const u8* const buffer = code.getCode();
    const size_t buffer_size = code.GetTotalCodeSize();
    if (target >= buffer && target < buffer + buffer_size) {
        if (const auto it = emitter.exit_stub_targets.find(target); it != emitter.exit_stub_targets.end()) {
            return Target{TargetKind::ExitStub, it->second, 0, 0};
        }
        BuildPreludeSymbols();
        if (const auto it = prelude_symbols.find(target); it != prelude_symbols.end()) {
            return Target{TargetKind::Prelude, it->second.first, it->second.second, 0};
        }
        const auto pool = code.GetConstantPoolRange();
        if (target >= pool.data() && target < pool.data() + pool.size()) {
            const size_t offset = size_t(target - pool.data());
            const u8* const entry = pool.data() + (offset & ~size_t(15));
            return Target{TargetKind::Constant, ReadUnaligned<u64>(entry), ReadUnaligned<u64>(entry + 8), s64(offset & 15)};
        }
        // Another block, or the middle of something: not nameable.
        return std::nullopt;
    }
    const auto& module = Image::GetHostModule();
    if (module.base && target >= module.base && target < module.base + module.size) {
        return Target{TargetKind::Host, u64(target - module.base), 0, 0};
    }
    if (conf.inline_cntpct_factor) {
        const auto* const p = reinterpret_cast<const u8*>(conf.inline_cntpct_factor);
        if (target >= p && target < p + sizeof(*conf.inline_cntpct_factor)) {
            return Target{TargetKind::ConfigPointer, u64(Image::ConfigPointer::InlineCntpctFactor), 0, s64(target - p)};
        }
    }
    if (conf.inline_cntpct_mode) {
        const auto* const p = reinterpret_cast<const u8*>(conf.inline_cntpct_mode);
        if (target >= p && target < p + sizeof(*conf.inline_cntpct_mode)) {
            return Target{TargetKind::ConfigPointer, u64(Image::ConfigPointer::InlineCntpctMode), 0, s64(target - p)};
        }
    }
    return std::nullopt;
}

std::optional<Image::Target> CodeImageLink::ClassifyImm64(u64 value, const Segments& segments, const BlockCapture& capture, std::span<const u64> ir_immediates) {
    const auto* const target = reinterpret_cast<const u8*>(value);
    const u8* const buffer = code.getCode();
    const auto& module = Image::GetHostModule();
    const bool in_buffer = target >= buffer && target < buffer + code.GetTotalCodeSize();
    const bool in_module = module.base && target >= module.base && target < module.base + module.size;
    const bool is_config_pointer = (conf.inline_cntpct_factor && value == std::bit_cast<u64>(conf.inline_cntpct_factor)) ||
                                   (conf.inline_cntpct_mode && value == std::bit_cast<u64>(conf.inline_cntpct_mode));
    if (!in_buffer && !in_module && !is_config_pointer) {
        // A plain constant. Any other host address (heap, another module) cannot be named.
        if (Image::IsMappedHostAddress(value)) {
            return Image::Target{Image::TargetKind(0xFF), 0, 0, 0};
        }
        return std::nullopt;
    }
    if (std::ranges::find(ir_immediates, value) != ir_immediates.end()) {
        // A guest value that happens to look like a host address: ambiguous.
        return Image::Target{Image::TargetKind(0xFF), 0, 0, 0};
    }
    if (auto classified = Classify(target, segments, capture)) {
        return classified;
    }
    return Image::Target{Image::TargetKind(0xFF), 0, 0, 0};
}

std::optional<Image::Block> CodeImageLink::BuildRecord(u64 location, u64 guest_pc, u64 guest_end, std::vector<u32> guest_code, const u8* entrypoint, size_t size, const BlockCapture& capture, std::span<const u64> ir_immediates) {
    using namespace Image;
    const Segments segments{entrypoint, entrypoint + size, capture.far_begin, capture.far_end};
    Block record;
    record.location = location;
    record.guest_pc = guest_pc;
    record.guest_end = guest_end;
    record.guest_code = std::move(guest_code);
    record.near_code.assign(entrypoint, entrypoint + size);
    if (capture.far_begin) {
        record.far_code.assign(capture.far_begin, capture.far_end);
    }

    const auto locate = [&](const u8* p, bool allow_end = false) -> std::optional<std::pair<Segment, u32>> {
        if (p >= segments.near_begin && (p < segments.near_end || (allow_end && p == segments.near_end))) {
            return std::pair{Segment::Near, u32(p - segments.near_begin)};
        }
        if (segments.far_begin && p >= segments.far_begin && (p < segments.far_end || (allow_end && p == segments.far_end))) {
            return std::pair{Segment::Far, u32(p - segments.far_begin)};
        }
        return std::nullopt;
    };
    const auto bytes_of = [&](Segment s) -> std::vector<u8>& {
        return s == Segment::Near ? record.near_code : record.far_code;
    };

    // Patch sites are rewritten when the block is installed: their bytes are not part of it.
    struct Range {
        Segment segment;
        u32 begin, end;
    };
    std::vector<Range> rewritten;
    for (const auto& site : capture.patch_sites) {
        const auto where = locate(site.site);
        if (!where) {
            return std::nullopt;
        }
        const size_t site_size = PatchSiteSize(site.kind, conf.compact_block_exits);
        auto& bytes = bytes_of(where->first);
        if (where->second + site_size > bytes.size()) {
            return std::nullopt;
        }
        std::fill_n(bytes.begin() + where->second, site_size, u8(0));
        rewritten.push_back({where->first, where->second, u32(where->second + site_size)});
        record.patch_sites.push_back({where->first, site.kind, where->second, site.target});
    }
    for (const auto& [target, stub] : capture.exit_stubs) {
        const auto where = locate(stub);
        if (!where) {
            return std::nullopt;
        }
        record.exit_stubs.push_back({where->first, where->second, target});
    }
    for (const auto& patch : capture.fastmem_patches) {
        const auto rip = locate(patch.rip);
        const auto resume = locate(patch.resume, true);
        const auto callback = Classify(patch.callback, segments, capture);
        if (!rip || !resume || !callback) {
            return std::nullopt;
        }
        record.fastmem_patches.push_back({rip->first, rip->second, resume->first, resume->second, *callback,
                                          patch.inst_name, patch.recompile, patch.location});
    }

    const auto decode_segment = [&](Segment segment, const u8* begin, const u8* end) -> bool {
        auto& bytes = bytes_of(segment);
        size_t pos = 0;
        const size_t length = size_t(end - begin);
        while (pos < length) {
            const auto insn = DecodeX64Instruction(begin + pos, length - pos);
            if (!insn) {
                return false;
            }
            const size_t field = pos + insn->field_offset;
            const size_t insn_end = pos + insn->length;
            pos = insn_end;
            if (insn->field == X64FieldKind::None) {
                continue;
            }
            const bool in_patch_site = std::ranges::any_of(rewritten, [&](const Range& r) {
                return r.segment == segment && field >= r.begin && field < r.end;
            });
            if (in_patch_site) {
                continue;
            }
            switch (insn->field) {
            case X64FieldKind::Rel8: {
                const u8* const target = begin + insn_end + s8(begin[field]);
                if (target < begin || target >= end) {
                    return false;
                }
                break;
            }
            case X64FieldKind::Rel32:
            case X64FieldKind::RipDisp32: {
                const u8* const target = begin + insn_end + ReadUnaligned<s32>(begin + field);
                if (target >= begin && target < end) {
                    break;  // inside this segment: position-independent
                }
                const auto classified = Classify(target, segments, capture);
                if (!classified) {
                    return false;
                }
                record.relocations.push_back({segment, insn->field, u8(insn_end - field), u32(field), *classified});
                std::fill_n(bytes.begin() + std::ptrdiff_t(field), 4, u8(0));
                break;
            }
            case X64FieldKind::Imm64: {
                const auto classified = ClassifyImm64(ReadUnaligned<u64>(begin + field), segments, capture, ir_immediates);
                if (!classified) {
                    break;  // a constant
                }
                if (classified->kind == TargetKind(0xFF)) {
                    return false;
                }
                record.relocations.push_back({segment, insn->field, 0, u32(field), *classified});
                std::fill_n(bytes.begin() + std::ptrdiff_t(field), 8, u8(0));
                break;
            }
            case X64FieldKind::None:
                break;
            }
        }
        return true;
    };
    if (!decode_segment(Segment::Near, segments.near_begin, segments.near_end)) {
        return std::nullopt;
    }
    if (segments.far_begin && !decode_segment(Segment::Far, segments.far_begin, segments.far_end)) {
        return std::nullopt;
    }
    return record;
}

void CodeImageLink::Record(const IR::Block& block, CodePtr entrypoint, size_t size, const BlockCapture& capture) {
    auto& impl = image->GetImpl();
    const u64 location = block.Location().Value();
    const auto existing = impl.Find(codegen_hash, location);
    if (existing && !conf.code_image_verify && GuestCodeMatches(*existing)) {
        return;
    }
    const A64::LocationDescriptor descriptor{block.Location()};
    const u64 guest_pc = descriptor.PC();
    const u64 guest_end = A64::LocationDescriptor{block.EndLocation()}.PC();
    std::vector<u32> guest_code;
    if (guest_end > guest_pc) {
        guest_code.reserve((guest_end - guest_pc) / 4);
        for (u64 pc = guest_pc; pc < guest_end; pc += 4) {
            const auto word = conf.callbacks->MemoryReadCode(pc);
            if (!word) {
                impl.not_recordable.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            guest_code.push_back(*word);
        }
    }
    std::vector<u64> ir_immediates;
    for (const auto& inst : block.instructions) {
        for (size_t i = 0; i < inst.NumArgs(); ++i) {
            const IR::Value arg = inst.GetArg(i);
            if (arg.IsImmediate() && arg.GetType() == IR::Type::U64) {
                ir_immediates.push_back(arg.GetU64());
            }
        }
    }
    auto record = BuildRecord(location, guest_pc, guest_end, std::move(guest_code), static_cast<const u8*>(entrypoint), size,
                              capture, ir_immediates);
    if (!record) {
        impl.not_recordable.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (existing && conf.code_image_verify && GuestCodeMatches(*existing)) {
        // The block just compiled must give the stored record, and so must the stored record
        // installed (as unused code) and read back.
        if (!(*existing == *record)) {
            impl.verify_mismatches.fetch_add(1, std::memory_order_relaxed);
        }
        BlockCapture installed;
        const auto entrypoint = Install(*existing, installed);
        if (!entrypoint) {
            impl.not_installable.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto reread = BuildRecord(existing->location, existing->guest_pc, existing->guest_end, existing->guest_code,
                                        static_cast<const u8*>(*entrypoint), existing->near_code.size(), installed, {});
        if (!reread || !(*reread == *existing)) {
            impl.verify_mismatches.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    // New, or the guest code at this location changed since the record was made.
    impl.Add(codegen_hash, std::make_shared<const Image::Block>(std::move(*record)));
}

bool CodeImageLink::GuestCodeMatches(const Image::Block& block) const {
    for (size_t i = 0; i < block.guest_code.size(); ++i) {
        if (conf.callbacks->MemoryReadCode(block.guest_pc + 4 * i) != block.guest_code[i]) {
            return false;
        }
    }
    return true;
}

const u8* CodeImageLink::EnsureExitStub(u64 target) {
    if (const auto it = emitter.exit_stubs.find(target); it != emitter.exit_stubs.end()) {
        return static_cast<const u8*>(it->second);
    }
    // Exit stubs go in the far region, as FlushPendingExitStubs does in Emit. (Not named `far`:
    // windows.h defines it.)
    const bool in_far_region = code.HasFarCode();
    if (in_far_region) {
        code.SwitchToFarCode();
    }
    emitter.PendingExitStub(IR::LocationDescriptor{target});
    emitter.FlushPendingExitStubs();
    if (in_far_region) {
        code.SwitchToNearCode();
    }
    emitter.shared_labels.clear();
    return static_cast<const u8*>(emitter.exit_stubs.at(target));
}

std::optional<CodePtr> CodeImageLink::Install(const Image::Block& block, BlockCapture& installed) {
    using namespace Image;
    BuildPreludeSymbols();
    if (!block.far_code.empty() && !code.HasFarCode()) {
        return std::nullopt;
    }
    const size_t sites = block.indirect_branch_sites.size();
    // Records with indirect-branch cache sites come from a code generator this one is not.
    if (sites != 0) {
        return std::nullopt;
    }
    const auto& module = GetHostModule();

    code.EnableWriting();
    code.align();
    u8* const near_code = code.getCurr<u8*>();
    std::memcpy(near_code, block.near_code.data(), block.near_code.size());
    code.SetCodePtr(near_code + block.near_code.size());
    u8* far_code = nullptr;
    if (!block.far_code.empty()) {
        code.SwitchToFarCode();
        far_code = code.getCurr<u8*>();
        std::memcpy(far_code, block.far_code.data(), block.far_code.size());
        code.SetCodePtr(far_code + block.far_code.size());
        code.SwitchToNearCode();
    }
    const auto segment_base = [&](Segment s) { return s == Segment::Near ? near_code : far_code; };
    const auto rollback = [&] {
        code.SetCodePtr(near_code);
        if (far_code) {
            code.SwitchToFarCode();
            code.SetCodePtr(far_code);
            code.SwitchToNearCode();
        }
        code.DisableWriting();
        return std::nullopt;
    };

    // Everything but exit stubs resolves without side effects; check it all before creating any.
    const auto resolve = [&](const Target& target) -> const u8* {
        switch (target.kind) {
        case TargetKind::SelfNear:
            return target.a < block.near_code.size() ? near_code + target.a : nullptr;
        case TargetKind::SelfFar:
            return far_code && target.a < block.far_code.size() ? far_code + target.a : nullptr;
        case TargetKind::Prelude: {
            const u8* const p = ResolvePrelude(target.a, target.b);
            return p ? p + target.addend : nullptr;
        }
        case TargetKind::Constant:
            return static_cast<const u8*>(code.GetConstantAddress(target.a, target.b)) + target.addend;
        case TargetKind::IndirectBranchEntry:
            return nullptr;  // no indirect-branch cache in this code generator
        case TargetKind::ExitStub:
            return nullptr;  // resolved below
        case TargetKind::Host:
            return module.base && target.a < module.size ? module.base + target.a : nullptr;
        case TargetKind::ConfigPointer:
            if (target.a == u64(ConfigPointer::InlineCntpctFactor) && conf.inline_cntpct_factor) {
                return reinterpret_cast<const u8*>(conf.inline_cntpct_factor) + target.addend;
            }
            if (target.a == u64(ConfigPointer::InlineCntpctMode) && conf.inline_cntpct_mode) {
                return reinterpret_cast<const u8*>(conf.inline_cntpct_mode) + target.addend;
            }
            return nullptr;
        }
        return nullptr;
    };
    const auto fits_rel32 = [](const u8* target, const u8* insn_end) {
        const s64 disp = target - insn_end;
        return disp >= std::numeric_limits<s32>::min() && disp <= std::numeric_limits<s32>::max();
    };
    std::vector<const u8*> targets(block.relocations.size());
    for (size_t i = 0; i < block.relocations.size(); ++i) {
        const Relocation& r = block.relocations[i];
        if (!segment_base(r.segment)) {
            return rollback();
        }
        if (r.target.kind == TargetKind::ExitStub) {
            continue;
        }
        targets[i] = resolve(r.target);
        if (!targets[i]) {
            return rollback();
        }
        if (r.field != X64FieldKind::Imm64 && !fits_rel32(targets[i], segment_base(r.segment) + r.offset + r.end_delta)) {
            return rollback();
        }
    }
    std::vector<const u8*> fastmem_callbacks;
    for (const FastmemPatch& f : block.fastmem_patches) {
        const u8* const callback = f.callback.kind == TargetKind::ExitStub ? nullptr : resolve(f.callback);
        if (!callback || !segment_base(f.rip_segment) || !segment_base(f.resume_segment)) {
            return rollback();
        }
        fastmem_callbacks.push_back(callback);
    }
    for (const PatchSite& p : block.patch_sites) {
        if (!segment_base(p.segment)) {
            return rollback();
        }
    }
    for (const IndirectBranchSite& s : block.indirect_branch_sites) {
        if (!segment_base(s.segment)) {
            return rollback();
        }
    }
    for (const ExitStub& s : block.exit_stubs) {
        if (!segment_base(s.segment)) {
            return rollback();
        }
    }

    // Committed from here on.
    installed = {};
    installed.far_begin = far_code;
    installed.far_end = far_code ? far_code + block.far_code.size() : nullptr;
    for (const ExitStub& s : block.exit_stubs) {
        const u8* const stub = segment_base(s.segment) + s.offset;
        if (!emitter.exit_stubs.contains(s.target)) {
            emitter.exit_stubs[s.target] = stub;
            emitter.exit_stub_targets[stub] = s.target;
        }
        installed.exit_stubs.emplace_back(s.target, stub);
    }
    for (size_t i = 0; i < block.relocations.size(); ++i) {
        if (block.relocations[i].target.kind == TargetKind::ExitStub) {
            targets[i] = EnsureExitStub(block.relocations[i].target.a);
        }
    }
    if (conf.compact_block_exits) {
        for (const PatchSite& p : block.patch_sites) {
            if (!emitter.GetBasicBlock(IR::LocationDescriptor{p.target})) {
                EnsureExitStub(p.target);
            }
        }
    }
    for (size_t i = 0; i < block.relocations.size(); ++i) {
        const Relocation& r = block.relocations[i];
        u8* const field = segment_base(r.segment) + r.offset;
        if (r.field == X64FieldKind::Imm64) {
            WriteUnaligned<u64>(field, std::bit_cast<u64>(targets[i]));
        } else {
            WriteUnaligned<s32>(field, s32(targets[i] - (field + r.end_delta)));
        }
    }
    for (size_t i = 0; i < block.fastmem_patches.size(); ++i) {
        const FastmemPatch& f = block.fastmem_patches[i];
        installed.fastmem_patches.push_back({segment_base(f.rip_segment) + f.rip, segment_base(f.resume_segment) + f.resume,
                                             fastmem_callbacks[i], f.inst_name, f.recompile, f.location});
    }

    // Link sites: written the way EmitX64::Patch writes them.
    const CodePtr saved = code.getCurr();
    for (const PatchSite& p : block.patch_sites) {
        u8* const site = segment_base(p.segment) + p.offset;
        const IR::LocationDescriptor target{p.target};
        code.SetCodePtr(site);
        installed.patch_sites.push_back({p.target, p.kind, site});
        const auto linked = emitter.GetBasicBlock(target);
        const CodePtr target_code = linked ? linked->entrypoint : nullptr;
        switch (p.kind) {
        case EmitX64::PatchKind::Jg:
            emitter.EmitPatchJg(target, target_code);
            break;
        case EmitX64::PatchKind::Jz:
            emitter.EmitPatchJz(target, target_code);
            break;
        case EmitX64::PatchKind::Jmp:
            emitter.EmitPatchJmp(target, target_code);
            break;
        case EmitX64::PatchKind::MovRcx:
            emitter.EmitPatchMovRcx(target_code);
            break;
        }
    }
    code.SetCodePtr(saved);
    code.DisableWriting();
    return near_code;
}

bool CodeImageLink::Contains(IR::LocationDescriptor location) const {
    const auto block = image->GetImpl().Find(codegen_hash, location.Value());
    return block && GuestCodeMatches(*block);
}

std::optional<CodePtr> CodeImageLink::TryLoad(IR::LocationDescriptor location) {
    auto& impl = image->GetImpl();
    const auto block = impl.Find(codegen_hash, location.Value());
    if (!block || conf.code_image_verify) {
        return std::nullopt;  // verify mode: compiled, then compared in Record
    }
    if (!GuestCodeMatches(*block)) {
        impl.guest_changed.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    BlockCapture installed;
    const auto entrypoint = Install(*block, installed);
    if (!entrypoint) {
        impl.not_installable.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    // Registered like a compiled block (A64EmitX64::Emit).
    code.EnableWriting();
    for (const auto& patch : installed.patch_sites) {
        const CodePtr saved = code.getCurr();
        code.SetCodePtr(patch.site);
        emitter.AddPatchLocation(IR::LocationDescriptor{patch.target}, patch.kind);
        code.SetCodePtr(saved);
    }
    for (const auto& f : installed.fastmem_patches) {
        emitter.AddFastmemPatch(f.rip, f.resume, reinterpret_cast<void (*)()>(const_cast<u8*>(f.callback)),
                                A64EmitX64::DoNotFastmemMarker{IR::LocationDescriptor{f.location}, f.inst_name}, f.recompile);
    }
    const auto range = boost::icl::discrete_interval<u64>::closed(block->guest_pc, block->guest_end - 1);
    emitter.block_ranges.AddRange(range, location);
    emitter.RegisterBlock(location, *entrypoint, block->near_code.size());
    code.DisableWriting();
    impl.loaded.fetch_add(1, std::memory_order_relaxed);
    return entrypoint;
}

}  // namespace Dynarmic::Backend::X64

namespace Dynarmic::A64 {

using Backend::X64::Image::Block;

std::shared_ptr<const Block> CodeImage::Impl::Find(u64 codegen_hash, u64 location) const {
    std::lock_guard lock{mutex};
    const auto it = blocks.find({codegen_hash, location});
    return it == blocks.end() ? nullptr : it->second;
}

void CodeImage::Impl::Add(u64 codegen_hash, std::shared_ptr<const Block> block) {
    std::lock_guard lock{mutex};
    const std::pair key{codegen_hash, block->location};
    blocks.insert_or_assign(key, std::move(block));
    added.push_back(key);
    dirty = true;
    recorded.fetch_add(1, std::memory_order_relaxed);
}

CodeImage::CodeImage(std::vector<std::uint8_t> frontend_key)
        : impl(std::make_unique<Impl>(std::move(frontend_key))) {}

CodeImage::~CodeImage() = default;

void CodeImage::SetFrontendKey(std::vector<std::uint8_t> frontend_key) {
    std::lock_guard lock{impl->mutex};
    impl->frontend_key = std::move(frontend_key);
}

bool CodeImage::Deserialize(std::span<const std::uint8_t> data, std::size_t* consumed) {
    std::vector<u8> key;
    {
        std::lock_guard lock{impl->mutex};
        key = impl->frontend_key;
    }
    if (consumed) {
        *consumed = 0;
    }
    auto blocks = Backend::X64::Image::DeserializeBlocks(data, key, u64(Backend::X64::BlockOfCode::HostFeatures()), consumed);
    std::lock_guard lock{impl->mutex};
    impl->blocks.clear();
    impl->added.clear();
    if (blocks) {
        impl->blocks.reserve(blocks->size());
    }
    impl->dirty = false;
    if (!blocks) {
        return false;
    }
    for (auto& [codegen_hash, block] : *blocks) {
        const u64 location = block->location;
        impl->blocks.insert_or_assign({codegen_hash, location}, std::move(block));
    }
    return true;
}

namespace {

std::vector<std::uint8_t> SerializeHeld(std::vector<std::pair<u64, std::shared_ptr<const Block>>> held) {
    // In key order, so the same blocks always serialise to the same bytes.
    std::ranges::sort(held, [](const auto& a, const auto& b) {
        return std::pair{a.first, a.second->location} < std::pair{b.first, b.second->location};
    });
    std::vector<std::pair<u64, const Block*>> blocks;
    blocks.reserve(held.size());
    for (const auto& [codegen_hash, block] : held) {
        blocks.emplace_back(codegen_hash, block.get());
    }
    return Backend::X64::Image::SerializeChunk(blocks);
}

}  // namespace

std::vector<std::uint8_t> CodeImage::Serialize() const {
    // Records are immutable once added: take references under the lock, encode outside it, so
    // Jits compiling meanwhile are not held up.
    std::vector<std::pair<u64, std::shared_ptr<const Block>>> held;
    std::vector<u8> key;
    {
        std::lock_guard lock{impl->mutex};
        held.reserve(impl->blocks.size());
        for (const auto& [map_key, block] : impl->blocks) {
            held.emplace_back(map_key.first, block);
        }
        key = impl->frontend_key;
        impl->added.clear();
        impl->dirty = false;
    }
    std::vector<u8> out = Backend::X64::Image::SerializeHeader(key, u64(Backend::X64::BlockOfCode::HostFeatures()));
    const std::vector<u8> chunk = SerializeHeld(std::move(held));
    out.insert(out.end(), chunk.begin(), chunk.end());
    return out;
}

std::vector<std::uint8_t> CodeImage::SerializeAdded() {
    std::vector<std::pair<u64, std::shared_ptr<const Block>>> held;
    {
        std::lock_guard lock{impl->mutex};
        held.reserve(impl->added.size());
        for (const auto& key : impl->added) {
            if (const auto it = impl->blocks.find(key); it != impl->blocks.end()) {
                held.emplace_back(key.first, it->second);
            }
        }
        impl->added.clear();
        impl->dirty = false;
    }
    // A location recorded twice since the last call (its guest code changed) is written once.
    std::ranges::sort(held, [](const auto& a, const auto& b) { return a.second < b.second; });
    held.erase(std::unique(held.begin(), held.end(), [](const auto& a, const auto& b) { return a.second == b.second; }), held.end());
    return SerializeHeld(std::move(held));
}

CodeImage::Statistics CodeImage::GetStatistics() const {
    Statistics s;
    {
        std::lock_guard lock{impl->mutex};
        s.blocks = impl->blocks.size();
    }
    s.loaded = impl->loaded.load(std::memory_order_relaxed);
    s.recorded = impl->recorded.load(std::memory_order_relaxed);
    s.not_recordable = impl->not_recordable.load(std::memory_order_relaxed);
    s.guest_changed = impl->guest_changed.load(std::memory_order_relaxed);
    s.not_installable = impl->not_installable.load(std::memory_order_relaxed);
    s.verify_mismatches = impl->verify_mismatches.load(std::memory_order_relaxed);
    return s;
}

bool CodeImage::Dirty() const {
    std::lock_guard lock{impl->mutex};
    return impl->dirty;
}

}  // namespace Dynarmic::A64
