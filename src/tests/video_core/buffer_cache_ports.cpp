// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "common/page_table.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/control/channel_state.h"
#include "video_core/gpu.h"
#include "video_core/renderer_null/null_rasterizer.h"

namespace {

struct Runtime;

struct Buffer : VideoCommon::BufferBase {
    Buffer(Runtime&, VideoCommon::NullBufferParams params) : BufferBase{params} {}
    Buffer(Runtime&, DAddr address, u32 size, bool mapped)
        : BufferBase{address, size}, bytes(size), host_mapped{mapped} {}

    void MarkUsage(u64 offset, u64 size) {
        REQUIRE(offset + size <= bytes.size());
    }
    std::span<u8> Mapped() {
        return host_mapped ? std::span{bytes} : std::span<u8>{};
    }
    u64 LastUsageTick() const {
        return tick;
    }

    std::vector<u8> bytes;
    bool host_mapped{};
    u64 tick{};
};

struct Staging {
    Buffer* buffer;
    u64 offset;
    std::span<u8> mapped_span;
};

struct Runtime {
    bool CanReportMemoryUsage() const {
        return false;
    }
    u64 GetDeviceLocalMemory() const {
        return 0;
    }
    u32 GetStorageBufferAlignment() const {
        return alignment;
    }
    u32 GetUniformBufferAlignment() const {
        return 1;
    }
    bool IsFree(u64) const {
        return true;
    }
    void WaitSubmittedTick(u64) {}
    bool CanReorderUpload(const Buffer&, std::span<VideoCommon::BufferCopy>) const {
        return false;
    }
    Staging DownloadStagingBuffer(u64 size, bool = false) {
        staging = std::make_unique<Buffer>(*this, 0, size + 128, false);
        ++allocations;
        return {staging.get(), 128, std::span{staging->bytes}.subspan(128)};
    }
    Staging UploadStagingBuffer(u64 size) {
        return DownloadStagingBuffer(size);
    }
    void CopyBuffer(Buffer* dst, Buffer& src, std::span<const VideoCommon::BufferCopy> copies,
                    bool) {
        for (const auto& copy : copies) {
            REQUIRE(copy.src_offset + copy.size <= src.bytes.size());
            REQUIRE(copy.dst_offset + copy.size <= dst->bytes.size());
            pending.push_back([dst, &src, copy] {
                std::memcpy(dst->bytes.data() + copy.dst_offset, src.bytes.data() + copy.src_offset,
                            copy.size);
            });
            ++downloads;
        }
    }
    void CopyBuffer(Buffer& dst, Buffer* src, std::span<const VideoCommon::BufferCopy> copies, bool,
                    bool) {
        for (const auto& copy : copies) {
            REQUIRE(copy.src_offset + copy.size <= src->bytes.size());
            REQUIRE(copy.dst_offset + copy.size <= dst.bytes.size());
            std::memcpy(dst.bytes.data() + copy.dst_offset, src->bytes.data() + copy.src_offset,
                        copy.size);
            ++uploads;
            upload_bytes += copy.size;
        }
    }
    void CopyBuffer(Buffer& dst, Buffer& src, std::span<const VideoCommon::BufferCopy> copies,
                    bool) {
        for (const auto& copy : copies) {
            REQUIRE(copy.src_offset + copy.size <= src.bytes.size());
            REQUIRE(copy.dst_offset + copy.size <= dst.bytes.size());
            std::memcpy(dst.bytes.data() + copy.dst_offset, src.bytes.data() + copy.src_offset,
                        copy.size);
        }
    }
    void ClearBuffer(Buffer& buffer, u32 offset, size_t size, u32 value) {
        REQUIRE(offset + size <= buffer.bytes.size());
        std::memset(buffer.bytes.data() + offset, static_cast<int>(value), size);
    }
    void TickFrame(Common::SlotVector<Buffer>&) {}
    u64 GetDeviceMemoryUsage() const {
        return 0;
    }
    void FreeDeferredStagingBuffer(Staging&) {}
    void Finish() {
        if (before_completion) {
            before_completion();
        }
        for (auto& copy : pending) {
            copy();
        }
        pending.clear();
        ++finishes;
    }
    void Wait(u64 tick) {
        if (before_completion) {
            before_completion();
        }
        waited_tick = tick;
        ++waits;
    }
    void FinishForHostRead() {
        ++host_read_barriers;
        Finish();
    }
    void BindUniformBuffer(Buffer&, u32, u32) {}
    std::span<u8> BindMappedUniformBuffer(size_t, u32, u32 size) {
        stream.resize(size);
        return stream;
    }

