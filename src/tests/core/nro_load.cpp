// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <memory>
#include <string>
#include <string_view>
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
#include "core/loader/nro.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"

namespace {

void Put32(std::vector<u8>& out, std::size_t offset, u32 value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

void PutString(std::vector<u8>& out, std::size_t offset, std::string_view text) {
    std::memcpy(out.data() + offset, text.data(), text.size());
}

// An NRO of 0x2800 bytes, not page aligned: .text, .rodata and .data of a page each, the last
// one ending at the NRO's size, a marker at the start of each and at the end of the NRO, and
// 0x100 bytes after it that are not part of the NRO.
std::vector<u8> MakeNro(u32 bss_size) {
    std::vector<u8> bytes(0x2900, 0x5A);
    std::fill(bytes.begin(), bytes.begin() + 0x2800, u8{0});
    Put32(bytes, 0x10, Common::MakeMagic('N', 'R', 'O', '0'));
    Put32(bytes, 0x18, 0x2800);
    const u32 segments[3][2] = {{0, 0x1000}, {0x1000, 0x1000}, {0x2000, 0x800}};
    for (std::size_t i = 0; i < 3; ++i) {
        Put32(bytes, 0x20 + i * 8, segments[i][0]);
        Put32(bytes, 0x24 + i * 8, segments[i][1]);
    }
    Put32(bytes, 0x38, bss_size);
    PutString(bytes, 0x1000, "rodata");
    PutString(bytes, 0x2000, "data");
    PutString(bytes, 0x27FC, "tail");
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

// Loads `nro` into a new process of an initialised kernel and reads back `size` bytes of its
// image; empty if it did not load.
std::vector<u8> Load(std::vector<u8> nro, std::size_t size) {
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
    Loader::AppLoader_NRO loader{
        std::make_shared<FileSys::VectorVfsFile>(std::move(nro), "test.nro")};
    if (loader.Load(process, system).first != Loader::ResultStatus::Success) {
        return {};
    }
    std::vector<u8> image(size);
    process.GetMemory().ReadBlock(process.GetEntryPoint(), image.data(), image.size());
    return image;
}

std::string_view At(const std::vector<u8>& image, std::size_t offset, std::size_t size) {
    return {reinterpret_cast<const char*>(image.data()) + offset, size};
}

} // Anonymous namespace

TEST_CASE("NRO whose size is not page aligned loads with a zero tail", "[core][loader]") {
    const ProgramArguments arguments{""};
    const auto image = Load(MakeNro(0x1000), 0x4000);
    REQUIRE(image.size() == 0x4000);
    CHECK(At(image, 0x1000, 6) == "rodata");
    CHECK(At(image, 0x2000, 4) == "data");
    CHECK(At(image, 0x27FC, 4) == "tail");
    // The bytes after the NRO's size are not copied: the page's tail and .bss are zero.
    for (std::size_t offset = 0x2800; offset < 0x4000; ++offset) {
        if (image[offset] != 0) {
            FAIL("non-zero byte at " << offset);
        }
    }
}

TEST_CASE("NRO loads with program arguments after its image", "[core][loader]") {
    const ProgramArguments arguments{"--flag"};
    // The image is 0x3000 bytes and .bss 0x1000; the configuration's four 0x18 byte entries
    // come next, then argv.
    constexpr std::size_t argv_offset = 0x4000 + 4 * 0x18;
    const auto image = Load(MakeNro(0x1000), argv_offset + 0x20);
    REQUIRE(!image.empty());
    CHECK(At(image, argv_offset, 16) == std::string_view("homebrew --flag\0", 16));
}

TEST_CASE("NRO loader refuses a damaged NRO", "[core][loader]") {
    const ProgramArguments arguments{""};
    auto nro = MakeNro(0);
    Put32(nro, 0x18, 0x10000); // a size larger than the file
    CHECK(Load(std::move(nro), 0x10).empty());
}
