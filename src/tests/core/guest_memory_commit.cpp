// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <cstring>

#include <catch2/catch_test_macros.hpp>
#ifdef _WIN32
#include <windows.h>
#endif

#include "common/host_memory.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_memory_manager.h"
#include "core/hle/kernel/k_page_group.h"
#include "core/hle/kernel/k_system_resource.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc_results.h"
#include "tests/common/host_memory_test_access.h"
#include "tests/core/system_test_context.h"

namespace {
constexpr size_t Backing = 0x100000;
constexpr size_t Virtual = 0x400000;
constexpr auto ReadWrite = Common::MemoryPermission::ReadWrite;

#ifdef _WIN32
/// The section reserves its pages unless EDEN_BACKING_COMMIT_ALL=1 brings SEC_COMMIT back.
bool ReservesSection() {
    const char* const value = std::getenv("EDEN_BACKING_COMMIT_ALL");
    return value == nullptr || value[0] != '1';
}

MEMORY_BASIC_INFORMATION Query(const void* address) {
    MEMORY_BASIC_INFORMATION info{};
    REQUIRE(VirtualQuery(address, &info, sizeof(info)) == sizeof(info));
    return info;
}
#endif
} // namespace

TEST_CASE("CommitBacking refuses ranges that end past guest memory", "[host_memory_commit]") {
    Common::HostMemory memory{Backing, Virtual};
    REQUIRE(memory.BackingBasePointer() != nullptr);
    CHECK(memory.CommitBacking(0, 0));
    CHECK(memory.CommitBacking(0, Backing));
    const u64 committed = memory.CommittedBytes();
    CHECK_FALSE(memory.CommitBacking(Backing, 0x1000));
    CHECK_FALSE(memory.CommitBacking(Backing - 0x1000, 0x2000));
    CHECK_FALSE(memory.CommitBacking(0x1000, ~size_t{0}));
    CHECK_FALSE(memory.CommitBacking(~size_t{0} - 0xfff, 0x1000));
    CHECK(memory.CommittedBytes() == committed);
#ifdef _WIN32
    if (ReservesSection() && memory.VirtualBasePointer() != nullptr) {
        CHECK(committed == Backing);
    }
#endif
}

TEST_CASE("CommitBacking checks requests against the exact guest memory size",
          "[host_memory_commit]") {
    // The section's last page is only partly guest memory.
    constexpr size_t Unaligned = Backing + 0x1001;
    Common::HostMemory memory{Unaligned, Virtual};
    REQUIRE(memory.BackingBasePointer() != nullptr);
    CHECK(memory.CommitBacking(Unaligned - 1, 1));
    CHECK(memory.CommitBacking(Backing, 0x1001));
    CHECK_FALSE(memory.CommitBacking(Unaligned, 1));
    CHECK_FALSE(memory.CommitBacking(Backing + 0x1800, 1));
    CHECK_FALSE(memory.CommitBacking(Backing, 0x1002));
    CHECK(memory.CommitBacking(Unaligned, 0));
}