    u32 alignment{256};
    u32 allocations{}, downloads{}, uploads{}, finishes{}, waits{};
    u32 host_read_barriers{};
    u64 upload_bytes{}, waited_tick{};
    std::unique_ptr<Buffer> staging;
    std::vector<u8> stream;
    std::vector<std::function<void()>> pending;
    std::function<void()> before_completion;
};

struct Tracker : VideoCommon::MemoryTrackerBase<Tegra::MaxwellDeviceMemoryManager> {
    using MemoryTrackerBase::MemoryTrackerBase;
    void MarkRegionAsCpuModified(DAddr address, u64 size) {
        ++cpu_marks;
        MemoryTrackerBase::MarkRegionAsCpuModified(address, size);
    }
    u32 cpu_marks{};
};

struct Policy {
    using Runtime = ::Runtime;
    using Buffer = ::Buffer;
    using Async_Buffer = Staging;
    using MemoryTracker = Tracker;
    static constexpr bool IS_OPENGL = false;
    static constexpr bool HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS = false;
    static constexpr bool HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT = false;
    static constexpr bool NEEDS_BIND_UNIFORM_INDEX = false;
    static constexpr bool NEEDS_BIND_STORAGE_INDEX = false;
    static constexpr bool USE_MEMORY_MAPS = true;
    static constexpr bool SEPARATE_IMAGE_BUFFER_BINDINGS = false;
    static constexpr bool USE_MEMORY_MAPS_FOR_UPLOADS = true;
};

using Cache = VideoCommon::BufferCache<Policy>;

} // namespace

namespace VideoCommon {

struct BufferCacheTestAccess {
    static Binding Bind(Cache& cache, Tegra::MemoryManager& memory, GPUVAddr descriptor,
                        bool written = false, bool native = false, bool table = false) {
        cache.gpu_memory = &memory;
        return cache.StorageBufferBinding(descriptor, 0, written, native, table);
    }
    static BufferId Add(Cache& cache, DAddr address, u32 size, bool mapped = false) {
        const auto id = cache.slot_buffers.insert(cache.runtime, address, size, mapped);
        cache.Register(id);
        cache.memory_tracker.ForEachUploadRange(address, size, [](auto, auto) {});
        return id;
    }
    static Buffer& Get(Cache& cache, BufferId id) {
        return cache.slot_buffers[id];
    }
    static void Dirty(Cache& cache, DAddr address, u64 size) {
        cache.memory_tracker.MarkRegionAsGpuModified(address, size);
        cache.gpu_modified_ranges.Add(address, size);
        cache.uncommitted_gpu_modified_ranges.Add(address, size);
        cache.committed_gpu_modified_ranges.emplace_back().Add(address, size);
        cache.pending_download_ranges.emplace_back().Add(address, size);
    }
    static bool Pending(Cache& cache, DAddr address, u64 size) {
        if (cache.gpu_modified_ranges.Intersects(address, size) ||
            cache.uncommitted_gpu_modified_ranges.Intersects(address, size)) {
            return true;
        }
        for (const auto& ranges : cache.committed_gpu_modified_ranges) {
            if (ranges.Intersects(address, size))
                return true;
        }
        for (const auto& ranges : cache.pending_download_ranges) {
            if (ranges.Intersects(address, size))
                return true;
        }
        return false;
    }
    static void DrawWrite(Cache& cache, DAddr page) {
        std::scoped_lock lock{cache.nx2_list_mutex};
        auto& entry = cache.nx2_list_pages[page];
        entry.draw_written = true;
        ++entry.writes;
    }
    static void Download(Cache& cache, std::span<const std::pair<DAddr, u64>> ranges) {
        cache.DownloadRangesMemory(ranges);
    }
    static void Uniform(Cache& cache, BufferCacheChannelInfo& channel, BufferId id, DAddr address,
                        u32 size) {
        cache.channel_state = &channel;
        cache.maxwell3d = &channel.maxwell3d;
        cache.gpu_memory = &channel.gpu_memory;
        channel.uniform_buffers[0][0] = {.device_addr = address, .size = size, .buffer_id = id};
        cache.BindHostGraphicsUniformBuffer(0, 0, 0, true);
    }
    static void NextFrame(Cache& cache) {
        cache.native_indirect_uniforms_this_frame.clear();
    }
    static u32 CpuMarks(Cache& cache) {
        return cache.memory_tracker.cpu_marks;
    }
    static size_t Deferred(Cache& cache) {
        std::scoped_lock lock{cache.deferred_page_writes_mutex};
        return cache.deferred_page_writes.size();
    }
    static void Defer(Cache& cache, DAddr address, u64 size) {
        cache.DeferPageWrite(address, size);
    }
    static void ApplyDeferred(Cache& cache) {
        cache.ApplyDeferredPageWrites();
    }
    static std::vector<std::pair<DAddr, u64>> DeferredRanges(Cache& cache) {
        std::scoped_lock lock{cache.deferred_page_writes_mutex};
        return cache.deferred_page_writes;
    }
};

} // namespace VideoCommon

namespace {

using Access = VideoCommon::BufferCacheTestAccess;

struct Fixture {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device{physical};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    std::optional<Tegra::MemoryManager> memory;
    Runtime runtime;
    std::unique_ptr<Cache> cache;
    std::unique_ptr<Kernel::KProcess> process;
    Core::Asid asid{};
    u8* bytes{};

