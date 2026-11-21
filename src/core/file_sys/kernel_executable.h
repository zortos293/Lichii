// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>
#include <string>
#include <vector>

#include "common/common_funcs.h"
#include "common/common_types.h"
#include "common/swap.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace Loader {
enum class ResultStatus : u16;
}

namespace FileSys {

struct KIPSectionHeader {
    u32_le offset;
    u32_le decompressed_size;
    u32_le compressed_size;
    u32_le attribute;
};
static_assert(sizeof(KIPSectionHeader) == 0x10, "KIPSectionHeader has incorrect size.");

struct KIPHeader {
    u32_le magic;
    std::array<char, 0xC> name;
    u64_le title_id;
    u32_le process_category;
    u8 main_thread_priority;
    u8 default_core;
    INSERT_PADDING_BYTES(1);
    u8 flags;
    std::array<KIPSectionHeader, 6> sections;
    std::array<u32, 0x20> capabilities;
};
static_assert(sizeof(KIPHeader) == 0x100, "KIPHeader has incorrect size.");

struct INIHeader {
    u32_le magic;
    u32_le size;
    u32_le kip_count;
    INSERT_PADDING_BYTES(0x4);
};
static_assert(sizeof(INIHeader) == 0x10, "INIHeader has incorrect size.");

/// Where a segment of a KIP's image lies and how far it reaches, page aligned.
struct KIPSegment {
    u64 offset;
    u64 size;
};

// Kernel Internal Process
class KIP {
public:
    explicit KIP(const VirtualFile& file);

    Loader::ResultStatus GetStatus() const;

    std::string GetName() const;
    u64 GetTitleID() const;
    std::vector<u8> GetSectionDecompressed(u8 index) const;

    // Executable Flags
    bool Is64Bit() const;
    bool Is39BitAddressSpace() const;
    bool IsService() const;

    std::vector<u32> GetKernelCapabilities() const;

    s32 GetMainThreadPriority() const;
    u32 GetMainThreadStackSize() const;
    u32 GetMainThreadCpuCore() const;

    std::span<const u8> GetTextSection() const;
    std::span<const u8> GetRODataSection() const;
    std::span<const u8> GetDataSection() const;

    u32 GetTextOffset() const;
    u32 GetRODataOffset() const;
    u32 GetDataOffset() const;

    u32 GetBSSSize() const;
    u32 GetBSSOffset() const;

    /// The size .text, .rodata or .data (0 to 2) has in memory, from the header. The section's
    /// bytes may be fewer (a section stored raw with fewer bytes than its size): the rest of the
    /// segment is zero. Only what the file stores or decompresses to is kept.
    u32 GetSectionSize(u8 index) const;

    /// The size of the process image, .text to the end of .bss, page aligned, at most 4 GiB. With
    /// Success, the sections lie inside it in order, as the kernel requires (see the constructor).
    u64 GetImageSize() const;

    /// The extents of the .text, .rodata and .data segments in the image, checked by the
    /// constructor: page aligned, in order, each holding its section's bytes, the last running to
    /// the end of the image over .bss. A segment may be empty. All zero unless Success.
    const std::array<KIPSegment, 3>& GetSegments() const;

    // The header and the stored sections, which is where the next KIP of an INI1 starts.
    u64 GetSize() const;

    /// Whether GetSize() can be relied on: the header was read and the file holds every stored
    /// section, whatever else is wrong with the KIP.
    bool IsSizeKnown() const;

private:
    Loader::ResultStatus status;

    KIPHeader header{};
    std::array<std::vector<u8>, 3> decompressed_sections;
    std::array<KIPSegment, 3> segments{};
    u64 image_size{};
    bool size_known{};
};

class INI {
public:
    explicit INI(const VirtualFile& file);

    Loader::ResultStatus GetStatus() const;

    const std::vector<KIP>& GetKIPs() const;

private:
    Loader::ResultStatus status;

    INIHeader header{};
    std::vector<KIP> kips;
};

} // namespace FileSys
