// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "tests/core/system_test_context.h"
#include "video_core/gpu.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/renderer_null/null_rasterizer.h"
#include "video_core/shader_cache.h"

namespace VideoCommon {
struct ShaderCacheTestAccess {
    static std::unique_ptr<ShaderCache> Create(Tegra::MaxwellDeviceMemoryManager& memory) {
        return std::unique_ptr<ShaderCache>{new ShaderCache{memory}};
    }
    static void Register(ShaderCache& cache, VAddr address, size_t size, u64 hash) {
        auto data = std::make_unique<ShaderInfo>();
        data->unique_hash = hash;
        cache.Register(std::move(data), address, size);
    }
    static const ShaderInfo* Find(const ShaderCache& cache, VAddr address) {
        return cache.TryGet(address);
    }
    static void RegisterNative(ShaderCache& cache, VAddr address, u64 hash, u64 key) {
        auto data = std::make_unique<ShaderInfo>();
        data->unique_hash = hash;
        auto lowered = std::make_shared<LoweredNativeCode>();
        lowered->native_program_key = key;
        data->lowered_native = std::move(lowered);
        cache.Register(std::move(data), address, 0x100);
    }
    static void Reject(ShaderCache& cache, VAddr address) {
        RememberRejection(cache, address, address, "rejected");
    }
    static bool IsRejected(ShaderCache& cache, VAddr address) {
        return IsRejected(cache, address, address);
    }
    static void Rethrow(ShaderCache& cache, VAddr address) {
        if (const auto error = cache.LookupRejection(address, address, 0)) {
            std::rethrow_exception(error);
        }
    }
    static void RememberRejection(ShaderCache& cache, VAddr address) {
        RememberRejection(cache, address, address, "lowering failed");
    }
    static void RememberRejection(ShaderCache& cache, VAddr address, GPUVAddr gpu_addr,
                                  const char* message) {
        try {
            throw std::runtime_error{message};
        } catch (...) {
            const u64 serial = cache.gpu_memory ? cache.gpu_memory->HostPointerSerial() : 0;
            cache.RememberRejection(address, gpu_addr, serial);
        }
    }
    static bool IsRejected(ShaderCache& cache, VAddr address, GPUVAddr gpu_addr) {
        const u64 serial = cache.gpu_memory ? cache.gpu_memory->HostPointerSerial() : 0;
        return bool(cache.LookupRejection(address, gpu_addr, serial));
    }
    static void SetMemory(ShaderCache& cache, Tegra::MemoryManager& memory) {
        cache.gpu_memory = &memory;
        cache.current_address_space = memory.GetID();
    }
    static u64 Serial(const ShaderCache& cache) {
        return cache.removal_serial.load();
    }
    static size_t PageRefs(const ShaderCache& cache, VAddr page) {
        const auto it = cache.rejected_page_refs.find(page);
        return it == cache.rejected_page_refs.end() ? 0 : it->second.refs;
    }
};
} // namespace VideoCommon

using VideoCommon::ShaderCacheTestAccess;

