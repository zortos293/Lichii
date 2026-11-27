// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>

#include <minizip/unzip.h>
#include <minizip/zip.h>
#include <zlib.h>

#include "frontend_common/cloud_sync/zip_archive.h"

namespace FrontendCommon::CloudSync {

namespace {

// minizip reads and writes the zip through these callbacks, so it never opens a file by name
// (minizip's fopen takes ANSI paths on Windows).
struct MemoryStream {
    std::vector<u8> bytes;
    std::size_t position = 0;
};

voidpf ZCALLBACK OpenStream(voidpf opaque, const void*, int) {
    return opaque;
}

uLong ZCALLBACK ReadStream(voidpf, voidpf stream, void* buffer, uLong size) {
    auto& memory = *static_cast<MemoryStream*>(stream);
    const std::size_t count = std::min<std::size_t>(size, memory.bytes.size() - memory.position);
    std::memcpy(buffer, memory.bytes.data() + memory.position, count);
    memory.position += count;
    return static_cast<uLong>(count);
}

uLong ZCALLBACK WriteStream(voidpf, voidpf stream, const void* buffer, uLong size) {
    auto& memory = *static_cast<MemoryStream*>(stream);
    if (memory.position + size > memory.bytes.size()) {
        memory.bytes.resize(memory.position + size);
    }
    std::memcpy(memory.bytes.data() + memory.position, buffer, size);
    memory.position += size;
    return size;
}

ZPOS64_T ZCALLBACK TellStream(voidpf, voidpf stream) {
    return static_cast<MemoryStream*>(stream)->position;
}

long ZCALLBACK SeekStream(voidpf, voidpf stream, ZPOS64_T offset, int origin) {
    auto& memory = *static_cast<MemoryStream*>(stream);
    u64 base = 0;
    switch (origin) {
    case ZLIB_FILEFUNC_SEEK_CUR:
        base = memory.position;
        break;
    case ZLIB_FILEFUNC_SEEK_END:
        base = memory.bytes.size();
        break;
    default:
        break;
    }
    if (base + offset > memory.bytes.size()) {
        return -1;
    }
    memory.position = static_cast<std::size_t>(base + offset);
    return 0;
}

int ZCALLBACK CloseStream(voidpf, voidpf) {
    return 0;
}

int ZCALLBACK StreamError(voidpf, voidpf) {
    return 0;
}

zlib_filefunc64_def MemoryFunctions(MemoryStream& stream) {
    return {OpenStream, ReadStream,  WriteStream, TellStream,
            SeekStream, CloseStream, StreamError, &stream};
}

// minizip asks for a name to open; the callbacks ignore it.
constexpr char StreamName[] = "memory";

bool AddEntry(zipFile zip, const ZipEntry& entry) {
    // 1980-01-01, the earliest time a zip can hold.
    zip_fileinfo info{};
    info.tmz_date.tm_year = 1980;
    info.tmz_date.tm_mday = 1;
    const int zip64 = NeedsZip64(entry.data.size()) ? 1 : 0;
    if (zipOpenNewFileInZip64(zip, entry.name.c_str(), &info, nullptr, 0, nullptr, 0, nullptr,
                              Z_DEFLATED, Z_DEFAULT_COMPRESSION, zip64) != ZIP_OK) {
        return false;
    }
    // zipWriteInFileInZip takes an unsigned length.
    constexpr std::size_t Chunk = 1 << 30;
    bool written = true;
    for (std::size_t offset = 0; written && offset < entry.data.size(); offset += Chunk) {
        const auto size = static_cast<unsigned>(std::min(Chunk, entry.data.size() - offset));
        written = zipWriteInFileInZip(zip, entry.data.data() + offset, size) == ZIP_OK;
    }
    return zipCloseFileInZip(zip) == ZIP_OK && written;
}

std::optional<ZipEntry> ReadCurrentEntry(unzFile zip) {
    unz_file_info64 info{};
    if (unzGetCurrentFileInfo64(zip, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK) {
        return std::nullopt;
    }
    ZipEntry entry;
    entry.name.resize(info.size_filename);
    if (unzGetCurrentFileInfo64(zip, nullptr, entry.name.data(),
                                static_cast<uLong>(entry.name.size()), nullptr, 0, nullptr,
                                0) != UNZ_OK ||
        unzOpenCurrentFile(zip) != UNZ_OK) {
        return std::nullopt;
    }
    // Read in chunks rather than trusting the size the header claims.
    std::array<u8, 64 * 1024> chunk;
    int read = 0;
    while ((read = unzReadCurrentFile(zip, chunk.data(), static_cast<unsigned>(chunk.size()))) >
           0) {
        entry.data.insert(entry.data.end(), chunk.begin(), chunk.begin() + read);
    }
    // Closing checks the CRC once the whole entry was read.
    if (unzCloseCurrentFile(zip) != UNZ_OK || read < 0 ||
        entry.data.size() != info.uncompressed_size) {
        return std::nullopt;
    }
    return entry;
}

} // namespace

bool NeedsZip64(u64 size) {
    constexpr u64 Max32 = 0xFFFFFFFF;
    if constexpr (sizeof(uLong) >= sizeof(u64)) {
        return compressBound(static_cast<uLong>(size)) >= Max32;
    } else {
        // compressBound's uLong is 32 bits (Windows) and would overflow here. Its bound adds less
        // than 1/2048 of the size.
        return size >= Max32 - Max32 / 2048;
    }
}

std::optional<std::vector<u8>> WriteZip(std::span<const ZipEntry> entries) {
    MemoryStream stream;
    auto functions = MemoryFunctions(stream);
    const zipFile zip = zipOpen2_64(StreamName, APPEND_STATUS_CREATE, nullptr, &functions);
    if (zip == nullptr) {
        return std::nullopt;
    }
    const bool added = std::ranges::all_of(
        entries, [zip](const ZipEntry& entry) { return AddEntry(zip, entry); });
    if (zipClose(zip, nullptr) != ZIP_OK || !added) {
        return std::nullopt;
    }
    return std::move(stream.bytes);
}

std::optional<std::vector<ZipEntry>> ReadZip(std::span<const u8> bytes) {
    MemoryStream stream{.bytes{bytes.begin(), bytes.end()}};
    auto functions = MemoryFunctions(stream);
    const unzFile zip = unzOpen2_64(StreamName, &functions);
    if (zip == nullptr) {
        return std::nullopt;
    }
    unz_global_info64 info{};
    std::vector<ZipEntry> entries;
    // unzGoToFirstFile fails on a zip without entries, so count them instead.
    bool ok = unzGetGlobalInfo64(zip, &info) == UNZ_OK;
    for (ZPOS64_T i = 0; ok && i < info.number_entry; ++i) {
        auto entry = (i == 0 ? unzGoToFirstFile(zip) : unzGoToNextFile(zip)) == UNZ_OK
                         ? ReadCurrentEntry(zip)
                         : std::nullopt;
        ok = entry.has_value();
        if (ok) {
            entries.push_back(std::move(*entry));
        }
    }
    unzClose(zip);
    if (!ok) {
        return std::nullopt;
    }
    return entries;
}

} // namespace FrontendCommon::CloudSync