    Fixture() {
        system.Initialize();
        process = std::make_unique<Kernel::KProcess>(system.Kernel());
        auto& table = process->GetPageTable().GetImpl();
        table.Resize(24, 12);
        bytes = physical.GetPointerFromRaw<u8>(0x10000);
        std::memset(bytes, 0, 0x40000);
        for (u64 page = 0; page < 64; ++page) {
            table.entries.GetAndFault(0x10 + page)
                .Store(false, Common::PageType::Memory, 1,
                       reinterpret_cast<uintptr_t>(bytes) - 0x10000);
        }
        process->GetMemory().SetCurrentPageTable(*process);
        asid = device.RegisterProcess(&process->GetMemory());
        device.BindInterface(&rasterizer);
        device.Map(0x10000, 0x10000, 0x40000, asid);
        ResetMemory();
        cache = std::make_unique<Cache>(device, runtime);
    }
    ~Fixture() {
        cache.reset();
        memory.reset();
        device.Unmap(0x10000, 0x40000);
        device.UnregisterProcess(asid);
        system.Kernel().UnregisterKernelObject(process.get());
    }
    void ResetMemory() {
        memory.reset();
        memory.emplace(system, device, 32, 1ULL << 31, 16);
        memory->BindRasterizer(&rasterizer);
        memory->Map(0x100000, 0x10000, 0x10000, Tegra::PTEKind::PITCH, true);
    }
    void Descriptor(GPUVAddr address, u64 size) {
        std::memcpy(bytes, &address, sizeof(address));
        std::memcpy(bytes + 8, &size, sizeof(size));
    }
    VideoCommon::Binding Bind(bool written = false, bool native = false, bool table = false) {
        return Access::Bind(*cache, *memory, 0x100000, written, native, table);
    }
};

} // namespace

TEST_CASE("Storage binding cache tracks map changes and descriptor dependencies",
          "[buffer_ports]") {
    Fixture f;
    constexpr GPUVAddr base = 0x200000;
    f.memory->Map(base, 0x20000, 0x10000, Tegra::PTEKind::PITCH, true);
    f.Descriptor(base + 0x123, 0x456);
    for (int i = 0; i < 3; ++i) {
        const auto binding = f.Bind(true);
        REQUIRE(binding.device_addr == 0x20100);
        REQUIRE(binding.gpu_addr == base + 0x100);
        REQUIRE(binding.size == 0x479);
        REQUIRE(binding.buffer_id == VideoCommon::BufferId{});
        REQUIRE(binding.segment_count == 0);
    }
    REQUIRE(f.Bind().size == 0xf00);
    f.Descriptor(base + 0x123, 0x1456);
    REQUIRE(f.Bind(true).size == 0x1479);
    f.runtime.alignment = 4096;
    REQUIRE(f.Bind(true).device_addr == 0x20000);
    REQUIRE(f.Bind(true).size == 0x1579);
    REQUIRE(f.Bind(true, true).size == 0x10000);
    f.memory->Map(base, 0x30000, 0x10000, Tegra::PTEKind::PITCH, true);
    REQUIRE(f.Bind(true).device_addr == 0x30000);
    f.memory->MapSparse(base, 0x10000, true);
    REQUIRE(f.Bind().size == 0);
    f.memory->Unmap(base, 0x10000);
    REQUIRE(f.Bind().size == 0);
    f.memory->Map(base, 0x20000, 0x2000, Tegra::PTEKind::PITCH, false);
    REQUIRE(f.Bind(true, true).size == 0x2000);
    f.memory->Map(base + 0x1000, 0x40000, 0x1000, Tegra::PTEKind::PITCH, false);
    REQUIRE(f.Bind(true, true).size == 0x1000);
    const auto old_owner = f.memory->GetID();
    f.ResetMemory();
    f.memory->Map(base, 0x40000, 0x10000, Tegra::PTEKind::PITCH, true);
    REQUIRE(f.memory->GetID() != old_owner);
    REQUIRE(f.Bind(true).device_addr == 0x40000);
    f.Descriptor(0, 0x123);
    REQUIRE(f.Bind().size == 0);
}

TEST_CASE("Storage binding cache preserves native and table size policies", "[buffer_ports]") {
    Fixture f;
    constexpr GPUVAddr base = 0x4000000;
    f.memory->Map(base, 0x10000000, 80 << 20, Tegra::PTEKind::PITCH, true);
    f.Descriptor(base, 256);
    REQUIRE(f.Bind().size == 4096);
    REQUIRE(f.Bind(false, true).size == 8 << 20);
    REQUIRE(f.Bind(false, false, true).size == 64 << 20);
    REQUIRE(f.Bind(true, false, true).size == 256);
    f.Descriptor(base, (1ULL << 32) | 256);
    REQUIRE(f.Bind().size == 8 << 20);
    REQUIRE(f.Bind(false, false, true).size == 64 << 20);
    f.Descriptor(base + (79 << 20), 0);
    REQUIRE(f.Bind(false, false, true).size == 1 << 20);
}

TEST_CASE("Storage binding cache does not reuse a destroyed address space", "[buffer_ports]") {
    Fixture f;
    f.memory->Map(0x200000, 0x20000, 0x10000, Tegra::PTEKind::PITCH, true);
    f.Descriptor(0x200000, 0x1000);
    REQUIRE(f.Bind(true).device_addr == 0x20000);
    const auto serial = f.memory->MapSerial();
    const auto owner = f.memory->GetID();
    const auto* address = &*f.memory;
    f.ResetMemory();
    f.memory->Map(0x200000, 0x30000, 0x10000, Tegra::PTEKind::PITCH, true);
    REQUIRE(&*f.memory == address);
    REQUIRE(f.memory->MapSerial() == serial);
    REQUIRE(f.memory->GetID() != owner);
    REQUIRE(f.Bind(true).device_addr == 0x30000);
}

