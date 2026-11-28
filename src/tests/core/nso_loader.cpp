// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <memory>
#include <vector>


#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/loader/nso.h"
#include "tests/core/system_test_context.h"

namespace {

void Put32(std::vector<u8>& out, std::size_t offset, u32 value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

u32 Get32(const std::vector<u8>& in, std::size_t offset) {
    u32 value;
    std::memcpy(&value, in.data() + offset, sizeof(value));
    return value;
}

// An NSO whose text segment is LZ4-compressed as `text` and decompresses to `text_size` bytes,
// with stored 0x10-byte rodata and data segments.
FileSys::VirtualFile MakeNso(const std::vector<u8>& text, u32 text_size) {
    std::vector<u8> bytes(0x100);
    Put32(bytes, 0, Common::MakeMagic('N', 'S', 'O', '0'));
    Put32(bytes, 0x0C, 1); // text compressed
    const u32 text_offset = 0x100;
    const u32 ro_offset = text_offset + static_cast<u32>(text.size());
    const u32 data_offset = ro_offset + 0x10;
    const u32 segments[3][3] = {
        {text_offset, 0, text_size}, {ro_offset, 0x1000, 0x10}, {data_offset, 0x2000, 0x10}};
    for (std::size_t i = 0; i < 3; ++i) {
        Put32(bytes, 0x10 + i * 0x10, segments[i][0]);
        Put32(bytes, 0x14 + i * 0x10, segments[i][1]);
        Put32(bytes, 0x18 + i * 0x10, segments[i][2]);
    }
    Put32(bytes, 0x60, static_cast<u32>(text.size()));
    Put32(bytes, 0x64, 0x10);
    Put32(bytes, 0x68, 0x10);
    bytes.insert(bytes.end(), text.begin(), text.end());
    bytes.resize(bytes.size() + 0x20, 0x5A);
    return std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), "main");
}

struct Segment {
    u32 location;
    u32 size;
    u32 stored; // bytes in the file
};

// An NSO with three stored (uncompressed) segments.
FileSys::VirtualFile MakeStoredNso(const std::array<Segment, 3>& segments) {
    std::vector<u8> bytes(0x100);
    Put32(bytes, 0, Common::MakeMagic('N', 'S', 'O', '0'));
    u32 offset = 0x100;
    for (std::size_t i = 0; i < 3; ++i) {
        Put32(bytes, 0x10 + i * 0x10, offset);
        Put32(bytes, 0x14 + i * 0x10, segments[i].location);
        Put32(bytes, 0x18 + i * 0x10, segments[i].size);
        Put32(bytes, 0x60 + i * 4, segments[i].stored);
        offset += segments[i].stored;
    }
    bytes.resize(offset, 0x5A);
    return std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), "main");
}

// Loads `nso` without mapping it into the process.
std::optional<VAddr> Load(const FileSys::VirtualFile& nso) {
    ScopedSystemTestContext environment;
    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    Kernel::KThread bootstrap{kernel};
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, &bootstrap, nullptr).IsSuccess());
    kernel.SetCurrentEmuThread(&bootstrap);
    kernel.Initialize();
    SCOPE_EXIT {
        system.CoreTiming().ClearPendingEvents();
        kernel.Shutdown();
        kernel.SetCurrentEmuThread(nullptr);
        kernel.UnregisterKernelObject(&bootstrap);
    };
    Kernel::KProcess process{kernel};
    Kernel::KAutoObject::Create(&process);
    SCOPE_EXIT {
        kernel.UnregisterKernelObject(&process);
    };
    return Loader::AppLoader_NSO::LoadModule(process, system, *nso, 0x8000000, false, false);
}

} // Anonymous namespace

TEST_CASE("NSO with an LZ4 text segment loads", "[core][loader]") {
    // One LZ4 sequence of 8 literals: the token's high nibble is the literal count.
    const std::vector<u8> text{0x80, 'N', 'S', 'O', 't', 'e', 'x', 't', '!'};
    REQUIRE(Load(MakeNso(text, 8)).has_value());
}

TEST_CASE("NSO rejects an LZ4 segment that does not decompress to its size", "[core][loader]") {
    // Both used to load with the previous buffer contents as the text segment.
    SECTION("corrupt stream") {
        const std::vector<u8> text(9, 0xFF);
        CHECK_FALSE(Load(MakeNso(text, 8)).has_value());
    }
    SECTION("stream shorter than the segment") {
        const std::vector<u8> text{0x80, 'N', 'S', 'O', 't', 'e', 'x', 't', '!'};
        CHECK_FALSE(Load(MakeNso(text, 0x40)).has_value());
    }
}

TEST_CASE("NSO image covers a segment that ends after the last one", "[core][loader]") {
    // The image used to be sized by the data segment, the last in the header, so the text segment
    // placed after it was copied past the end of the image.
    const auto end = Load(MakeStoredNso({{{0x3000, 0x10, 0x10}, {0x1000, 0x10, 0x10},
                                          {0x2000, 0x10, 0x10}}}));
    REQUIRE(end.has_value());
    CHECK(*end - 0x8000000 >= 0x3010);
}

TEST_CASE("NSO rejects a stored segment larger than its data", "[core][loader]") {
    // 0x40 bytes were copied from the 0x10 read for the text segment.
    CHECK_FALSE(Load(MakeStoredNso({{{0, 0x40, 0x10}, {0x1000, 0x10, 0x10},
                                     {0x2000, 0x10, 0x10}}}))
                    .has_value());
}

