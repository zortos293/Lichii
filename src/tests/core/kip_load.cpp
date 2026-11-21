// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <memory>
#include <string_view>
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
#include "core/loader/kip.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"

namespace {

void Put32(std::vector<u8>& out, std::size_t offset, u32 value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

struct Section {
    u32 offset;
    u32 size;
    std::string_view stored;
};

// A 64-bit KIP whose .text, .rodata and .data are stored raw.
FileSys::VirtualFile MakeKip(const Section& text, const Section& rodata, const Section& data,
                             u32 bss_offset, u32 bss_size) {
    std::vector<u8> bytes(0x100);
    Put32(bytes, 0, Common::MakeMagic('K', 'I', 'P', '1'));
    bytes[0x1F] = 0x8 | 0x10; // 64-bit, 39-bit address space
    const Section* sections[] = {&text, &rodata, &data};
    for (std::size_t i = 0; i < 3; ++i) {
        Put32(bytes, 0x20 + i * 0x10, sections[i]->offset);
        Put32(bytes, 0x24 + i * 0x10, sections[i]->size);
        Put32(bytes, 0x28 + i * 0x10, static_cast<u32>(sections[i]->stored.size()));
    }
    Put32(bytes, 0x50, bss_offset);
    Put32(bytes, 0x54, bss_size);
    for (const auto* section : sections) {
        bytes.insert(bytes.end(), section->stored.begin(), section->stored.end());
    }
    return std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), "test.kip");
}

struct Loaded {
    Loader::ResultStatus status;
    std::vector<u8> image; // The first bytes of the loaded image, when loaded.
};

// Loads `kip` into a new process of an initialised kernel and reads back the start of its image.
Loaded Load(const FileSys::VirtualFile& kip, std::size_t read_size) {
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
    Loader::AppLoader_KIP loader{kip};
    Loaded loaded{loader.Load(process, system).first, {}};
    if (loaded.status == Loader::ResultStatus::Success) {
        loaded.image.resize(read_size);
        process.GetMemory().ReadBlock(process.GetEntryPoint(), loaded.image.data(),
                                      loaded.image.size());
    }
    return loaded;
}

} // Anonymous namespace

TEST_CASE("KIP loads into a process", "[core][loader]") {
    const auto loaded =
        Load(MakeKip({0, 4, "text"}, {0x1000, 6, "rodata"}, {0x2000, 4, "data"}, 0x3000, 0x2000),
             0x2004);
    REQUIRE(loaded.status == Loader::ResultStatus::Success);
    const auto at = [&](std::size_t offset, std::size_t size) {
        return std::string_view{reinterpret_cast<const char*>(loaded.image.data()) + offset, size};
    };
    CHECK(at(0, 4) == "text");
    CHECK(at(0x1000, 6) == "rodata");
    CHECK(at(0x2000, 4) == "data");
    // Past a section's bytes, its segment is zero.
    CHECK(at(4, 4) == std::string_view("\0\0\0\0", 4));
}

TEST_CASE("KIP with an empty .rodata loads into a process", "[core][loader]") {
    const auto loaded =
        Load(MakeKip({0, 4, "text"}, {0x1000, 0, ""}, {0x1000, 4, "data"}, 0x2000, 0), 0x1004);
    REQUIRE(loaded.status == Loader::ResultStatus::Success);
    CHECK(std::string_view{reinterpret_cast<const char*>(loaded.image.data()) + 0x1000, 4} ==
          "data");
}

TEST_CASE("KIP loader refuses a layout the kernel would refuse", "[core][loader]") {
    // .rodata off a page.
    const auto loaded =
        Load(MakeKip({0, 4, "text"}, {0x800, 4, "ro"}, {0x2000, 4, "data"}, 0x3000, 0), 0);
    CHECK(loaded.status == Loader::ResultStatus::ErrorBadKIPHeader);
}