TEST_CASE("Batch downloads clamp buffers, deduplicate pages and finish before publication",
          "[buffer_ports]") {
    Fixture f;
    const auto first = Access::Add(*f.cache, 0x20000, 0x10000);
    const auto second = Access::Add(*f.cache, 0x30000, 0x10000);
    std::ranges::fill(Access::Get(*f.cache, first).bytes, 0x12);
    std::ranges::fill(Access::Get(*f.cache, second).bytes, 0x34);
    Access::Dirty(*f.cache, 0x2f000, 0x2000);
    Access::Dirty(*f.cache, 0x32000, 0x1000);
    f.runtime.before_completion = [&] {
        REQUIRE(f.bytes[0x1f000] == 0);
        REQUIRE(f.bytes[0x20000] == 0);
    };
    const std::array<std::pair<DAddr, u64>, 3> ranges{{
        {0x2f008, 0x1000},
        {0x2f000, 0x2000},
        {0x20000, 0},
    }};
    Access::Download(*f.cache, ranges);
    REQUIRE(f.runtime.finishes == 1);
    REQUIRE(f.runtime.allocations == 1);
    REQUIRE(f.runtime.downloads == 2);
    REQUIRE(f.bytes[0x1f000] == 0x12);
    REQUIRE(f.bytes[0x1ffff] == 0x12);
    REQUIRE(f.bytes[0x20000] == 0x34);
    REQUIRE(f.bytes[0x20fff] == 0x34);
    REQUIRE(f.bytes[0x22000] == 0);
    REQUIRE_FALSE(Access::Pending(*f.cache, 0x2f000, 0x2000));
    REQUIRE(Access::Pending(*f.cache, 0x32000, 0x1000));
    Access::Download(*f.cache, ranges);
    REQUIRE(f.runtime.finishes == 1);
}

TEST_CASE("Mapped and mixed batches make device writes host visible in one completion",
          "[buffer_ports]") {
    Fixture f;
    const auto first = Access::Add(*f.cache, 0x20000, 0x10000, true);
    const auto second = Access::Add(*f.cache, 0x30000, 0x10000, true);
    const auto clean = Access::Add(*f.cache, 0x40000, 0x10000, true);
    auto& a = Access::Get(*f.cache, first);
    auto& b = Access::Get(*f.cache, second);
    a.tick = 8;
    b.tick = 19;
    Access::Get(*f.cache, clean).tick = 99;
    const std::array<std::pair<DAddr, u64>, 3> ranges{{
        {0x20000, 0x1000},
        {0x30000, 0x1000},
        {0x40000, 0x1000},
    }};
    Access::Dirty(*f.cache, 0x20000, 0x1000);
    Access::Dirty(*f.cache, 0x30000, 0x1000);
    f.runtime.before_completion = [&] {
        REQUIRE(f.runtime.host_read_barriers == 1);
        REQUIRE(f.bytes[0x10000] == 0);
        REQUIRE(f.bytes[0x20000] == 0);
        std::ranges::fill(a.bytes, 0x56);
        std::ranges::fill(b.bytes, 0x78);
    };
    Access::Download(*f.cache, ranges);
    REQUIRE(f.runtime.waits == 0);
    REQUIRE(f.runtime.host_read_barriers == 1);
    REQUIRE(f.runtime.finishes == 1);
    REQUIRE(f.runtime.allocations == 0);
    REQUIRE(f.bytes[0x10000] == 0x56);
    REQUIRE(f.bytes[0x20000] == 0x78);
    b.host_mapped = false;
    Access::Dirty(*f.cache, 0x20000, 0x1000);
    Access::Dirty(*f.cache, 0x30000, 0x1000);
    f.runtime.before_completion = [&] {
        REQUIRE(f.runtime.host_read_barriers == 2);
        REQUIRE(f.bytes[0x10000] == 0x56);
        REQUIRE(f.bytes[0x20000] == 0x78);
        std::ranges::fill(a.bytes, 0x9a);
        std::ranges::fill(b.bytes, 0xbc);
    };
    Access::Download(*f.cache, ranges);
    REQUIRE(f.runtime.waits == 0);
    REQUIRE(f.runtime.finishes == 2);
    REQUIRE(f.runtime.downloads == 1);
    REQUIRE(f.bytes[0x10000] == 0x9a);
    REQUIRE(f.bytes[0x20000] == 0xbc);
    Access::Download(*f.cache, ranges);
    REQUIRE(f.runtime.host_read_barriers == 2);
    REQUIRE(f.runtime.finishes == 2);
}

TEST_CASE("Single mapped download completes host visibility before guest publication",
          "[buffer_ports]") {
    Fixture f;
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000, true);
    auto& buffer = Access::Get(*f.cache, id);
    buffer.tick = 7;
    Access::Dirty(*f.cache, 0x20000, 0x1000);
    f.runtime.before_completion = [&] {
        REQUIRE(f.runtime.host_read_barriers == 1);
        REQUIRE(f.bytes[0x10000] == 0);
        std::ranges::fill(buffer.bytes, 0xcd);
    };
    f.cache->DownloadMemory(0x20000, 0x1000);
    REQUIRE(f.runtime.finishes == 1);
    REQUIRE(f.runtime.waits == 0);
    REQUIRE(f.runtime.allocations == 0);
    REQUIRE(f.bytes[0x10000] == 0xcd);
    f.cache->DownloadMemory(0x20000, 0x1000);
    REQUIRE(f.runtime.finishes == 1);
}

