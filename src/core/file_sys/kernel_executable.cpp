// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <limits>

#include "common/alignment.h"
#include "common/logging.h"
#include "common/string_util.h"
#include "core/file_sys/kernel_executable.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/loader/loader.h"

namespace FileSys {

constexpr u32 INI_MAX_KIPS = 0x50;

namespace {
bool DecompressBLZ(std::vector<u8>& data) {
    if (data.size() < 0xC)
        return {};

    const auto data_size = data.size() - 0xC;

    u32 compressed_size{};
    u32 init_index{};
    u32 additional_size{};
    std::memcpy(&compressed_size, data.data() + data_size, sizeof(u32));
    std::memcpy(&init_index, data.data() + data_size + 0x4, sizeof(u32));
    std::memcpy(&additional_size, data.data() + data_size + 0x8, sizeof(u32));

    // The footer's sizes come from the file: keep every index inside the section.
    if (compressed_size > data.size() || init_index > compressed_size) {
        return false;
    }

    // Added in 64 bits: two u32s from the file can wrap. BLZ turns at most 17 input bytes into
    // 144 output bytes, so a valid stream never asks for more than nine times its size.
    const u64 out_size = u64{compressed_size} + additional_size;
    if (out_size > u64{compressed_size} * 9 + 144) {
        return false;
    }

    const auto start_offset = data.size() - compressed_size;
    data.resize(out_size + start_offset);

    std::size_t index = compressed_size - init_index;
    std::size_t out_index = out_size;

    while (out_index > 0) {
        if (index < 1) {
            return false;
        }
        --index;
        auto control = data[index + start_offset];
        for (size_t i = 0; i < 8; ++i) {
            if (((control << i) & 0x80) > 0) {
                if (index < 2) {
                    return false;
                }
                index -= 2;
                std::size_t segment_offset =
                    data[index + start_offset] | data[index + start_offset + 1] << 8;
                std::size_t segment_size = ((segment_offset >> 12) & 0xF) + 3;
                segment_offset &= 0xFFF;
                segment_offset += 3;

                if (out_index < segment_size)
                    segment_size = out_index;

                if (out_index < segment_size) {
                    return false;
                }

                out_index -= segment_size;

                for (size_t j = 0; j < segment_size; ++j) {
                    if (out_index + j + segment_offset + start_offset >= data.size()) {
                        return false;
                    }
                    data[out_index + j + start_offset] =
                        data[out_index + j + segment_offset + start_offset];
                }
            } else {
                if (out_index < 1 || index < 1) {
                    return false;
                }
                --out_index;
                --index;
                data[out_index + start_offset] = data[index + start_offset];
            }

            if (out_index == 0)
                break;
        }
    }

    return true;
}

constexpr u64 KipPageSize = 0x1000;

// The layout the kernel accepts for an initial process (Atmosphere's
// KInitialProcessReader::MakeCreateProcessParameter): .text at 0, then .rodata, .data and .bss in
// that order, each of the first three starting on a page and ending before the next begins, .bss
// after .data. The header's fields are 32-bit; every sum here is 64-bit, so none wraps. The image
// must also fit what a code set segment, whose size is a u32, can describe: real KIPs are a few
// megabytes. Sets the image size and the page aligned extent of .text, .rodata and .data, the last
// running to the end of the image over .bss.
bool ValidLayout(const KIPHeader& header, u64& image_size, std::array<KIPSegment, 3>& segments) {
    const u64 rx = header.sections[0].offset;
    const u64 rx_size = header.sections[0].decompressed_size;
    const u64 ro = header.sections[1].offset;
    const u64 ro_size = header.sections[1].decompressed_size;
    const u64 rw = header.sections[2].offset;
    const u64 rw_size = header.sections[2].decompressed_size;
    const u64 bss = header.sections[3].offset;
    const u64 bss_size = header.sections[3].decompressed_size;
    if (rx != 0 || ro % KipPageSize != 0 || rw % KipPageSize != 0) {
        return false;
    }
    const u64 rx_extent = Common::AlignUp(rx_size, KipPageSize);
    const u64 ro_extent = Common::AlignUp(ro_size, KipPageSize);
    if (rx + rx_extent > ro || ro + ro_extent > rw || rw + rw_size > bss) {
        return false;
    }
    const u64 end = Common::AlignUp(bss_size > 0 ? bss + bss_size : rw + rw_size, KipPageSize);
    if (end > std::numeric_limits<u32>::max()) {
        return false;
    }
    image_size = end;
    segments = {{{rx, rx_extent}, {ro, ro_extent}, {rw, end - rw}}};
    return true;
}
} // Anonymous namespace

KIP::KIP(const VirtualFile& file) : status(Loader::ResultStatus::Success) {
    if (file == nullptr) {
        status = Loader::ResultStatus::ErrorNullFile;
        return;
    }

    if (file->GetSize() < sizeof(KIPHeader) || file->ReadObject(&header) != sizeof(KIPHeader)) {
        status = Loader::ResultStatus::ErrorBadKIPHeader;
        return;
    }

    if (header.magic != Common::MakeMagic('K', 'I', 'P', '1')) {
        status = Loader::ResultStatus::ErrorBadKIPHeader;
        return;
    }

    // A rejected KIP keeps nothing it read or decompressed.
    const auto reject = [this](Loader::ResultStatus result) {
        status = result;
        decompressed_sections = {};
        segments = {};
        image_size = 0;
    };

    // The six sections' stored bytes follow the header back to back. Their sum, in 64-bit
    // arithmetic so six u32s cannot wrap, is where the next KIP of an INI1 starts; a file shorter
    // than that is a truncated KIP, and ReadBytes would stop at its end.
    u64 end = sizeof(KIPHeader);
    for (const auto& section : header.sections) {
        end += section.compressed_size;
    }
    if (end > file->GetSize()) {
        reject(Loader::ResultStatus::ErrorBadKIPHeader);
        return;
    }
    size_known = true;

    // From the header alone, before anything is read.
    if (!ValidLayout(header, image_size, segments)) {
        reject(Loader::ResultStatus::ErrorBadKIPHeader);
        return;
    }

    // As the kernel's loader (Atmosphere's KInitialProcessReader) and hactool do: the flags say
    // which sections are BLZ compressed; a section stored raw is copied, and whatever its size has
    // beyond its stored bytes is zero, which the loader's image already is. So no section is ever
    // allocated at the size its header gives: that size comes from the file, and a damaged .kip
    // claiming gigabytes for one cost that much as soon as the game list read it. Only .text,
    // .rodata and .data are read: .bss and the two unused sections hold no file data that is used.
    u64 offset = sizeof(KIPHeader);
    for (std::size_t i = 0; i < decompressed_sections.size(); ++i) {
        auto& section = decompressed_sections[i];
        const bool compressed = (header.flags & (1U << i)) != 0;
        // A section stored raw is its bytes as they are: more of them than its size is known
        // from the header, without reading them.
        if (!compressed &&
            header.sections[i].compressed_size > header.sections[i].decompressed_size) {
            reject(Loader::ResultStatus::ErrorBadKIPHeader);
            return;
        }
        section = file->ReadBytes(header.sections[i].compressed_size, offset);
        offset += header.sections[i].compressed_size;
        if (section.size() != header.sections[i].compressed_size) {
            reject(Loader::ResultStatus::ErrorBadKIPHeader);
            return;
        }
        // A compressed section with no stored bytes has nothing to decompress: all zero.
        if (compressed && !section.empty() && !DecompressBLZ(section)) {
            reject(Loader::ResultStatus::ErrorBLZDecompressionFailed);
            return;
        }
        // Raw or decompressed, the bytes must fit the section the layout check placed.
        if (section.size() > header.sections[i].decompressed_size) {
            reject(Loader::ResultStatus::ErrorBadKIPHeader);
            return;
        }
    }
}

Loader::ResultStatus KIP::GetStatus() const {
    return status;
}

std::string KIP::GetName() const {
    return Common::StringFromFixedZeroTerminatedBuffer(header.name.data(), header.name.size());
}

u64 KIP::GetTitleID() const {
    return header.title_id;
}

std::vector<u8> KIP::GetSectionDecompressed(u8 index) const {
    return index < decompressed_sections.size() ? decompressed_sections[index] : std::vector<u8>{};
}

const std::array<KIPSegment, 3>& KIP::GetSegments() const {
    return segments;
}

bool KIP::Is64Bit() const {
    return (header.flags & 0x8) != 0;
}

bool KIP::Is39BitAddressSpace() const {
    return (header.flags & 0x10) != 0;
}

bool KIP::IsService() const {
    return (header.flags & 0x20) != 0;
}

std::vector<u32> KIP::GetKernelCapabilities() const {
    return std::vector<u32>(header.capabilities.begin(), header.capabilities.end());
}

s32 KIP::GetMainThreadPriority() const {
    return static_cast<s32>(header.main_thread_priority);
}

u32 KIP::GetMainThreadStackSize() const {
    return header.sections[1].attribute;
}

u32 KIP::GetMainThreadCpuCore() const {
    return header.default_core;
}

std::span<const u8> KIP::GetTextSection() const {
    return decompressed_sections[0];
}

std::span<const u8> KIP::GetRODataSection() const {
    return decompressed_sections[1];
}

std::span<const u8> KIP::GetDataSection() const {
    return decompressed_sections[2];
}

u32 KIP::GetTextOffset() const {
    return header.sections[0].offset;
}

u32 KIP::GetRODataOffset() const {
    return header.sections[1].offset;
}

u32 KIP::GetDataOffset() const {
    return header.sections[2].offset;
}

u32 KIP::GetSectionSize(u8 index) const {
    return index < 3 ? header.sections[index].decompressed_size : 0;
}

u64 KIP::GetImageSize() const {
    return image_size;
}

bool KIP::IsSizeKnown() const {
    return size_known;
}

u64 KIP::GetSize() const {
    u64 size = sizeof(KIPHeader);
    for (const auto& section : header.sections) {
        size += section.compressed_size;
    }
    return size;
}

u32 KIP::GetBSSSize() const {
    return header.sections[3].decompressed_size;
}

u32 KIP::GetBSSOffset() const {
    return header.sections[3].offset;
}

INI::INI(const VirtualFile& file) : status(Loader::ResultStatus::Success) {
    if (file->GetSize() < sizeof(INIHeader) || file->ReadObject(&header) != sizeof(INIHeader)) {
        status = Loader::ResultStatus::ErrorBadINIHeader;
        return;
    }

    if (header.magic != Common::MakeMagic('I', 'N', 'I', '1')) {
        status = Loader::ResultStatus::ErrorBadINIHeader;
        return;
    }

    if (header.kip_count > INI_MAX_KIPS) {
        status = Loader::ResultStatus::ErrorINITooManyKIPs;
        return;
    }

    // The KIPs follow the header back to back. A KIP whose header or stored sections the file
    // does not hold gives no reliable size, so the ones after it cannot be found. One that is
    // whole but rejected (a layout the kernel would refuse, a section that does not decompress)
    // is skipped: stopping there lost every KIP after it, FS and spl among them.
    u64 offset = sizeof(INIHeader);
    for (std::size_t i = 0; i < header.kip_count && offset < file->GetSize(); ++i) {
        const auto kip_file =
            std::make_shared<OffsetVfsFile>(file, file->GetSize() - offset, offset);
        KIP kip(kip_file);
        if (!kip.IsSizeKnown()) {
            break;
        }
        offset += kip.GetSize();
        if (kip.GetStatus() != Loader::ResultStatus::Success) {
            LOG_WARNING(Loader, "INI1: KIP {} ({}) skipped: {}", i, kip.GetName(),
                        Loader::GetResultStatusString(kip.GetStatus()));
            continue;
        }
        kips.push_back(std::move(kip));
    }
}

Loader::ResultStatus INI::GetStatus() const {
    return status;
}

const std::vector<KIP>& INI::GetKIPs() const {
    return kips;
}

} // namespace FileSys
