// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/file_sys/program_metadata.h"
#include "core/loader/nso.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"

namespace {

void Put32(std::vector<u8>& out, std::size_t offset, u32 value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
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
std::optional<VAddr> Load(const FileSys::VirtualFile& nso, bool pass_arguments = false) {
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
    return Loader::AppLoader_NSO::LoadModule(process, system, *nso, 0x8000000, pass_arguments,
                                             false);
}

// Loads `nso` into a process with arguments and reads back `size` bytes of its image from
// `offset`.
std::vector<u8> LoadIntoProcess(const FileSys::VirtualFile& nso, u64 offset, std::size_t size) {
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
    REQUIRE(process
                .LoadFromMetadata(kernel, FileSys::ProgramMetadata::GetDefault(), 0x100000, 0, 0)
                .IsSuccess());
    const VAddr base = GetInteger(process.GetEntryPoint());
    REQUIRE(Loader::AppLoader_NSO::LoadModule(process, system, *nso, base, true, true));
    std::vector<u8> bytes(size);
    process.GetMemory().ReadBlock(base + offset, bytes.data(), bytes.size());
    return bytes;
}

// Sets the program arguments for the scope.
class ProgramArguments {
public:
    explicit ProgramArguments(std::string arguments)
        : saved{Settings::values.program_args.GetValue()} {
        Settings::values.program_args.SetValue(std::move(arguments));
    }
    ~ProgramArguments() {
        Settings::values.program_args.SetValue(saved);
    }

private:
    std::string saved;
};

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

TEST_CASE("NSO argument block is sized for its arguments", "[core][loader]") {
    const auto nso = [] {
        return MakeStoredNso({{{0, 0x10, 0x10}, {0x1000, 0x10, 0x10}, {0x2000, 0x10, 0x10}}});
    };
    const auto without = Load(nso());
    REQUIRE(without);
    // Sized as the console's loader sizes it: room for the arguments twice over and 32 KiB more,
    // page aligned. Short arguments get the 0x9000 bytes they always did.
    {
        const ProgramArguments arguments{"homebrew --flag"};
        const auto with = Load(nso(), true);
        REQUIRE(with);
        CHECK(*with - *without == 0x9000);
    }
    {
        const ProgramArguments arguments{std::string(0x7FFF, 'a')};
        const auto with = Load(nso(), true);
        REQUIRE(with);
        CHECK(*with - *without == 0x19000); // AlignUp(8 + 2 * 0x7FFF + 0x8000)
    }
}

TEST_CASE("NSO starts without arguments longer than the console accepts", "[core][loader]") {
    const auto nso = [] {
        return MakeStoredNso({{{0, 0x10, 0x10}, {0x1000, 0x10, 0x10}, {0x2000, 0x10, 0x10}}});
    };
    const auto without = Load(nso());
    REQUIRE(without);
    // One byte under the limit is passed (above); at the limit, and well past the 0x9000 byte
    // block they were once copied into whatever their length, there is no block at all.
    for (const std::size_t size : {std::size_t{0x8000}, std::size_t{0x9000}}) {
        const ProgramArguments arguments{std::string(size, 'a')};
        const auto with = Load(nso(), true);
        REQUIRE(with);
        CHECK(*with == *without);
    }
}

TEST_CASE("NSO argument block holds its header and the arguments", "[core][loader]") {
    const ProgramArguments arguments{"homebrew --flag"};
    // The block starts where the last segment's bytes end: .data at 0x2000, 0x10 bytes.
    const auto block = LoadIntoProcess(
        MakeStoredNso({{{0, 0x10, 0x10}, {0x1000, 0x10, 0x10}, {0x2000, 0x10, 0x10}}}), 0x2010,
        0x40);
    Loader::NSOArgumentHeader header{};
    std::memcpy(&header, block.data(), sizeof(header));
    CHECK(header.allocated_size == 0x9000);
    CHECK(header.actual_size == 15);
    CHECK(std::string(reinterpret_cast<const char*>(block.data()) + sizeof(header), 15) ==
          "homebrew --flag");
    CHECK(block[sizeof(header) + 15] == 0);
}