TEST_CASE("Command page batch publishes only completed matching write generations",
          "[buffer_ports]") {
    Fixture f;
    const bool mapped = GENERATE(false, true);
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000, mapped);
    std::ranges::fill(Access::Get(*f.cache, id).bytes, 0x42);
    Access::Dirty(*f.cache, 0x20000, 0x3000);
    Access::DrawWrite(*f.cache, 0x20000);
    Access::DrawWrite(*f.cache, 0x21000);
    Access::DrawWrite(*f.cache, 0x22000);
    std::array<u8, 4> out{};
    using Read = Cache::Nx2ListRead;
    f.runtime.before_completion = [&] {
        REQUIRE(f.runtime.host_read_barriers == 1);
        REQUIRE(f.bytes[0x10000] == 0);
        REQUIRE(f.cache->Nx2ReadListCopy(0x20000, out, true) == Read::Unknown);
        Access::DrawWrite(*f.cache, 0x22000);
    };
    const std::array<DAddr, 1> requested{0x20000};
    f.cache->Nx2FlushListPages(requested);
    REQUIRE(f.runtime.finishes == 1);
    REQUIRE(f.runtime.downloads == (mapped ? 0 : 3));
    REQUIRE(f.cache->Nx2ReadListCopy(0x20000, out, true) == Read::Synced);
    REQUIRE(f.cache->Nx2ReadListCopy(0x21000, out, true) == Read::Synced);
    REQUIRE(f.cache->Nx2ReadListCopy(0x22000, out, true) == Read::Unknown);
    REQUIRE(f.bytes[0x12000] == 0x42);
    REQUIRE(f.cache->Nx2ReadListCopy(0x21000, out, false) == Read::NeedsSync);
    Access::DrawWrite(*f.cache, 0x21000);
    REQUIRE(f.cache->Nx2ReadListCopy(0x21000, out, true) == Read::Unknown);
}

TEST_CASE("Command page readers remain ordered during batch completion", "[buffer_ports]") {
    struct {
        Core::DeviceMemory physical;
        Tegra::MaxwellDeviceMemoryManager device{physical};
        Runtime runtime;
        std::unique_ptr<Cache> cache = std::make_unique<Cache>(device, runtime);
    } f;
    Access::Add(*f.cache, 0x20000, 0x10000);
    Access::Dirty(*f.cache, 0x20000, 0x1000);
    Access::DrawWrite(*f.cache, 0x20000);
    std::barrier rendezvous{2};
    Cache::Nx2ListRead before{}, after{};
    std::jthread reader{[&] {
        std::array<u8, 4> out{};
        rendezvous.arrive_and_wait();
        before = f.cache->Nx2ReadListCopy(0x20000, out, true);
        rendezvous.arrive_and_wait();
        rendezvous.arrive_and_wait();
        after = f.cache->Nx2ReadListCopy(0x20000, out, true);
    }};
    f.runtime.before_completion = [&] {
        rendezvous.arrive_and_wait();
        rendezvous.arrive_and_wait();
    };
    const std::array<DAddr, 1> requested{0x20000};
    f.cache->Nx2FlushListPages(requested);
    rendezvous.arrive_and_wait();
    reader.join();
    REQUIRE(before == Cache::Nx2ListRead::Unknown);
    REQUIRE(after == Cache::Nx2ListRead::Synced);
    REQUIRE(f.runtime.finishes == 1);
    REQUIRE(f.runtime.downloads == 1);
}

TEST_CASE("Native uniform banks upload directly without overwriting GPU-owned bytes",
          "[buffer_ports]") {
    Fixture f;
    f.memory->EnableNativeTiling();
    Tegra::Control::ChannelState state{0};
    state.memory_manager = &*f.memory;
    state.payload.emplace(f.system, *f.memory, state);
    VideoCommon::BufferCacheChannelInfo channel{state};
    VideoCommon::UniformBufferSizes sizes{};
    sizes[0][0] = 0x10000;
    channel.uniform_buffer_sizes = &sizes;
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000);
    std::memset(f.bytes + 0x10000, 0x5a, 0x10000);
    Access::Uniform(*f.cache, channel, id, 0x20000, 0x10000);
    REQUIRE(f.runtime.upload_bytes == 0x10000);
    REQUIRE(Access::CpuMarks(*f.cache) == 0);
    REQUIRE(Access::Get(*f.cache, id).bytes[0xffff] == 0x5a);
    Access::Uniform(*f.cache, channel, id, 0x20000, 0x10000);
    REQUIRE(f.runtime.uploads == 1);
    Access::NextFrame(*f.cache);
    Access::Dirty(*f.cache, 0x21000, 0x1000);
    std::memset(f.bytes + 0x10000, 0xa5, 0x10000);
    Access::Uniform(*f.cache, channel, id, 0x20000, 0x10000);
    REQUIRE(f.runtime.uploads == 1);
    REQUIRE(Access::Get(*f.cache, id).bytes[0x1000] == 0x5a);
}