TEST_CASE("NSO rejects segments that end past a 32-bit image", "[core][loader]") {
    // The review of #1920: text at 0xfffffff0 with 0x20 bytes ends at 0x100000010, which the
    // image's u32 size arithmetic wrapped to 0x10 after allocating over 4 GiB.
    CHECK_FALSE(Load(MakeStoredNso({{{0xfffffff0, 0x20, 0x20}, {0x1000, 0x10, 0x10},
                                     {0x2000, 0x10, 0x10}}}))
                    .has_value());
}

namespace {

// The bytes of an NSO with three stored segments, whose stored sizes the test may change after.
std::vector<u8> StoredNsoBytes(const std::array<Segment, 3>& segments) {
    const auto file = MakeStoredNso(segments);
    return file->ReadAllBytes();
}

FileSys::VirtualFile AsFile(std::vector<u8> bytes) {
    return std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), "main");
}

} // Anonymous namespace

TEST_CASE("NSO loads raw, empty and .bss segments", "[core][loader]") {
    SECTION("raw") {
        CHECK(Load(MakeStoredNso({{{0, 0x10, 0x10}, {0x1000, 0x10, 0x10}, {0x2000, 0x10, 0x10}}}))
                  .has_value());
    }
    SECTION("an empty segment") {
        CHECK(Load(MakeStoredNso({{{0, 0x10, 0x10}, {0x1000, 0, 0}, {0x2000, 0x10, 0x10}}}))
                  .has_value());
    }
    SECTION(".bss after .data") {
        auto bytes = StoredNsoBytes({{{0, 0x10, 0x10}, {0x1000, 0x10, 0x10}, {0x2000, 0x10, 0x10}}});
        Put32(bytes, 0x3C, 0x4000);
        const auto end = Load(AsFile(std::move(bytes)));
        REQUIRE(end.has_value());
        CHECK(*end - 0x8000000 >= 0x2010 + 0x4000);
    }
}

TEST_CASE("NSO rejects a segment past the end of the file", "[core][loader]") {
    // Its stored size runs past the file's end, by a little and by far.
    for (const u32 stored : {0x11u, 0x7FFFFFFFu}) {
        auto bytes = StoredNsoBytes({{{0, 0x10, 0x10}, {0x1000, 0x10, 0x10}, {0x2000, 0x10, 0x10}}});
        Put32(bytes, 0x68, stored);
        CHECK_FALSE(Load(AsFile(std::move(bytes))).has_value());
    }
}

TEST_CASE("NSO rejects an LZ4 segment larger than its compressed bytes can fill",
          "[core][loader]") {
    // LZ4 makes at most 255 bytes of each byte it reads.
    const std::vector<u8> text{0x80, 'N', 'S', 'O', 't', 'e', 'x', 't', '!'};
    CHECK_FALSE(Load(MakeNso(text, 255 * 9 + 1)).has_value());
}

TEST_CASE("NSO rejects an image no process can hold", "[core][loader]") {
    // Inside a 32-bit image, but past the largest pool of the 4 GiB layout, which
    // ScopedSystemTestContext (in Load) sets for the test and restores after: its application pool
    // is 3285 MiB. This one used to load, allocating its whole image.
    CHECK_FALSE(Load(MakeStoredNso({{{0, 0x10, 0x10}, {0x1000, 0x10, 0x10},
                                     {0xF0000000, 0x10, 0x10}}}))
                    .has_value());
}

TEST_CASE("NSO keeps the rule for an empty LZ4 segment", "[core][loader]") {
    // As before: an empty segment decompresses from a stream of one empty sequence, and with no
    // stored bytes at all it is refused.
    CHECK(Load(MakeNso({0x00}, 0)).has_value());
    CHECK_FALSE(Load(MakeNso({}, 0)).has_value());
}

TEST_CASE("NSO keeps the rule for an empty LZ4 segment in an empty image", "[core][loader]") {
    // Every segment empty and at 0, with no .bss and no arguments, so the image has no storage at
    // all: lz4 was handed a null destination, which UBSan reports inside it. With a .bss the image
    // is reserved before the segments are read, and this would no longer reach the null.
    const auto empty_image = [](const std::vector<u8>& text) {
        auto bytes = MakeNso(text, 0)->ReadAllBytes();
        for (const std::size_t field : {0x24, 0x28, 0x34, 0x38, 0x64, 0x68}) {
            Put32(bytes, field, 0); // .rodata's and .data's location, size and stored size
        }
        return AsFile(std::move(bytes));
    };
    CHECK(Load(empty_image({0x00})).has_value());
    CHECK_FALSE(Load(empty_image({})).has_value());
}


TEST_CASE("NSO loads an empty segment whose offset lies past the file", "[core][loader]") {
    // It reads no bytes, as it always did, wherever its offset points.
    auto bytes = StoredNsoBytes({{{0, 0x10, 0x10}, {0x1000, 0, 0}, {0x2000, 0x10, 0x10}}});
    Put32(bytes, 0x20, 0x7FFFFFF0); // .rodata's file offset
    CHECK(Load(AsFile(std::move(bytes))).has_value());
}

TEST_CASE("NSO refuses a segment larger than the console loads", "[core][loader]") {
    // The console's loader refuses a segment larger than INT32_MAX; so does this one, before
    // anything is allocated for it, though a ZBIC segment that large with a small stream used to
    // load.
    const std::vector<u8> text{0x80, 'N', 'S', 'O', 't', 'e', 'x', 't', '!'};
    auto bytes = MakeNso(text, 8)->ReadAllBytes();
    Put32(bytes, 0x0C, Get32(bytes, 0x0C) | (1u << 7)); // ZBIC, which has no ratio bound
    Put32(bytes, 0x18, 0x80000000);                       // .text's size
    CHECK_FALSE(Load(AsFile(std::move(bytes))).has_value());
}
