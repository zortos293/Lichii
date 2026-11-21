// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dynarmic/backend/x64/emit_x64.h"
#include "dynarmic/backend/x64/x64_decode.h"
#include "dynarmic/interface/A64/code_image.h"
#include "dynarmic/interface/A64/config.h"

namespace Dynarmic::IR {
class Block;
}  // namespace Dynarmic::IR

namespace Dynarmic::Backend::X64 {

class A64EmitX64;
class BlockOfCode;

/// Format of one block in a code image. All code addresses are offsets into the block's two
/// segments (its near code and its part of the far code region); every field of the code that
/// depends on where the code or its referents are is listed as a relocation and is zero in
/// the stored bytes, so a block record is the same whatever addresses it was compiled at.
namespace Image {

enum class Segment : u8 { Near,
                          Far };

enum class TargetKind : u8 {
    SelfNear,             ///< a: offset in the block's near code
    SelfFar,              ///< a: offset in the block's far code
    Prelude,              ///< a: PreludeSymbol, b: its key (fallback thunks)
    Constant,             ///< a, b: the constant pool entry (lower, upper)
    IndirectBranchEntry,  ///< a: the block's indirect-branch site (index into Block::indirect_branch_sites)
    ExitStub,             ///< a: target location descriptor
    Host,                 ///< a: offset from the start of the executable that holds dynarmic
    ConfigPointer,        ///< a: ConfigPointer
};

enum class PreludeSymbol : u8 {
    ReturnFromRunCode,  ///< b: index into BlockOfCode::GetReturnFromRunCodeAddresses
    PopRSBHint,
    FastDispatchHint,
    CntpctFallback,  ///< the inline CNTPCT slow path
    FastDispatchLookup,
    MemoryRead128,
    MemoryWrite128,
    MemoryExclusiveWrite128,
    ReadFallback,  ///< b: the fallback's key, packed by PackFallbackKey
    WriteFallback,
    ExclusiveWriteFallback,
};

enum class ConfigPointer : u8 {
    InlineCntpctFactor,
    InlineCntpctMode,
};

struct Target {
    TargetKind kind{};
    u64 a = 0;
    u64 b = 0;
    s64 addend = 0;
    bool operator==(const Target&) const = default;
};

struct Relocation {
    Segment segment{};
    X64FieldKind field{};
    u8 end_delta = 0;  ///< Rel32/RipDisp32: from the field to the end of the instruction
    u32 offset = 0;    ///< of the field in its segment
    Target target;
    bool operator==(const Relocation&) const = default;
};

struct PatchSite {
    Segment segment{};
    EmitX64::PatchKind kind{};
    u32 offset = 0;
    u64 target = 0;
    bool operator==(const PatchSite&) const = default;
};

struct IndirectBranchSite {
    Segment segment{};
    u32 offset = 0;  ///< the site's first instruction (patched once the site is megamorphic)
    bool operator==(const IndirectBranchSite&) const = default;
};

struct ExitStub {
    Segment segment{};  ///< far code, or near code when the code cache has no far region
    u32 offset = 0;
    u64 target = 0;
    bool operator==(const ExitStub&) const = default;
};

struct FastmemPatch {
    Segment rip_segment{};
    u32 rip = 0;
    Segment resume_segment{};
    u32 resume = 0;
    Target callback;
    u32 inst_name = 0;
    bool recompile = false;
    u64 location = 0;
    bool operator==(const FastmemPatch&) const = default;
};

struct Block {
    u64 location = 0;
    /// Guest instructions the block was translated from: [guest_pc, guest_end).
    u64 guest_pc = 0;
    u64 guest_end = 0;
    std::vector<u32> guest_code;
    std::vector<u8> near_code;
    std::vector<u8> far_code;
    std::vector<Relocation> relocations;
    std::vector<PatchSite> patch_sites;
    std::vector<IndirectBranchSite> indirect_branch_sites;
    std::vector<ExitStub> exit_stubs;
    std::vector<FastmemPatch> fastmem_patches;
    bool operator==(const Block&) const = default;
};

/// Bytes a patch site of `kind` takes (EmitX64::Patch rewrites exactly these).
size_t PatchSiteSize(EmitX64::PatchKind kind, bool compact_block_exits);

using FileHash = std::array<u64, 2>;

/// The executable or shared object that holds dynarmic: its mapped range and a hash of its file.
struct HostModule {
    const u8* base = nullptr;
    size_t size = 0;
    FileHash file_hash{};
};
const HostModule& GetHostModule();

/// Whether `address` is in a mapped region of this process.
bool IsMappedHostAddress(u64 address);

/// Hash of every UserConfig field that changes the code a Jit generates. Per-instance pointers
/// (callbacks, monitor, TPIDR, page table, fastmem base) count only as present or absent: with
/// core_independent_code the code does not contain them.
u64 CodegenHash(const A64::UserConfig& conf);

/// An image is a header followed by chunks of records, each with its own checksum, so new
/// records can be appended to an image file.
std::vector<u8> SerializeHeader(std::span<const u8> frontend_key, u64 host_features);
/// Empty for no blocks.
std::vector<u8> SerializeChunk(const std::vector<std::pair<u64, const Block*>>& blocks);
/// nullopt when the header does not match or the data is damaged.
std::optional<std::vector<std::pair<u64, std::unique_ptr<Block>>>> DeserializeBlocks(
    std::span<const u8> data, std::span<const u8> frontend_key, u64 host_features, size_t* consumed = nullptr);

}  // namespace Image

/// Connects one A64EmitX64 to a code image: records what it compiles and installs what the
/// image holds.
class CodeImageLink {
public:
    CodeImageLink(std::shared_ptr<A64::CodeImage> image, const A64::UserConfig& conf, BlockOfCode& code, A64EmitX64& emitter);