TEST_CASE("Native uniform uploads stop at holes and keep unaligned bank boundaries",
          "[buffer_ports]") {
    Fixture f;
    f.memory->EnableNativeTiling();
    Tegra::Control::ChannelState state{0};
    state.memory_manager = &*f.memory;
    state.payload.emplace(f.system, *f.memory, state);
    VideoCommon::BufferCacheChannelInfo channel{state};
    VideoCommon::UniformBufferSizes sizes{};
    sizes[0][0] = 0x10000;
    channel.uniform_buffer_sizes = &sizes;
    const auto id = Access::Add(*f.cache, 0x20000, 0x20000);
    std::memset(f.bytes + 0x10000, 0x6b, 0x20000);
    f.device.Unmap(0x22000, 0x1000);
    Access::Uniform(*f.cache, channel, id, 0x20123, 0x10000);
    REQUIRE(f.runtime.upload_bytes == 0x1edd);
    const auto& data = Access::Get(*f.cache, id).bytes;
    REQUIRE(data[0x122] == 0);
    REQUIRE(data[0x123] == 0x6b);
    REQUIRE(data[0x1fff] == 0x6b);
    REQUIRE(data[0x2000] == 0);
    REQUIRE(data[0x3000] == 0);
    REQUIRE(Access::CpuMarks(*f.cache) == 0);
    f.device.Map(0x22000, 0x22000, 0x1000, f.asid);
    Access::NextFrame(*f.cache);
    Access::Uniform(*f.cache, channel, id, 0x20123, 0x10000);
    REQUIRE(f.runtime.upload_bytes == 0x11edd);
    REQUIRE(data[0x10122] == 0x6b);
    REQUIRE(data[0x10123] == 0);
}

TEST_CASE("Downloads keep CPU writes collected and not yet applied", "[buffer_ports]") {
    Fixture f;
    const auto staged = Access::Add(*f.cache, 0x20000, 0x10000);
    const auto mapped = Access::Add(*f.cache, 0x30000, 0x10000, true);
    std::ranges::fill(Access::Get(*f.cache, staged).bytes, 0x12);
    std::ranges::fill(Access::Get(*f.cache, mapped).bytes, 0x34);
    f.cache->SetPendingCpuWrites([](const std::function<void(DAddr, size_t)>& callback) {
        callback(0x20100, 0x10);
        callback(0x30ff8, 0x10);
    });
    std::memset(f.bytes + 0x10100, 0xee, 0x10);
    std::memset(f.bytes + 0x20ff8, 0xee, 0x10);
    Access::Dirty(*f.cache, 0x20000, 0x2000);
    Access::Dirty(*f.cache, 0x30000, 0x2000);
    const std::array<std::pair<DAddr, u64>, 1> ranges{{{0x20000, 0x2000}}};
    Access::Download(*f.cache, ranges);
    REQUIRE(f.bytes[0x100ff] == 0x12);
    REQUIRE(std::all_of(f.bytes + 0x10100, f.bytes + 0x10110, [](u8 b) { return b == 0xee; }));
    REQUIRE(f.bytes[0x10110] == 0x12);
    REQUIRE(f.bytes[0x11fff] == 0x12);
    f.cache->DownloadMemory(0x30000, 0x2000);
    REQUIRE(f.bytes[0x20ff7] == 0x34);
    REQUIRE(std::all_of(f.bytes + 0x20ff8, f.bytes + 0x21008, [](u8 b) { return b == 0xee; }));
    REQUIRE(f.bytes[0x21008] == 0x34);
}

namespace {

/// Runs work on a thread marked as the GPU thread (the scheduler's owner), one call at a time.
class OwnerThread {
public:
    OwnerThread() : thread{[this](std::stop_token stop) { Run(stop); }} {
        std::unique_lock lock{mutex};
        ready.wait(lock, [&] { return marked; });
    }
    ~OwnerThread() {
        {
            std::scoped_lock lock{mutex};
            thread.request_stop();
        }
        wake.notify_all();
        thread.join();
    }
    /// Runs `work` on the owner and rethrows what it threw (a failed REQUIRE included).
    void Do(std::function<void()> work) {
        std::unique_lock lock{mutex};
        job = std::move(work);
        wake.notify_all();
        ready.wait(lock, [&] { return !job; });
        if (auto thrown = std::exchange(error, nullptr)) {
            std::rethrow_exception(thrown);
        }
    }

private:
    void Run(std::stop_token stop) {
        VideoCommon::GpuSplit::MarkGpuThread();
        SCOPE_EXIT {
            VideoCommon::GpuSplit::UnmarkGpuThread();
        };
        {
            std::scoped_lock lock{mutex};
            marked = true;
        }
        ready.notify_all();
        std::unique_lock lock{mutex};
        while (!stop.stop_requested()) {
            wake.wait(lock, [&] { return job || stop.stop_requested(); });
            if (job) {
                try {
                    job();
                } catch (...) {
                    error = std::current_exception();
                }
                job = nullptr;
                ready.notify_all();
            }
        }
    }

    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable ready;
    std::function<void()> job;
    std::exception_ptr error;
    bool marked{};
    std::jthread thread;
};

struct DeferredWriteFixture : Fixture {
    DeferredWriteFixture() {
        id = Access::Add(*cache, 0x20000, 0x10000);
        std::ranges::fill(Access::Get(*cache, id).bytes, 0x12);
        Access::Dirty(*cache, 0x20000, 0x1000);
        // The guest CPU writes 16 bytes in the middle of the GPU-written page.
        std::memset(bytes + 0x10100, 0xee, 0x10);
    }
    bool GuestHasGpuBytes() const {
        return bytes[0x10000] == 0x12 && bytes[0x100ff] == 0x12 && bytes[0x10110] == 0x12 &&
               bytes[0x10fff] == 0x12;
    }
    bool GuestKeepsCpuBytes() const {
        return std::all_of(bytes + 0x10100, bytes + 0x10110, [](u8 b) { return b == 0xee; });
    }
    VideoCommon::BufferId id;
};

} // namespace