#ifdef _WIN32
TEST_CASE("Windows guest memory is committed as it is handed out or mapped",
          "[host_memory_commit]") {
    Common::HostMemory memory{Backing, Virtual};
    u8* const backing = memory.BackingBasePointer();
    REQUIRE(backing != nullptr);
    if (memory.VirtualBasePointer() == nullptr || !ReservesSection()) {
        SKIP("no fastmem arena, or the section is committed whole");
    }
    CHECK(Query(backing).State == MEM_RESERVE);

    REQUIRE(memory.MarkUsed(0x10000, 0x1000));
    CHECK(Query(backing + 0x10000).State == MEM_COMMIT);
    CHECK(Query(backing + 0x1f000).State == MEM_COMMIT); // the whole 64 KiB
    CHECK(Query(backing + 0x20000).State == MEM_RESERVE);

    memory.Map(0x40000, 0x30000, 0x2000, ReadWrite, false);
    CHECK(Query(backing + 0x30000).State == MEM_COMMIT);
    u8* const view = memory.VirtualBasePointer() + 0x40000;
    view[0] = 0x5a;
    CHECK(backing[0x30000] == 0x5a);
    memory.Protect(0x40000, 0x1000, Common::MemoryPermission::Read);
    CHECK(Query(view).Protect == PAGE_READONLY);

    // Out of commit, Map leaves the range unmapped instead of a view over reserved pages.
    Common::HostMemoryTestAccess::FailCommits(memory, true);
    CHECK_FALSE(memory.MarkUsed(0x60000, 0x1000));
    memory.Map(0x80000, 0x70000, 0x1000, ReadWrite, false);
    CHECK(Query(backing + 0x70000).State == MEM_RESERVE);
    CHECK(Query(memory.VirtualBasePointer() + 0x80000).Type != MEM_MAPPED);
    Common::HostMemoryTestAccess::FailCommits(memory, false);
    CHECK(Query(backing + 0x60000).State == MEM_RESERVE);
}
#endif

TEST_CASE("Kernel allocations fail softly when guest memory can't be committed",
          "[core][kernel][host_memory_commit]") {
    ScopedSystemTestContext environment;
    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    Kernel::KThread bootstrap{kernel};
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, &bootstrap, nullptr).IsSuccess());
    kernel.SetCurrentEmuThread(&bootstrap);
    kernel.Initialize();

    using Manager = Kernel::KMemoryManager;
    auto& manager = kernel.MemoryManager();
    const u32 option = Manager::EncodeOption(Manager::Pool::Application,
                                             Manager::Direction::FromFront);
    const size_t free_before = manager.GetFreeSize(Manager::Pool::Application);
    auto& buffer = system.DeviceMemory().buffer;
    constexpr size_t Pages = 32;

    Common::HostMemoryTestAccess::FailCommits(buffer, true);
    CHECK(manager.AllocateAndOpenContinuous(Pages, 1, option) == 0);
    {
        Kernel::KPageGroup group{kernel,
                                 kernel.GetSystemSystemResource().GetBlockInfoManagerPointer()};
        CHECK(manager.AllocateAndOpen(&group, Pages, option) == Kernel::ResultOutOfMemory);
        CHECK(group.GetNumPages() == 0);
    }
    // Nothing leaked: every page went back to the pool.
    CHECK(manager.GetFreeSize(Manager::Pool::Application) == free_before);

    // A failure after the first blocks frees those too.
    for (size_t succeeded = 1; succeeded <= 3; ++succeeded) {
        Kernel::KPageGroup group{kernel,
                                 kernel.GetSystemSystemResource().GetBlockInfoManagerPointer()};
        Common::HostMemoryTestAccess::FailCommitsAfter(buffer, succeeded);
        CHECK(manager.AllocateAndOpen(&group, Pages * 4 + 1, option) ==
              Kernel::ResultOutOfMemory);
        CHECK(group.GetNumPages() == 0);
        CHECK(manager.GetFreeSize(Manager::Pool::Application) == free_before);
    }

    Common::HostMemoryTestAccess::FailCommits(buffer, false);
    const Kernel::KPhysicalAddress block = manager.AllocateAndOpenContinuous(Pages, 1, option);
    CHECK(block != 0);
    {
        Kernel::KPageGroup group{kernel,
                                 kernel.GetSystemSystemResource().GetBlockInfoManagerPointer()};
        REQUIRE(manager.AllocateAndOpen(&group, Pages, option).IsSuccess());
        CHECK(group.GetNumPages() == Pages);
        group.Close(kernel);
    }
    if (block != 0) {
        manager.Close(block, Pages);
    }
    CHECK(manager.GetFreeSize(Manager::Pool::Application) == free_before);

    system.CoreTiming().ClearPendingEvents();
    kernel.Shutdown();
    kernel.SetCurrentEmuThread(nullptr);
    kernel.UnregisterKernelObject(&bootstrap);
}