TEST_CASE("Shader batch invalidation matches sequential cache contents", "[shader_invalidation]") {
    Core::DeviceMemory memory;
    Tegra::MaxwellDeviceMemoryManager device_memory{memory};
    auto batched = ShaderCacheTestAccess::Create(device_memory);
    auto sequential = ShaderCacheTestAccess::Create(device_memory);
    std::mt19937 random{0x16'67};
    constexpr VAddr base = 0x100000;
    for (unsigned iteration = 0; iteration < 600; ++iteration) {
        for (unsigned slot = 0; slot < 128; ++slot) {
            const auto address = base + slot * 4096;
            if (!ShaderCacheTestAccess::Find(*batched, address)) {
                const auto size = 1 + random() % 65536;
                ShaderCacheTestAccess::Register(*batched, address, size, iteration + slot);
                ShaderCacheTestAccess::Register(*sequential, address, size, iteration + slot);
            }
        }
        std::vector<std::pair<DAddr, size_t>> writes;
        for (unsigned i = 0, count = random() % 24; i < count; ++i) {
            writes.emplace_back(base + random() % (128 * 4096), random() % 32768);
            if (i % 4 == 0) {
                writes.push_back(writes.back());
            }
        }
        for (const auto& [address, size] : writes) {
            sequential->InvalidateRegion(address, size);
        }
        batched->InvalidateRegions(writes);
        for (unsigned slot = 0; slot < 128; ++slot) {
            const auto address = base + slot * 4096;
            const auto actual = ShaderCacheTestAccess::Find(*batched, address);
            const auto expected = ShaderCacheTestAccess::Find(*sequential, address);
            REQUIRE(bool(actual) == bool(expected));
            if (actual) {
                REQUIRE(actual->unique_hash == expected->unique_hash);
            }
        }
    }
}

TEST_CASE("Shader empty batches preserve pending invalidations", "[shader_invalidation]") {
    Core::DeviceMemory memory;
    Tegra::MaxwellDeviceMemoryManager device_memory{memory};
    auto cache = ShaderCacheTestAccess::Create(device_memory);
    constexpr VAddr address = 0x100000;
    ShaderCacheTestAccess::Register(*cache, address, 32768, 123);
    cache->OnCacheInvalidation(address, 1);
    cache->InvalidateRegions({});
    REQUIRE(ShaderCacheTestAccess::Find(*cache, address));
    const std::array unrelated{std::pair<DAddr, size_t>{address + 0x100000, 4096}};
    cache->InvalidateRegions(unrelated);
    REQUIRE_FALSE(ShaderCacheTestAccess::Find(*cache, address));
}

TEST_CASE("Debug server rejections drop matching native programs", "[shader_invalidation]") {
    Core::DeviceMemory memory;
    Tegra::MaxwellDeviceMemoryManager device_memory{memory};
    auto cache = ShaderCacheTestAccess::Create(device_memory);
    constexpr VAddr by_key = 0x100000;
    constexpr VAddr by_hash = 0x200000;
    constexpr VAddr kept = 0x300000;
    constexpr VAddr maxwell = 0x400000;
    constexpr VAddr earlier = 0x500000;
    ShaderCacheTestAccess::RegisterNative(*cache, by_key, 0x11, 0xaaaa);
    ShaderCacheTestAccess::RegisterNative(*cache, by_hash, 0x22, 0xbbbb);
    ShaderCacheTestAccess::RegisterNative(*cache, kept, 0x33, 0xcccc);
    ShaderCacheTestAccess::Register(*cache, maxwell, 0x100, 0xaaaa);
    ShaderCacheTestAccess::Reject(*cache, earlier);

    VideoCore::Nx2Debug::BeginSession();
    const std::array<u64, 2> rejected{0xaaaa, 0x22};
    const u64 generation = VideoCore::Nx2Debug::SetRejected(rejected, true);
    const auto change = VideoCore::Nx2Debug::TakeRejectChange();
    REQUIRE(change);
    REQUIRE(change->generation == generation);
    REQUIRE_FALSE(change->unrejected);
    cache->RefreshDebugRejections(change->unrejected);
    REQUIRE_FALSE(ShaderCacheTestAccess::Find(*cache, by_key));
    REQUIRE_FALSE(ShaderCacheTestAccess::Find(*cache, by_hash));
    REQUIRE(ShaderCacheTestAccess::Find(*cache, kept));
    // Only lowered native programs are matched; a Maxwell program with the same hash stays.
    REQUIRE(ShaderCacheTestAccess::Find(*cache, maxwell));
    REQUIRE(ShaderCacheTestAccess::IsRejected(*cache, earlier));

    const std::array<u64, 1> restored{0xaaaa};
    VideoCore::Nx2Debug::SetRejected(restored, false);
    const auto undo = VideoCore::Nx2Debug::TakeRejectChange();
    REQUIRE(undo);
    REQUIRE(undo->unrejected);
    cache->RefreshDebugRejections(undo->unrejected);
    REQUIRE_FALSE(ShaderCacheTestAccess::IsRejected(*cache, earlier));
    REQUIRE(VideoCore::Nx2Debug::RejectedHashes() == std::vector<u64>{0x22});
    // Later tests in the process see the same reject list.
    const std::array<u64, 1> remaining{0x22};
    VideoCore::Nx2Debug::SetRejected(remaining, false);
    VideoCore::Nx2Debug::EndSession();
}

TEST_CASE("A remembered rejection marks its page so filtered writes still clear it",
          "[shader_invalidation][page_filter]") {
    Core::DeviceMemory memory;
    Tegra::MaxwellDeviceMemoryManager device_memory{memory};
    auto cache = ShaderCacheTestAccess::Create(device_memory);
    constexpr VAddr rejected = 0x7'1234'5100;
    REQUIRE_FALSE(cache->page_filter.Any(rejected, 1));

    ShaderCacheTestAccess::RememberRejection(*cache, rejected);
    REQUIRE(ShaderCacheTestAccess::IsRejected(*cache, rejected));
    CHECK(cache->page_filter.Any(rejected, 1));
    constexpr VAddr page = rejected & ~VAddr{0xfff};
    CHECK(cache->page_filter.Any(page, 0x1000));
    CHECK_FALSE(cache->page_filter.Any(page + 0x1000, 0x1000));
    CHECK_FALSE(cache->page_filter.Any(page - 0x1000, 0x1000));

    // A write elsewhere keeps the rejection.
    const std::array unrelated{std::pair<DAddr, size_t>{rejected + 0x10000, 4}};
    cache->InvalidateRegions(unrelated);
    CHECK(ShaderCacheTestAccess::IsRejected(*cache, rejected));

    // A write over it, admitted by the filter, lets the program be lowered again.
    const std::array over{std::pair<DAddr, size_t>{rejected, 8}};
    REQUIRE(cache->page_filter.Any(over[0].first, over[0].second));
    cache->InvalidateRegions(over);
    CHECK_FALSE(ShaderCacheTestAccess::IsRejected(*cache, rejected));
}

TEST_CASE("Remembering a rejection twice keeps the first error",
          "[shader_invalidation][page_filter]") {
    Core::DeviceMemory memory;
    Tegra::MaxwellDeviceMemoryManager device_memory{memory};
    auto cache = ShaderCacheTestAccess::Create(device_memory);
    constexpr VAddr rejected = 0x200000;
    ShaderCacheTestAccess::Reject(*cache, rejected);
    ShaderCacheTestAccess::RememberRejection(*cache, rejected);
    CHECK(ShaderCacheTestAccess::IsRejected(*cache, rejected));
    try {
        ShaderCacheTestAccess::Rethrow(*cache, rejected);
        FAIL("Rejection was not rethrown");
    } catch (const std::runtime_error& error) {
        CHECK(std::string_view{error.what()} == "rejected");
    }
}

TEST_CASE("Shader header-page writes invalidate remembered verdicts", "[shader_invalidation]") {
    Core::DeviceMemory memory;
    Tegra::MaxwellDeviceMemoryManager device_memory{memory};
    auto cache = ShaderCacheTestAccess::Create(device_memory);
    using Access = ShaderCacheTestAccess;
    constexpr VAddr page = 0x6f92000;
    constexpr VAddr first = page + 0xb00;
    constexpr VAddr second = page + 0xc00;
    Access::Reject(*cache, first);
    Access::Reject(*cache, first);
    Access::Reject(*cache, second);
    REQUIRE(Access::PageRefs(*cache, page) == 2);
    const u64 serial = Access::Serial(*cache);
    cache->InvalidateRegion(page - 0x1000, 0x1000);
    cache->InvalidateRegion(page + 0x1000, 0x1000);
    cache->InvalidateRegion(first, 0);
    CHECK(Access::Serial(*cache) == serial);
    CHECK(Access::IsRejected(*cache, first));

    cache->InvalidateRegion(first + 0x40, 4);
    CHECK_FALSE(Access::IsRejected(*cache, first));
    CHECK_FALSE(Access::IsRejected(*cache, second));
    CHECK(Access::Serial(*cache) > serial);
    CHECK(Access::PageRefs(*cache, page) == 0);
}

TEST_CASE("A rejected shader watches both GPU mappings of its header and code",
          "[shader_invalidation]") {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device{physical};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    Tegra::MemoryManager memory{system, device, 32, 1ULL << 31, 16};
    memory.BindRasterizer(&rasterizer);
    using Access = ShaderCacheTestAccess;

    constexpr GPUVAddr gpu_page = 0x100000;
    constexpr DAddr first_page = 0x200000;
    constexpr DAddr second_page = 0x300000;
    constexpr GPUVAddr gpu_addr = gpu_page + 0xf80;
    constexpr DAddr device_addr = first_page + 0xf80;
    system.Initialize();
    Kernel::KProcess process{system.Kernel()};
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(24, 12);
    const u8* const backing = physical.GetPointerFromRaw<u8>(0x10000);
    for (u64 page = 0x10; page < 0x12; ++page) {
        table.entries.GetAndFault(page).Store(
            false, Common::PageType::Memory, 1, reinterpret_cast<uintptr_t>(backing) - 0x10000);
    }
    process.GetMemory().SetCurrentPageTable(process);
    device.BindInterface(&rasterizer);
    const Core::Asid asid = device.RegisterProcess(&process.GetMemory());
    device.Map(first_page, 0x10000, 0x1000, asid);
    SCOPE_EXIT {
        device.Unmap(first_page, 0x1000);
        device.Unmap(second_page, 0x1000);
        device.UnregisterProcess(asid);
        system.Kernel().UnregisterKernelObject(&process);
    };
    auto cache = Access::Create(device);
    Access::SetMemory(*cache, memory);
    memory.Map(gpu_page, first_page, 0x1000, Tegra::PTEKind::PITCH, false);
    memory.Map(gpu_page + 0x1000, second_page, 0x1000, Tegra::PTEKind::PITCH, false);
    Access::RememberRejection(*cache, device_addr, gpu_addr, "rejected");
    CHECK(cache->page_filter.Any(first_page, 0x1000));
    CHECK(cache->page_filter.Any(second_page, 0x1000));
    CHECK_FALSE(cache->page_filter.Any(first_page + 0x1000, 0x1000));
    CHECK(device.IsRangeCached(first_page, 0x1000));
    CHECK_FALSE(device.IsRangeCached(second_page, 0x1000));
    REQUIRE(Access::IsRejected(*cache, device_addr, gpu_addr));

    device.Map(second_page, 0x11000, 0x1000, asid);
    Access::RememberRejection(*cache, device_addr + 0x10, gpu_addr + 0x10, "rejected later");
    CHECK(Access::PageRefs(*cache, second_page) == 2);
    CHECK(device.IsRangeCached(second_page, 0x1000));
    REQUIRE(Access::IsRejected(*cache, device_addr + 0x10, gpu_addr + 0x10));
    cache->OnCacheInvalidation(second_page + 0x10, 4);
    CHECK_FALSE(Access::IsRejected(*cache, device_addr, gpu_addr));
    CHECK_FALSE(Access::IsRejected(*cache, device_addr + 0x10, gpu_addr + 0x10));
    CHECK_FALSE(device.IsRangeCached(second_page, 0x1000));

    Access::RememberRejection(*cache, device_addr, gpu_addr, "rejected");
    memory.Map(gpu_page + 0x1000, second_page + 0x1000, 0x1000, Tegra::PTEKind::PITCH, false);
    CHECK_FALSE(Access::IsRejected(*cache, device_addr, gpu_addr));
    Access::RememberRejection(*cache, device_addr, gpu_addr, "rejected");
    memory.Unmap(gpu_page + 0x1000, 0x1000);
    CHECK_FALSE(Access::IsRejected(*cache, device_addr, gpu_addr));
    Access::RememberRejection(*cache, device_addr, gpu_addr, "unmapped");
    memory.Map(gpu_page + 0x1000, second_page, 0x1000, Tegra::PTEKind::PITCH, false);
    CHECK_FALSE(Access::IsRejected(*cache, device_addr, gpu_addr));
    CHECK(Access::PageRefs(*cache, first_page) == 0);
    CHECK(Access::PageRefs(*cache, second_page) == 0);
    CHECK_FALSE(device.IsRangeCached(first_page, 0x1000));
    CHECK_FALSE(device.IsRangeCached(second_page, 0x1000));
}

TEST_CASE("Shader invalidation CPU benchmark", "[.][cache_bench]") {
    Core::DeviceMemory memory;
    Tegra::MaxwellDeviceMemoryManager device_memory{memory};
    auto batched = ShaderCacheTestAccess::Create(device_memory);
    auto sequential = ShaderCacheTestAccess::Create(device_memory);
    std::vector<std::pair<DAddr, size_t>> writes;
    constexpr VAddr base = 0x100000;
    for (unsigned slot = 0; slot < 768; ++slot) {
        writes.emplace_back(base + slot * 65536, 1);
    }
    double sequential_ms{}, batched_ms{};
    for (unsigned repeat = 0; repeat < 20; ++repeat) {
        for (unsigned slot = 0; slot < 1024; ++slot) {
            const auto address = base + slot * 65536;
            if (!ShaderCacheTestAccess::Find(*batched, address)) {
                ShaderCacheTestAccess::Register(*batched, address, 64, slot);
                ShaderCacheTestAccess::Register(*sequential, address, 64, slot);
            }
        }
        auto start = std::chrono::steady_clock::now();
        for (const auto& [address, size] : writes) {
            sequential->InvalidateRegion(address, size);
        }
        sequential_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        start = std::chrono::steady_clock::now();
        batched->InvalidateRegions(writes);
        batched_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        REQUIRE_FALSE(ShaderCacheTestAccess::Find(*batched, base));
        REQUIRE(ShaderCacheTestAccess::Find(*batched, base + 1023 * 65536));
    }
    std::cout << "Shader invalidation: sequential=" << sequential_ms << " ms batched=" << batched_ms
              << " ms (CPU only, not gameplay FPS)\n";
}