TEST_CASE("Without a GPU thread a partial page write keeps the old behaviour",
          "[buffer_ports][deferred_page_writes]") {
    DeferredWriteFixture f;
    REQUIRE_FALSE(VideoCommon::GpuSplit::HasGpuThread());
    const u32 marks = Access::CpuMarks(*f.cache);
    f.cache->WriteMemory(0x20100, 0x10);
    CHECK(Access::Deferred(*f.cache) == 0);
    CHECK(Access::CpuMarks(*f.cache) == marks + 1);
    CHECK(f.runtime.downloads == 0);
}

TEST_CASE("A non-owner partial page write waits for the owner, which keeps the GPU bytes",
          "[buffer_ports][deferred_page_writes]") {
    DeferredWriteFixture f;
    OwnerThread owner;
    REQUIRE(VideoCommon::GpuSplit::HasGpuThread());
    REQUIRE_FALSE(VideoCommon::GpuSplit::OwnsScheduler());
    const u32 marks = Access::CpuMarks(*f.cache);

    f.cache->WriteMemory(0x20100, 0x10);
    CHECK(Access::Deferred(*f.cache) == 1);
    CHECK(Access::CpuMarks(*f.cache) == marks);
    CHECK(f.runtime.downloads == 0);
    // The CPU's bytes leave the GPU-written set at once; the rest of the page stays in it.
    CHECK_FALSE(Access::Pending(*f.cache, 0x20100, 0x10));
    CHECK(f.cache->IsRegionGpuModified(0x20000, 0x100));
    CHECK(f.cache->IsRegionGpuModified(0x20110, 0x100));

    // Another non-owner call doesn't apply it either.
    f.cache->DownloadMemory(0x20000, 0x1000);
    CHECK(Access::Deferred(*f.cache) == 1);

    SECTION("a download on the owner applies it first") {
        owner.Do([&] { f.cache->DownloadMemory(0x20000, 0x1000); });
    }
    SECTION("the owner's frame tick applies it") {
        owner.Do([&] { f.cache->TickFrame(); });
    }
    SECTION("an owner write elsewhere on the page applies it first") {
        std::memset(f.bytes + 0x10800, 0xdd, 4);
        owner.Do([&] { f.cache->WriteMemory(0x20800, 4); });
        CHECK(f.bytes[0x10800] == 0xdd);
    }
    SECTION("an owner write on a page without GPU data applies it first") {
        owner.Do([&] { f.cache->WriteMemory(0x40000, 4); });
    }
    CHECK(Access::Deferred(*f.cache) == 0);
    CHECK(f.runtime.downloads >= 1);
    CHECK(Access::CpuMarks(*f.cache) > marks);
    CHECK(f.GuestHasGpuBytes());
    CHECK(f.GuestKeepsCpuBytes());
}

TEST_CASE("An owner partial page write downloads the GPU bytes at once",
          "[buffer_ports][deferred_page_writes]") {
    DeferredWriteFixture f;
    OwnerThread owner;
    const u32 marks = Access::CpuMarks(*f.cache);
    owner.Do([&] { f.cache->WriteMemory(0x20100, 0x10); });
    CHECK(Access::Deferred(*f.cache) == 0);
    CHECK(Access::CpuMarks(*f.cache) == marks + 1);
    CHECK(f.GuestHasGpuBytes());
    CHECK(f.GuestKeepsCpuBytes());
}

TEST_CASE("Deferred page writes keep their order and skip pages already downloaded",
          "[buffer_ports][deferred_page_writes]") {
    DeferredWriteFixture f;
    OwnerThread owner;
    Access::Dirty(*f.cache, 0x23000, 0x1000);
    std::memset(f.bytes + 0x13000, 0x77, 8);
    f.cache->WriteMemory(0x20100, 0x10);
    f.cache->WriteMemory(0x23000, 8);
    // A write on a page without GPU data is applied at once.
    const u32 marks = Access::CpuMarks(*f.cache);
    f.cache->WriteMemory(0x25000, 8);
    CHECK(Access::CpuMarks(*f.cache) == marks + 1);
    CHECK(Access::Deferred(*f.cache) == 2);

    owner.Do([&] { f.cache->DownloadMemory(0x20000, 0x10000); });
    CHECK(Access::Deferred(*f.cache) == 0);
    CHECK(Access::CpuMarks(*f.cache) == marks + 3);
    CHECK(f.GuestHasGpuBytes());
    CHECK(f.GuestKeepsCpuBytes());
    CHECK(f.bytes[0x13000] == 0x77);
    CHECK(f.bytes[0x13007] == 0x77);
    CHECK(f.bytes[0x13008] == 0x12);
    CHECK_FALSE(Access::Pending(*f.cache, 0x20000, 0x10000));
}