    /// On a cache miss, before compiling: installs the image's block for `location` when guest
    /// memory holds its instructions and it fits this code cache.
    std::optional<CodePtr> TryLoad(IR::LocationDescriptor location);

    /// After emitting `block` (whose near code is `entrypoint`, `size` bytes) with `capture`.
    void Record(const IR::Block& block, CodePtr entrypoint, size_t size, const BlockCapture& capture);

    /// The record of a block emitted (or installed) at these addresses. nullopt when it holds
    /// something that cannot be relocated. Exposed for tests.
    std::optional<Image::Block> BuildRecord(u64 location, u64 guest_pc, u64 guest_end, std::vector<u32> guest_code, const u8* entrypoint, size_t size, const BlockCapture& capture, std::span<const u64> ir_immediates);

    /// Whether the image holds a block for this location whose guest code is still in memory.
    bool Contains(IR::LocationDescriptor location) const;

private:
    struct Segments {
        const u8* near_begin;
        const u8* near_end;
        const u8* far_begin;
        const u8* far_end;
    };
    std::optional<Image::Target> Classify(const u8* target, const Segments& segments, const BlockCapture& capture);
    std::optional<Image::Target> ClassifyImm64(u64 value, const Segments& segments, const BlockCapture& capture, std::span<const u64> ir_immediates);
    const u8* ResolvePrelude(u64 symbol, u64 key) const;
    void BuildPreludeSymbols();
    /// Installs `block`; nullopt (and nothing installed) when it does not fit.
    std::optional<CodePtr> Install(const Image::Block& block, BlockCapture& installed);
    const u8* EnsureExitStub(u64 target);
    bool GuestCodeMatches(const Image::Block& block) const;

    std::shared_ptr<A64::CodeImage> image;
    const A64::UserConfig& conf;
    BlockOfCode& code;
    A64EmitX64& emitter;
    u64 codegen_hash;
    bool prelude_symbols_built = false;
    std::unordered_map<const void*, std::pair<u64, u64>> prelude_symbols;
    std::map<std::pair<u64, u64>, const u8*> prelude_addresses;
};

}  // namespace Dynarmic::Backend::X64

namespace Dynarmic::A64 {

struct CodeImage::Impl {
    explicit Impl(std::vector<u8> frontend_key)
            : frontend_key(std::move(frontend_key)) {}

    std::shared_ptr<const Backend::X64::Image::Block> Find(u64 codegen_hash, u64 location) const;
    /// Adds or replaces the record of the block's location.
    void Add(u64 codegen_hash, std::shared_ptr<const Backend::X64::Image::Block> block);

    std::vector<u8> frontend_key;  // guarded by mutex
    mutable std::mutex mutex;
    struct KeyHash {
        size_t operator()(const std::pair<u64, u64>& key) const noexcept {
            return size_t((key.first ^ (key.second * 0x9E37'79B9'7F4A'7C15ULL)) * 0xBF58'476D'1CE4'E5B9ULL >> 7);
        }
    };
    /// (codegen hash, location) -> record. Hashed: an image of a whole game holds millions.
    std::unordered_map<std::pair<u64, u64>, std::shared_ptr<const Backend::X64::Image::Block>, KeyHash> blocks;
    bool dirty = false;
    /// Keys recorded since the last Serialize/SerializeAdded.
    std::vector<std::pair<u64, u64>> added;

    std::atomic<u64> loaded{0};
    std::atomic<u64> recorded{0};
    std::atomic<u64> not_recordable{0};
    std::atomic<u64> guest_changed{0};
    std::atomic<u64> not_installable{0};
    std::atomic<u64> verify_mismatches{0};
};

}  // namespace Dynarmic::A64