TEST_CASE("Unmaps never wait for the owner", "[buffer_ports][deferred_page_writes]") {
    DeferredWriteFixture f;
    OwnerThread owner;
    const u32 marks = Access::CpuMarks(*f.cache);
    f.cache->WriteMemory(0x20000, 0x1000, false);
    CHECK(Access::Deferred(*f.cache) == 0);
    CHECK(Access::CpuMarks(*f.cache) == marks + 1);
    CHECK(f.runtime.downloads == 0);
}

TEST_CASE("The GPU thread mark follows the thread's lifetime",
          "[buffer_ports][deferred_page_writes]") {
    REQUIRE_FALSE(VideoCommon::GpuSplit::HasGpuThread());
    {
        OwnerThread first;
        CHECK(VideoCommon::GpuSplit::HasGpuThread());
        bool owns = false;
        first.Do([&] { owns = VideoCommon::GpuSplit::OwnsScheduler(); });
        CHECK(owns);
        CHECK_FALSE(VideoCommon::GpuSplit::OwnsScheduler());
    }
    CHECK_FALSE(VideoCommon::GpuSplit::HasGpuThread());
}

TEST_CASE("Deferred page writes merge only where they overlap or touch",
          "[buffer_ports][deferred_page_writes]") {
    using Ranges = std::vector<std::pair<DAddr, u64>>;
    DeferredWriteFixture f;
    OwnerThread owner;
    std::memset(f.bytes + 0x10100, 0xee, 0x20);
    std::memset(f.bytes + 0x10400, 0xee, 4);

    for (int i = 0; i < 3; ++i) {
        f.cache->WriteMemory(0x20100, 0x10);
    }
    CHECK(Access::DeferredRanges(*f.cache) == Ranges{{0x20100, 0x10}});
    f.cache->WriteMemory(0x20108, 0x10);
    CHECK(Access::DeferredRanges(*f.cache) == Ranges{{0x20100, 0x18}});
    f.cache->WriteMemory(0x20118, 8);
    CHECK(Access::DeferredRanges(*f.cache) == Ranges{{0x20100, 0x20}});
    f.cache->WriteMemory(0x200f8, 8);
    CHECK(Access::DeferredRanges(*f.cache) == Ranges{{0x200f8, 0x28}});
    // A gap holds GPU-written bytes the guest copy lacks, so a range across it can't merge.
    f.cache->WriteMemory(0x20400, 4);
    CHECK(Access::DeferredRanges(*f.cache) == Ranges{{0x200f8, 0x28}, {0x20400, 4}});

    owner.Do([&] { f.cache->DownloadMemory(0x20000, 0x1000); });
    CHECK(Access::Deferred(*f.cache) == 0);
    CHECK(f.bytes[0x10000] == 0x12);
    CHECK(f.bytes[0x10200] == 0x12);
    CHECK(f.bytes[0x10404] == 0x12);
    CHECK(f.bytes[0x10fff] == 0x12);
    CHECK(std::all_of(f.bytes + 0x10100, f.bytes + 0x10120, [](u8 b) { return b == 0xee; }));
    CHECK(std::all_of(f.bytes + 0x10400, f.bytes + 0x10404, [](u8 b) { return b == 0xee; }));
}

TEST_CASE("Deferred page writes queued from several threads are each applied once",
          "[buffer_ports][deferred_page_writes]") {
    DeferredWriteFixture f;
    OwnerThread owner;
    constexpr int Threads = 4;
    constexpr int WritesPerThread = 64;
    const u32 marks = Access::CpuMarks(*f.cache);
    std::atomic<bool> done{};
    {
        std::vector<std::jthread> writers;
        for (int t = 0; t < Threads; ++t) {
            writers.emplace_back([&, t] {
                for (int i = 0; i < WritesPerThread; ++i) {
                    // 8 bytes every 16: no two writes touch, so none merge.
                    Access::Defer(*f.cache, 0x20000 + (i * Threads + t) * 0x10, 8);
                }
            });
        }
        std::jthread drainer{[&] {
            while (!done.load(std::memory_order_acquire)) {
                owner.Do([&] { Access::ApplyDeferred(*f.cache); });
            }
        }};
        for (auto& writer : writers) {
            writer.join();
        }
        done.store(true, std::memory_order_release);
    }
    owner.Do([&] { Access::ApplyDeferred(*f.cache); });
    CHECK(Access::Deferred(*f.cache) == 0);
    CHECK(Access::CpuMarks(*f.cache) == marks + Threads * WritesPerThread);
}

TEST_CASE("A failure on the owner thread reaches the test and leaves the owner usable",
          "[buffer_ports][deferred_page_writes]") {
    OwnerThread owner;
    CHECK_THROWS_AS(owner.Do([] { throw std::runtime_error{"owner job failed"}; }),
                    std::runtime_error);
    bool ran = false;
    owner.Do([&] { ran = VideoCommon::GpuSplit::OwnsScheduler(); });
    CHECK(ran);
}