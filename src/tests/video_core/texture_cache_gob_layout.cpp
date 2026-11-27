// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/control/channel_state.h"
#include "video_core/gpu.h"
#include "video_core/renderer_null/null_rasterizer.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/textures/decoders.h"

namespace {

struct TestAsyncBuffer {
    std::span<u8> mapped_span;
    size_t offset{};
};

struct TestRuntime {
    TestAsyncBuffer DownloadStagingBuffer(size_t size, bool deferred = false) {
        max_download_bytes = (std::max)(max_download_bytes, size);
        ++downloads;
        return AllocateStaging(size);
    }
    std::optional<TestAsyncBuffer> TryGetCollectorDownloadBuffer(size_t size) {
        if (!collector_buffer) {
            ++collector_reservation_attempts;
            if (fail_collector_reservation) {
                return std::nullopt;
            }
            collector_buffer = DownloadStagingBuffer(size, true);
        }
        REQUIRE(collector_buffer->mapped_span.size() >= size);
        return collector_buffer;
    }
    TestAsyncBuffer AllocateStaging(size_t size) {
        if (fail_staging_allocation) {
            throw std::bad_alloc{};
        }
        staging.push_back(std::make_unique<std::vector<u8>>(size));
        return {*staging.back()};
    }
    void FreeDeferredStagingBuffer(TestAsyncBuffer& map) {
        REQUIRE(
            (!collector_buffer || collector_buffer->mapped_span.data() != map.mapped_span.data()));
        REQUIRE(std::ranges::any_of(
            staging, [&](const auto& buffer) { return buffer->data() == map.mapped_span.data(); }));
        std::erase_if(staging, [&](const auto& buffer) {
            return buffer->data() == map.mapped_span.data();
        });
        freed_staging_tick = CurrentTick();
    }
    TestAsyncBuffer UploadStagingBuffer(size_t size, bool deferred = false) {
        REQUIRE(deferred);
        return AllocateStaging(size);
    }
    void AccelerateImageUpload(auto&, const TestAsyncBuffer&,
                               std::span<const VideoCommon::SwizzleParameters>, u32, u32) {
        ++unswizzle_uploads;
    }
    void InsertUploadMemoryBarrier() {}
    bool CanReportMemoryUsage() const { return true; }
    u64 GetDeviceMemoryUsage() const { return 0; }
    void TickFrame() {}
    u64 CurrentTick() const { return current_tick; }
    bool IsFree(u64 tick) const { return completed_tick >= tick; }
    void FlushDeferredClear() {}
    void Finish() {
        REQUIRE(allow_wait);
        ++waits;
        completed_tick = current_tick;
    }

    bool HasBrokenTextureViewFormats() const {
        return false;
    }
    bool HasNativeBgr() const {
        return false;
    }
    std::vector<std::unique_ptr<std::vector<u8>>> staging;
    std::optional<TestAsyncBuffer> collector_buffer;
    u32 collector_reservation_attempts{};
    bool fail_collector_reservation{};
    bool fail_staging_allocation{};
    u64 current_tick{1};
    u64 completed_tick{};
    u64 freed_staging_tick{};
    u32 downloads{};
    u32 unswizzle_uploads{};
    u32 waits{};
    bool allow_wait{};
    size_t max_download_bytes{};
};

struct TestImage : VideoCommon::ImageBase {
    TestImage(TestRuntime&, const VideoCommon::ImageInfo& image_info, GPUVAddr gpu_address,
              DAddr cpu_address)
        : ImageBase{image_info, gpu_address, cpu_address} {}
    explicit TestImage(const VideoCommon::NullImageParams& params) : ImageBase{params} {
        if (fail_holder_allocation) {
            throw std::bad_alloc{};
        }
    }
    static inline bool fail_holder_allocation{};
    TestImage(TestImage&&) = default;
    TestImage& operator=(TestImage&&) = default;
    void DownloadMemory(const TestAsyncBuffer& map,
                        std::span<const VideoCommon::BufferImageCopy> copies) {
        auto dest = map.mapped_span.subspan(map.offset);
        if (dest.size() >= gpu_bytes.size()) {
            std::ranges::copy(gpu_bytes, dest.begin());
            return;
        }
        // A compact download of a rectangle of the D16 test image.
        REQUIRE(info.format == VideoCore::Surface::PixelFormat::D16_UNORM);
        REQUIRE(copies.size() == 1);
        const auto& copy = copies.front();
        REQUIRE(copy.image_subresource.base_level == 0);
        REQUIRE(copy.image_subresource.base_layer == 0);
        REQUIRE(copy.image_subresource.num_layers == 1);
        const u32 pitch = copy.image_extent.width * 2;
        REQUIRE((copy.buffer_row_length == 0 || copy.buffer_row_length == copy.image_extent.width));
        REQUIRE(copy.image_offset.x >= 0);
        REQUIRE(copy.image_offset.y >= 0);
        REQUIRE(copy.buffer_offset + size_t{pitch} * copy.image_extent.height <= dest.size());
        for (u32 row = 0; row < copy.image_extent.height; ++row) {
            const size_t offset = (static_cast<size_t>(copy.image_offset.y + row) * info.size.width +
                                   copy.image_offset.x) * 2;
            REQUIRE(offset + pitch <= gpu_bytes.size());
            std::copy_n(gpu_bytes.data() + offset, pitch,
                        dest.data() + copy.buffer_offset + size_t{row} * pitch);
        }
    }
    void UploadMemory(const TestAsyncBuffer&, std::span<const VideoCommon::BufferImageCopy>) {
        FAIL("Unexpected CPU decode upload");
    }
    std::vector<u8> gpu_bytes;
    std::shared_ptr<int> lifetime;
};

struct TestImageView : VideoCommon::ImageViewBase {
    TestImageView(TestRuntime&, const VideoCommon::ImageViewInfo& info, VideoCommon::ImageId id,
                  TestImage& image, const Common::SlotVector<TestImage>&)
        : ImageViewBase{info, image.info, id, image.gpu_addr} {}
    TestImageView(TestRuntime&, const VideoCommon::ImageInfo& info,
                  const VideoCommon::ImageViewInfo& view_info, GPUVAddr addr)
        : ImageViewBase{info, view_info, addr} {}
    TestImageView(TestRuntime&, const VideoCommon::ImageInfo& info,
                  const VideoCommon::ImageViewInfo& view_info)
        : ImageViewBase{info, view_info, 0} {}
    TestImageView(TestRuntime&, const VideoCommon::NullImageViewParams& params)
        : ImageViewBase{params} {}
    TestImageView(TestImageView&&) = default;
    TestImageView& operator=(TestImageView&&) = default;
};

struct TestSampler {
    TestSampler(TestRuntime&, const Tegra::Texture::TSCEntry&) {}
};

struct TestImageAlloc {
    std::vector<VideoCommon::ImageId> images;
};

struct TestFramebuffer {};
struct TestBufferType {};

struct Policy {
    using Runtime = TestRuntime;
    using Image = TestImage;
    using ImageAlloc = TestImageAlloc;
    using ImageView = TestImageView;
    using Sampler = TestSampler;
    using Framebuffer = TestFramebuffer;
    using AsyncBuffer = TestAsyncBuffer;
    using BufferType = TestBufferType;
    static constexpr bool ENABLE_VALIDATION = false;
    static constexpr bool FRAMEBUFFER_BLITS = false;
    static constexpr bool HAS_EMULATED_COPIES = false;
    static constexpr bool HAS_DEVICE_MEMORY_INFO = false;
    static constexpr bool IMPLEMENTS_ASYNC_DOWNLOADS = true;
    static constexpr bool HAS_MSAA_DOWNLOADS = false;
};

using Cache = VideoCommon::TextureCache<Policy>;

VideoCommon::ImageInfo MakeInfo() {
    VideoCommon::ImageInfo info{};
    info.type = VideoCommon::ImageType::e2D;
    info.format = VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM;
    info.size = {64, 64, 1};
    info.resources = {1, 1};
    info.block = {0, 0, 0};
    info.num_samples = 1;
    return info;
}

} // namespace

namespace VideoCommon {

struct TextureCacheTestAccess {
    template <class C, class F>
    static void ForEachSparseSegment(C& cache, ImageBase& image, F&& func) {
        cache.ForEachSparseSegment(image, std::forward<F>(func));
    }
    template <class C>
    static ImageId Insert(C& cache, const ImageInfo& info, GPUVAddr gpu_addr, DAddr cpu_addr) {
        const auto id = cache.slot_images.insert(cache.runtime, info, gpu_addr, cpu_addr);
        cache.RegisterImage(id);
        return id;
    }
    template <class C>
    static ImageId InsertForCollection(C& cache, const ImageInfo& info, GPUVAddr gpu, DAddr cpu) {
        const auto id = Insert(cache, info, gpu, cpu);
        const auto [it, is_new] = cache.image_allocs_table.try_emplace(gpu);
        if (is_new) {
            it->second = cache.slot_image_allocs.insert();
        }
        cache.slot_image_allocs[it->second].images.push_back(id);
        return id;
    }
    template <class C>
    static ImageId Find(C& cache, const ImageInfo& info, GPUVAddr gpu_addr) {
        bool retry = false;
        return cache.FindImage(info, gpu_addr, RelaxedOptions{}, &retry);
    }
    template <class C>
    static void WriteGpu(C& cache, ImageId id, std::vector<u8> bytes) {
        auto& image = cache.slot_images[id];
        image.gpu_bytes = std::move(bytes);
        image.flags &= ~ImageFlagBits::CpuModified;
        cache.MarkModification(image);
    }
    template <class C>
    static void PollWritebacks(C& cache) {
        cache.FinishAsyncOverlapDownloads(false);
    }
    template <class C>
    static bool CollectDownload(C& cache, ImageId id) {
        return cache.QueueCollectorDownload(id) == C::CollectorQueueResult::Queued;
    }
    template <class C>
    static void FinishWritebacks(C& cache) {
        cache.FinishAsyncOverlapDownloads(true);
    }
    template <class C>
    static void CollectPass(C& cache, bool out_of_memory = false, u64 frame_tick = 10000) {
        cache.frame_tick = frame_tick;
        cache.expected_memory = 1;
        cache.critical_memory = ~u64{0};
        cache.RunGarbageCollector(out_of_memory, false);
    }
    template <class C>
    static bool Contains(const C& cache, ImageId id) {
        return cache.slot_images.contains(id);
    }
    template <class C>
    static auto CollectorState(const C& cache) {
        return std::tuple{cache.collector_count, cache.collector_held_bytes,
                          cache.async_overlap_downloads.size()};
    }
    template <class C>
    static void MarkStale(C& cache, const ImageBase& image) {
        cache.MarkStale(image);
    }
    template <class C>
    static bool Stale(const C& cache, DAddr addr, u64 size) {
        return cache.stale_pages.IsAll(addr, size);
    }
    template <class C>
    static void NoteWriteback(C& cache, const ImageBase& image) {
        cache.NoteOverlapWriteback(image);
    }
    template <class C>
    static auto History(const C& cache) {
        std::vector<std::tuple<DAddr, DAddr, DAddr, u64, bool>> result;
        for (const auto& [page, ranges] : cache.overlap_writeback_ranges) {
            for (const auto& range : ranges) {
                result.emplace_back(page, range.begin, range.end, range.tick, range.lost);
            }
        }
        std::ranges::sort(result);
        return result;
    }
    template <class C>
    static void NoteFlushed(C& cache, const ImageBase& image, u64 tick) {
        cache.NoteFlushed(image, tick);
    }
    template <class C>
    static auto WritebackPageCounts(const C& cache) {
        return std::pair{cache.overlap_writeback_ticks.size(),
                         cache.overlap_writeback_ranges.size()};
    }
    template <class C>
    static const ImageInfo& Info(C& cache, ImageId id) {
        return cache.slot_images[id].info;
    }
    template <class C>
    static void CacheLutView(C& cache, ImageId id, const Tegra::Texture::TICEntry& tic) {
        auto& channel = *cache.channel_state;
        auto& image = cache.slot_images[id];
        const auto view = cache.FindOrEmplaceImageView(id, ImageViewInfo{tic, 0});
        image.xc2_lut_view = true;
        channel.image_views.emplace(tic, view);
        cache.image_view_descriptors[view].emplace_back(cache.current_channel_id, tic);
    }
    template <class C>
    static bool HasCachedView(C& cache, const Tegra::Texture::TICEntry& tic) {
        return cache.channel_state->image_views.contains(tic);
    }
    template <class C>
    static u64 DescriptorRevision(C& cache) {
        return cache.channel_state->graphics_image_table.changes;
    }
    template <class C>
    static void LinkOverlap(C& cache, ImageId left, ImageId right) {
        cache.slot_images[left].overlapping_images.push_back(right);
        cache.slot_images[right].overlapping_images.push_back(left);
    }
    template <class C>
    static void Delete(C& cache, ImageId id) {
        const auto alloc = cache.slot_image_allocs.insert();
        cache.image_allocs_table.emplace(cache.slot_images[id].gpu_addr, alloc);
        cache.slot_image_allocs[alloc].images.push_back(id);
        cache.UnregisterImage(id);
        cache.DeleteImage(id);
    }
    template <class C>
    static auto& Image(C& cache, ImageId id) {
        return cache.slot_images[id];
    }
    template <class C>
    static void QueueDownload(C& cache, ImageId id) {
        cache.uncommitted_downloads.push_back({.is_swizzle = true, .object_id = id});
    }
    template <class C>
    static void QueueUnswizzle(C& cache, ImageId id) {
        cache.QueueAsyncUnswizzle(cache.slot_images[id], id);
    }
    template <class C>
    static void ProcessOneUnswizzleSlice(C& cache) {
        cache.swizzle_chunk_size = 512;
        cache.swizzle_slices_per_batch = 1;
        cache.TickAsyncUnswizzle();
    }
    template <class C>
    static std::vector<ImageId> QueuedUnswizzles(const C& cache) {
        std::vector<ImageId> ids;
        for (const auto& task : cache.unswizzle_queue) {
            ids.push_back(task.image_id);
        }
        return ids;
    }
};

} // namespace VideoCommon

namespace {

using Access = VideoCommon::TextureCacheTestAccess;

struct Fixture {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device{physical};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    TestRuntime runtime;
    std::unique_ptr<Cache> cache;
    std::unique_ptr<Kernel::KProcess> process;
    Core::Asid asid{};
    std::shared_ptr<Tegra::MemoryManager> memory;
    Tegra::Control::ChannelState state{0};
    u8* bytes{};
    size_t backing_size;

    explicit Fixture(u32 gpu_mapping_size = 0x10000)
        : backing_size{(std::max)(gpu_mapping_size, 0x40000U)} {
        system.Initialize();
        process = std::make_unique<Kernel::KProcess>(system.Kernel());
        auto& table = process->GetPageTable().GetImpl();
        table.Resize(gpu_mapping_size > (1U << 26)   ? 27
                     : gpu_mapping_size > (1U << 23) ? 26
                                                     : 24,
                     12);
        bytes = physical.GetPointerFromRaw<u8>(0x10000);
        std::memset(bytes, 0, backing_size);
        for (u64 page = 0; page < backing_size / 4096; ++page) {
            table.entries.GetAndFault(0x10 + page)
                .Store(false, Common::PageType::Memory, 1,
                       reinterpret_cast<uintptr_t>(bytes) - 0x10000);
        }
        process->GetMemory().SetCurrentPageTable(*process);
        asid = device.RegisterProcess(&process->GetMemory());
        device.BindInterface(&rasterizer);
        device.Map(0x10000, 0x10000, backing_size, asid);
        memory = std::make_shared<Tegra::MemoryManager>(system, device, 32, 1ULL << 31, 16);
        memory->BindRasterizer(&rasterizer);
        memory->EnableNativeTiling();
        memory->Map(0x100000, 0x10000, gpu_mapping_size, Tegra::PTEKind::PITCH, true);
        state.memory_manager = memory.get();
        state.payload.emplace(system, *memory, state);
        cache = std::make_unique<Cache>(runtime, device);
        cache->CreateChannel(state);
        cache->BindToChannel(0);
    }
    ~Fixture() {
        cache.reset();
        memory.reset();
        device.Unmap(0x10000, backing_size);
        device.UnregisterProcess(asid);
        system.Kernel().UnregisterKernelObject(process.get());
    }
};

} // namespace

TEST_CASE("An evicted depth image survives slot reuse and intervening byte writes",
          "[video_core][texture_cache][collector_writeback]") {
    using namespace VideoCommon;
    constexpr u32 size = 32U << 20;
    constexpr DAddr cpu = 0x10000;
    constexpr GPUVAddr gpu = 0x100000;
    Fixture f{size + 0x10000};
    auto& cache = *f.cache;
    auto info = MakeInfo();
    info.format = PixelFormat::D16_UNORM;
    info.size = {4096, 4096, 1};
    info.block = {0, 5, 0};
    info.native_tiling = true;
    REQUIRE(CalculateGuestSizeInBytes(info) == size);
    const auto id = Access::Insert(cache, info, gpu, cpu);
    std::vector<u8> linear(size);
    for (size_t i = 0; i < linear.size(); ++i) {
        linear[i] = static_cast<u8>(0x40 + (i ^ (i >> 12) ^ (i >> 17)) % 0x80);
    }
    std::vector<u8> expected(size);
    Tegra::Texture::SwizzleTexture(expected, linear, 2, 4096, 4096, 1, 5, 0, 0, true);
    Access::WriteGpu(cache, id, std::move(linear));
    REQUIRE(Access::CollectDownload(cache, id));
    Access::Delete(cache, id);
    REQUIRE(Access::Insert(cache, MakeInfo(), gpu + size + 0x20000, cpu + 0x2000) == id);

    // Let the first transfer finish, with the tail of the old allocation still pending.
    Access::PollWritebacks(cache);
    for (u32 i = 0; i < 3 && f.bytes[0] == 0; ++i) {
        f.runtime.completed_tick = f.runtime.current_tick++;
        Access::PollWritebacks(cache);
    }
    REQUIRE(f.bytes[0] == expected[0]);
    REQUIRE(f.bytes[size - 1] == 0);
    CHECK(f.runtime.waits == 0);

    SECTION("An unrelated flush creates no history when ordinary overlap writebacks are off") {
        const auto saved_program_id = Settings::GetCurrentProgramID();
        SCOPE_EXIT { Settings::SetCurrentProgramID(saved_program_id); };
        Settings::SetCurrentProgramID(0);
        const auto pages = Access::WritebackPageCounts(cache);
        const TestImage unrelated{f.runtime, MakeInfo(), gpu + size, cpu + size};
        Access::NoteFlushed(cache, unrelated, 100);
        CHECK(Access::WritebackPageCounts(cache) == pages);

        // A newer image has already written this part of the pending depth image to the guest.
        constexpr u32 offset = size / 2;
        const TestImage overlapping{f.runtime, MakeInfo(), gpu + offset, cpu + offset};
        std::fill_n(f.bytes + offset, overlapping.guest_size_bytes, 0xE7);
        std::fill_n(expected.begin() + offset, overlapping.guest_size_bytes, 0xE7);
        Access::NoteFlushed(cache, overlapping, 101);
    }
    SECTION("An overlapping guest read combines scattered stores with the old GPU image") {
        for (u32 i = 0; i < 40; ++i) {
            const u32 offset = size / 2 + i * 71;
            expected[offset] = static_cast<u8>(i);
            if (i % 2 == 0) {
                f.bytes[offset] = expected[offset];
                cache.WriteMemory(cpu + offset, 1);
            } else {
                cache.WriteMemory(cpu + offset, 1);
                f.bytes[offset] = expected[offset];
            }
        }
    }
    SECTION("A partially unmapped allocation keeps the bytes still mapped") {
        constexpr u32 offset = size / 2;
        constexpr u32 span = 0x10000;
        cache.UnmapGPUMemory(f.memory->GetID(), gpu + offset, span);
        f.memory->Unmap(gpu + offset, span);
        std::fill_n(f.bytes + offset, span, 0xD3);
        std::fill_n(expected.begin() + offset, span, 0xD3);
    }
    SECTION("Fully unmapping a pending image stops its remaining transfers") {
        const auto downloads = f.runtime.downloads;
        cache.UnmapGPUMemory(f.memory->GetID(), gpu, size);
        f.runtime.completed_tick = f.runtime.current_tick++;
        Access::PollWritebacks(cache);
        CHECK(f.runtime.downloads == downloads);
        CHECK(f.bytes[size - 1] == 0);
        return;
    }
    f.runtime.allow_wait = true;
    cache.DownloadMemory(cpu + size - 1, 1);
    CHECK(f.runtime.max_download_bytes <= (4U << 20));
    CHECK(std::equal(expected.begin(), expected.end(), f.bytes));
}

TEST_CASE("An older collector queued behind newer data preserves that data",
          "[video_core][texture_cache][collector_writeback]") {
    using namespace VideoCommon;
    constexpr u32 size = 32U << 20;
    constexpr DAddr cpu = 0x10000;
    constexpr GPUVAddr gpu = 0x100000;
    Fixture f{size + 0x10000};
    auto& cache = *f.cache;
    auto info = MakeInfo();
    info.format = PixelFormat::D16_UNORM;
    info.size = {4096, 4096, 1};
    info.block = {0, 5, 0};
    info.native_tiling = true;
    const auto older = Access::Insert(cache, info, gpu, cpu);
    Access::WriteGpu(cache, older, std::vector<u8>(size, 0x28));
    const auto newer = Access::Insert(cache, info, gpu, cpu);
    Access::WriteGpu(cache, newer, std::vector<u8>(size, 0xC3));
    REQUIRE(Access::CollectDownload(cache, newer));
    Access::Delete(cache, newer);
    SECTION("The newer image has not started landing") {}
    SECTION("The newer image has already landed its first chunk") {
        Access::PollWritebacks(cache);
        f.runtime.completed_tick = f.runtime.current_tick++;
        Access::PollWritebacks(cache);
        REQUIRE(f.bytes[0] == 0xC3);
        REQUIRE(f.bytes[size - 1] == 0);
    }
    REQUIRE_FALSE(Access::CollectDownload(cache, older));
    const auto downloads = f.runtime.downloads;
    Access::CollectPass(cache);
    CHECK(Access::Contains(cache, older));
    CHECK(f.runtime.downloads == downloads);
    f.runtime.allow_wait = true;
    Access::FinishWritebacks(cache);
    REQUIRE(Access::CollectDownload(cache, older));
    Access::Delete(cache, older);
    Access::FinishWritebacks(cache);
    CHECK(std::all_of(f.bytes, f.bytes + size, [](u8 byte) { return byte == 0xC3; }));
    CHECK(Access::CollectorState(cache) == std::tuple{size_t{0}, u64{0}, size_t{0}});
}

TEST_CASE("A pending collector protects its landed prefix from an older color flush",
          "[video_core][texture_cache][collector_writeback]") {
    using namespace VideoCommon;
    constexpr u32 size = 32U << 20;
    constexpr u32 first_chunk = 2U << 20;
    constexpr DAddr cpu = 0x10000;
    constexpr GPUVAddr gpu = 0x100000;
    Fixture f{size + 0x10000};
    auto& cache = *f.cache;
    auto color = MakeInfo();
    color.size = {4096, 2048, 1};
    color.block = {0, 5, 0};
    color.native_tiling = true;
    const auto older = Access::Insert(cache, color, gpu, cpu);
    Access::WriteGpu(cache, older, std::vector<u8>(size, 0x28));
    Access::QueueDownload(cache, older);
    cache.CommitAsyncFlushes();

    auto depth = color;
    depth.format = PixelFormat::D16_UNORM;
    depth.size = {4096, 4096, 1};
    const auto newer = Access::Insert(cache, depth, gpu, cpu);
    Access::WriteGpu(cache, newer, std::vector<u8>(size, 0xC3));
    REQUIRE(Access::CollectDownload(cache, newer));
    Access::Delete(cache, newer);
    Access::PollWritebacks(cache);
    f.runtime.completed_tick = f.runtime.current_tick++;
    Access::PollWritebacks(cache);
    REQUIRE(f.bytes[0] == 0xC3);
    REQUIRE(f.bytes[first_chunk] == 0);

    // This non-D16 image cannot use collector chunking either. The real GC must retain it.
    const auto downloads = f.runtime.downloads;
    Access::CollectPass(cache);
    REQUIRE(Access::Contains(cache, older));
    CHECK(f.runtime.downloads == downloads);
    cache.PopAsyncFlushes();
    CHECK(std::all_of(f.bytes, f.bytes + first_chunk, [](u8 byte) { return byte == 0xC3; }));
    CHECK(std::all_of(f.bytes + first_chunk, f.bytes + size,
                      [](u8 byte) { return byte == 0x28; }));
    f.runtime.allow_wait = true;
    Access::FinishWritebacks(cache);
    CHECK(std::all_of(f.bytes, f.bytes + size, [](u8 byte) { return byte == 0xC3; }));
}

TEST_CASE("Retained writebacks leave room for a clean eviction in the same GC pass",
          "[video_core][texture_cache][collector_writeback]") {
    using namespace VideoCommon;
    constexpr u32 size = 32U << 20;
    constexpr DAddr cpu = 0x10000;
    constexpr GPUVAddr gpu = 0x100000;
    Fixture f{size + 0x10000};
    auto& cache = *f.cache;
    auto depth = MakeInfo();
    depth.format = PixelFormat::D16_UNORM;
    depth.size = {4096, 4096, 1};
    depth.block = {0, 5, 0};
    depth.native_tiling = true;
    const auto source = Access::Insert(cache, depth, gpu, cpu);
    Access::WriteGpu(cache, source, std::vector<u8>(size, 0x42));
    REQUIRE(Access::CollectDownload(cache, source));
    Access::Delete(cache, source);

    // These small dirty images all overlap the queued source. Their age puts them before
    // the clean image, and their number exceeds the high-priority pass's 20 evictions.
    std::vector<ImageId> held;
    for (u32 i = 0; i < 21; ++i) {
        const auto offset = i * 0x4000;
        const auto id = Access::InsertForCollection(cache, MakeInfo(), gpu + offset, cpu + offset);
        Access::WriteGpu(cache, id, std::vector<u8>(0x4000, 0x27));
        held.push_back(id);
    }
    const auto clean = Access::InsertForCollection(cache, MakeInfo(), gpu + size, cpu + size);
    Access::CollectPass(cache);
    CHECK_FALSE(Access::Contains(cache, clean));
    CHECK(std::ranges::all_of(held, [&](ImageId id) { return Access::Contains(cache, id); }));
}

TEST_CASE("GC can still reclaim clean images after a writeback allocation fails",
          "[video_core][texture_cache][collector_writeback]") {
    using namespace VideoCommon;
    constexpr u32 size = 32U << 20;
    constexpr DAddr cpu = 0x10000;
    constexpr GPUVAddr gpu = 0x100000;
    Fixture f{size + 0x10000};
    auto& cache = *f.cache;
    auto info = MakeInfo();
    info.format = PixelFormat::D16_UNORM;
    info.size = {4096, 4096, 1};
    info.block = {0, 5, 0};
    info.native_tiling = true;
    std::array<ImageId, 3> held;
    for (auto& id : held) {
        id = Access::InsertForCollection(cache, info, gpu, cpu);
        Access::WriteGpu(cache, id, {});
    }
    SECTION("The Vulkan staging reservation is unavailable") {
        f.runtime.fail_collector_reservation = true;
    }
    SECTION("Host allocation fails") {
        f.runtime.fail_staging_allocation = true;
    }
    Access::CollectPass(cache);
    CHECK(f.runtime.collector_reservation_attempts == 1);
    const auto clean = Access::InsertForCollection(cache, MakeInfo(), gpu + size, cpu + size);
    Access::CollectPass(cache, false, 20000);
    CHECK_FALSE(Access::Contains(cache, clean));
    CHECK(std::ranges::all_of(held, [&](ImageId id) { return Access::Contains(cache, id); }));
}

TEST_CASE("Collector landing clears stale pages only when it writes them",
          "[video_core][texture_cache][collector_writeback]") {
    using namespace VideoCommon;
    constexpr u32 size = 32U << 20;
    constexpr DAddr cpu = 0x10000;
    constexpr GPUVAddr gpu = 0x100000;
    const bool old_ampere = Tegra::g_nx2_ampere.exchange(true);
    SCOPE_EXIT { Tegra::g_nx2_ampere.store(old_ampere); };
    Fixture f{size + 0x10000};
    auto& cache = *f.cache;
    auto info = MakeInfo();
    info.format = PixelFormat::D16_UNORM;
    info.size = {4096, 4096, 1};
    info.block = {0, 5, 0};
    info.native_tiling = true;
    const auto id = Access::Insert(cache, info, gpu, cpu);
    Access::WriteGpu(cache, id, std::vector<u8>(size, 0x45));
    Access::MarkStale(cache, Access::Image(cache, id));
    REQUIRE(Access::Stale(cache, cpu, 4096));
    REQUIRE(Access::CollectDownload(cache, id));
    Access::Delete(cache, id);
    Access::PollWritebacks(cache);
    f.runtime.completed_tick = f.runtime.current_tick++;
    Access::PollWritebacks(cache);
    REQUIRE(f.bytes[0] == 0x45);
    CHECK_FALSE(Access::Stale(cache, cpu, 4096));

    auto page_info = MakeInfo();
    page_info.size = {32, 32, 1};
    ImageBase later_lost{page_info, gpu, cpu};
    later_lost.flags = ImageFlagBits::GpuModified;
    Access::MarkStale(cache, later_lost);
    constexpr u32 kept_offset = size / 2;
    cache.WriteMemory(cpu + kept_offset, 4096);
    std::fill_n(f.bytes + kept_offset, 4096, 0xE9);
    later_lost.cpu_addr = cpu + kept_offset;
    Access::MarkStale(cache, later_lost);
    REQUIRE(Access::Stale(cache, cpu + kept_offset, 4096));

    f.runtime.allow_wait = true;
    cache.DownloadMemory(cpu + size - 1, 1);
    CHECK(Access::Stale(cache, cpu, 4096));
    CHECK(Access::Stale(cache, cpu + kept_offset, 4096));
    CHECK_FALSE(Access::Stale(cache, cpu + size - 4096, 4096));
    CHECK(std::all_of(f.bytes + kept_offset, f.bytes + kept_offset + 4096,
                      [](u8 byte) { return byte == 0xE9; }));
    CHECK(f.bytes[size - 1] == 0x45);
}

TEST_CASE("Collector allocation failure leaves the live image and history intact",
          "[video_core][texture_cache][collector_writeback]") {
    using namespace VideoCommon;
    constexpr u32 size = 32U << 20;
    constexpr DAddr cpu = 0x10000;
    constexpr GPUVAddr gpu = 0x100000;
    Fixture f{size + 0x10000};
    auto& cache = *f.cache;
    auto info = MakeInfo();
    info.format = PixelFormat::D16_UNORM;
    info.size = {4096, 4096, 1};
    info.block = {0, 5, 0};
    info.native_tiling = true;
    const auto id = Access::Insert(cache, info, gpu, cpu);
    Access::WriteGpu(cache, id, std::vector<u8>(size, 0x65));
    ImageBase existing{MakeInfo(), gpu + 8192, cpu + 8192};
    Access::NoteWriteback(cache, existing);
    const auto pages = Access::WritebackPageCounts(cache);
    const auto history = Access::History(cache);
    const auto watched = f.device.IsRangeCached(cpu, size);
    // Fail after reserving all history nodes, but before publishing the collector entry.
    TestImage::fail_holder_allocation = true;
    SCOPE_EXIT { TestImage::fail_holder_allocation = false; };
    REQUIRE_THROWS_AS(Access::CollectDownload(cache, id), std::bad_alloc);
    CHECK(Access::WritebackPageCounts(cache) == pages);
    CHECK(Access::History(cache) == history);
    CHECK(Access::CollectorState(cache) == std::tuple{size_t{0}, u64{0}, size_t{0}});
    CHECK(f.device.IsRangeCached(cpu, size) == watched);
    CHECK(Access::Image(cache, id).gpu_bytes.size() == size);
    CHECK(f.runtime.downloads == 1);
    REQUIRE_NOTHROW(Access::CollectPass(cache));
    CHECK(Access::Contains(cache, id));
    CHECK(Access::WritebackPageCounts(cache) == pages);
    CHECK(Access::History(cache) == history);
    CHECK(Access::CollectorState(cache) == std::tuple{size_t{0}, u64{0}, size_t{0}});
    CHECK(f.runtime.downloads == 1);
    TestImage::fail_holder_allocation = false;
    REQUIRE(Access::CollectDownload(cache, id));
    Access::Delete(cache, id);
    f.runtime.allow_wait = true;
    cache.DownloadMemory(cpu, size);
    CHECK(std::all_of(f.bytes, f.bytes + size, [](u8 byte) { return byte == 0x65; }));
}

TEST_CASE("Collector staging is reserved before eviction and reused through every drain",
          "[video_core][texture_cache][collector_writeback]") {
    using namespace VideoCommon;
    constexpr u32 size = 32U << 20;
    constexpr DAddr cpu = 0x10000;
    constexpr GPUVAddr gpu = 0x100000;
    Fixture f{size + 0x10000};
    auto& cache = *f.cache;
    auto info = MakeInfo();
    info.format = PixelFormat::D16_UNORM;
    info.size = {4096, 4096, 1};
    info.block = {0, 5, 0};
    info.native_tiling = true;
    const auto id = Access::Insert(cache, info, gpu, cpu);
    Access::WriteGpu(cache, id, std::vector<u8>(size, 0x65));
    const auto watched = f.device.IsRangeCached(cpu, size);
    const auto pages = Access::WritebackPageCounts(cache);
    const auto history = Access::History(cache);

    f.runtime.fail_collector_reservation = true;
    REQUIRE_NOTHROW(Access::CollectPass(cache));
    CHECK(Access::Contains(cache, id));
    CHECK(Access::CollectorState(cache) == std::tuple{size_t{0}, u64{0}, size_t{0}});
    CHECK(Access::WritebackPageCounts(cache) == pages);
    CHECK(Access::History(cache) == history);
    CHECK(f.device.IsRangeCached(cpu, size) == watched);
    CHECK(f.runtime.downloads == 0);
    f.runtime.fail_collector_reservation = false;
    f.runtime.fail_staging_allocation = true;
    REQUIRE_NOTHROW(Access::CollectPass(cache));
    CHECK(Access::Contains(cache, id));
    CHECK(Access::CollectorState(cache) == std::tuple{size_t{0}, u64{0}, size_t{0}});
    CHECK(Access::WritebackPageCounts(cache) == pages);
    CHECK(Access::History(cache) == history);
    CHECK(f.device.IsRangeCached(cpu, size) == watched);
    CHECK_FALSE(f.runtime.collector_buffer.has_value());
    CHECK(std::all_of(f.bytes, f.bytes + size, [](u8 byte) { return byte == 0; }));
    f.runtime.fail_collector_reservation = false;
    f.runtime.fail_staging_allocation = false;
    REQUIRE(Access::CollectDownload(cache, id));
    Access::Delete(cache, id);
    REQUIRE(f.runtime.collector_buffer.has_value());
    const auto downloads = f.runtime.downloads;
    // From here on, any staging allocation fails, including the old second-chunk site.
    f.runtime.fail_staging_allocation = true;
    Access::PollWritebacks(cache);
    f.runtime.completed_tick = f.runtime.current_tick++;
    Access::PollWritebacks(cache);
    REQUIRE(f.bytes[0] == 0x65);
    REQUIRE(f.bytes[size - 1] == 0);
    f.runtime.allow_wait = true;
    SECTION("Normal polling completes without another allocation") {
        for (unsigned i = 0; i < 32; ++i) {
            f.runtime.completed_tick = f.runtime.current_tick++;
            Access::PollWritebacks(cache);
        }
        CHECK(f.runtime.waits == 0);
    }
    SECTION("A guest read drains the partially landed image") {
        cache.DownloadMemory(cpu + size - 1, 1);
        CHECK(f.runtime.waits > 0);
    }
    SECTION("Device OOM drains the partially landed image") {
        Access::CollectPass(cache, true);
        CHECK(f.runtime.waits > 0);
    }
    SECTION("Cancellation releases the source but retains the reservation for a new image") {
        cache.UnmapGPUMemory(f.memory->GetID(), gpu, size);
        f.runtime.completed_tick = f.runtime.current_tick++;
        Access::PollWritebacks(cache);
        CHECK(Access::CollectorState(cache) == std::tuple{size_t{0}, u64{0}, size_t{0}});
        const auto next = Access::Insert(cache, info, gpu, cpu);
        Access::WriteGpu(cache, next, std::vector<u8>(size, 0x65));
        REQUIRE(Access::CollectDownload(cache, next));
        Access::Delete(cache, next);
        cache.DownloadMemory(cpu, size);
    }
    CHECK(f.runtime.downloads == downloads);
    CHECK(f.runtime.staging.size() == 1);
    CHECK(f.runtime.max_download_bytes == (4U << 20));
    CHECK(Access::CollectorState(cache) == std::tuple{size_t{0}, u64{0}, size_t{0}});
    CHECK(std::all_of(f.bytes, f.bytes + size, [](u8 byte) { return byte == 0x65; }));
}

TEST_CASE("Queued collectors share staging only after the prior GPU copy and landing",
          "[video_core][texture_cache][collector_writeback]") {
    using namespace VideoCommon;
    constexpr u32 size = 32U << 20;
    constexpr DAddr cpu = 0x10000;
    constexpr GPUVAddr gpu = 0x100000;
    Fixture f{2 * size + 0x10000};
    auto& cache = *f.cache;
    auto info = MakeInfo();
    info.format = PixelFormat::D16_UNORM;
    info.size = {4096, 4096, 1};
    info.block = {0, 5, 0};
    info.native_tiling = true;
    const auto first = Access::Insert(cache, info, gpu, cpu);
    const auto second = Access::Insert(cache, info, gpu + size, cpu + size);
    Access::WriteGpu(cache, first, std::vector<u8>(size, 0x31));
    Access::WriteGpu(cache, second, std::vector<u8>(size, 0x72));
    Access::Image(cache, first).lifetime = std::make_shared<int>();
    const std::weak_ptr<int> first_lifetime = Access::Image(cache, first).lifetime;
    REQUIRE(Access::CollectDownload(cache, first));
    REQUIRE(Access::CollectDownload(cache, second));
    Access::Delete(cache, first);
    Access::Delete(cache, second);
    f.runtime.fail_staging_allocation = true;
    REQUIRE(f.runtime.downloads == 1);
    REQUIRE(f.runtime.staging.size() == 1);
    Access::PollWritebacks(cache);
    bool cancelled = false;
    SECTION("Both images finish in FIFO order") {}
    SECTION("Cancelling the head still waits for its submitted copy") {
        cache.UnmapGPUMemory(f.memory->GetID(), gpu, size);
        cancelled = true;
    }
    Access::PollWritebacks(cache); // First copy has not completed on the GPU yet.
    CHECK_FALSE(first_lifetime.expired());
    CHECK(f.runtime.collector_buffer->mapped_span[0] == 0x31);
    CHECK(std::all_of(f.bytes, f.bytes + 2 * size, [](u8 byte) { return byte == 0; }));
    for (unsigned i = 0; i < 64; ++i) {
        f.runtime.completed_tick = f.runtime.current_tick++;
        Access::PollWritebacks(cache);
    }
    // The normal delete-tick keeper is retired by TickFrame, independently of the FIFO.
    cache.TickFrame();
    CHECK(first_lifetime.expired());
    CHECK(f.runtime.waits == 0);
    CHECK(f.runtime.downloads == 1);
    CHECK(f.runtime.staging.size() == 1);
    CHECK(Access::CollectorState(cache) == std::tuple{size_t{0}, u64{0}, size_t{0}});
    CHECK(std::all_of(f.bytes, f.bytes + size,
                      [cancelled](u8 byte) { return byte == (cancelled ? 0 : 0x31); }));
    CHECK(std::all_of(f.bytes + size, f.bytes + 2 * size, [](u8 byte) { return byte == 0x72; }));
}

TEST_CASE("Async image downloads and the delete tick both hold the image alive",
          "[video_core][texture_cache][image_lifetime]") {
    Fixture f;
    auto& cache = *f.cache;
    const auto id = Access::Insert(cache, MakeInfo(), 0x100000, 0x10000);
    auto& image = Access::Image(cache, id);
    image.lifetime = std::make_shared<int>();
    const std::weak_ptr<int> lifetime = image.lifetime;
    Access::WriteGpu(cache, id, std::vector<u8>(image.unswizzled_size_bytes, 0xA5));
    Access::QueueDownload(cache, id);

    SECTION("An older download can finish before later GPU work") {
        cache.CommitAsyncFlushes();
        f.runtime.current_tick = 2;
        Access::Delete(cache, id);
        REQUIRE(Access::Insert(cache, MakeInfo(), 0x102000, 0x12000) == id);

        f.runtime.completed_tick = 1;
        cache.PopAsyncFlushes();
        cache.TickFrame();
        CHECK(f.bytes[0] == 0xA5);
        CHECK_FALSE(lifetime.expired());

        f.runtime.completed_tick = 2;
        cache.TickFrame();
        CHECK(lifetime.expired());
    }
    SECTION("A newer download can still be uncommitted when the delete tick completes") {
        Access::Delete(cache, id);
        REQUIRE(Access::Insert(cache, MakeInfo(), 0x102000, 0x12000) == id);

        f.runtime.completed_tick = 1;
        f.runtime.current_tick = 2;
        cache.TickFrame();
        REQUIRE_FALSE(lifetime.expired());
        cache.CommitAsyncFlushes();
        f.runtime.completed_tick = 2;
        cache.PopAsyncFlushes();
        CHECK(f.bytes[0] == 0xA5);
        CHECK(lifetime.expired());
    }
}

TEST_CASE("Deleting an image cancels only its queued unswizzle",
          "[video_core][texture_cache][image_lifetime]") {
    Fixture f;
    auto& cache = *f.cache;
    auto info = MakeInfo();
    info.type = VideoCommon::ImageType::e3D;
    info.format = VideoCore::Surface::PixelFormat::BC1_RGBA_UNORM;
    info.size = {32, 32, 8};
    const auto id = Access::Insert(cache, info, 0x100000, 0x10000);
    const auto other = Access::Insert(cache, info, 0x104000, 0x14000);
    Access::QueueUnswizzle(cache, id);
    Access::QueueUnswizzle(cache, other);

    bool initialized = false;
    SECTION("Before processing starts") {}
    SECTION("With deferred staging already in use") {
        f.runtime.current_tick = 4;
        Access::ProcessOneUnswizzleSlice(cache);
        REQUIRE(f.runtime.unswizzle_uploads == 1);
        initialized = true;
    }
    f.runtime.current_tick = 5;
    Access::Delete(cache, id);

    REQUIRE(Access::Insert(cache, MakeInfo(), 0x108000, 0x18000) == id);
    CHECK(Access::QueuedUnswizzles(cache) == std::vector{other});
    CHECK(f.runtime.freed_staging_tick == (initialized ? 5u : 0u));
}

TEST_CASE("Sparse texture iteration skips segments that cannot be translated",
          "[video_core][texture_cache][sparse_segments]") {
    Fixture f;
    constexpr GPUVAddr gpu_addr = 0x200000;
    constexpr size_t page_size = 0x1000;
    constexpr DAddr invalid_addr = DAddr{1} << Tegra::MaxwellDeviceMemoryManager::AS_BITS;
    constexpr DAddr valid_addr = 0x20000;
    f.memory->Map(gpu_addr, invalid_addr, page_size, Tegra::PTEKind::PITCH, false);
    f.memory->Map(gpu_addr + page_size, valid_addr, page_size, Tegra::PTEKind::PITCH, false);

    VideoCommon::ImageBase image{MakeInfo(), gpu_addr, valid_addr};
    REQUIRE(f.memory->GetSubmappedRange(gpu_addr, image.guest_size_bytes).size() == 2);
    REQUIRE_FALSE(f.memory->GpuToCpuAddress(gpu_addr));

    size_t visited{};
    Access::ForEachSparseSegment(*f.cache, image, [&](GPUVAddr address, DAddr device, size_t size) {
        ++visited;
        CHECK(address == gpu_addr + page_size);
        CHECK(device == valid_addr);
        CHECK(size == page_size);
    });
    CHECK(visited == 1);
}

TEST_CASE("FindImage keeps GOB and native tiling images apart at one address",
          "[video_core][texture_cache][gob_layout]") {
    Fixture f;
    constexpr GPUVAddr gpu_addr = 0x100000;
    constexpr DAddr cpu_addr = 0x10000;

    auto gob = MakeInfo();
    gob.format = VideoCore::Surface::PixelFormat::BC7_UNORM;
    gob.maxwell_gob_layout = true;

    auto native = gob;
    native.maxwell_gob_layout = false;
    native.native_tiling = true;

    const auto gob_id = Access::Insert(*f.cache, gob, gpu_addr, cpu_addr);
    REQUIRE(gob_id != VideoCommon::ImageId{});
    CHECK(Access::Info(*f.cache, gob_id).maxwell_gob_layout);
    CHECK_FALSE(Access::Info(*f.cache, gob_id).native_tiling);

    // Same address and dimensions, but the other GOB order: a different image, not a subresource.
    CHECK(Access::Find(*f.cache, native, gpu_addr) == VideoCommon::ImageId{});
    // The matching layout still resolves to the cached image.
    CHECK(Access::Find(*f.cache, gob, gpu_addr) == gob_id);

    const auto native_id = Access::Insert(*f.cache, native, gpu_addr, cpu_addr);
    REQUIRE(native_id != VideoCommon::ImageId{});
    REQUIRE(native_id != gob_id);
    CHECK_FALSE(Access::Info(*f.cache, native_id).maxwell_gob_layout);
    CHECK(Access::Info(*f.cache, native_id).native_tiling);

    // With both cached, each descriptor resolves only to its own layout.
    CHECK(Access::Find(*f.cache, native, gpu_addr) == native_id);
    CHECK(Access::Find(*f.cache, gob, gpu_addr) == gob_id);
}

TEST_CASE("Partial texture transfers and writeback use the image GOB order",
          "[video_core][texture_cache][gob_layout]") {
    using namespace VideoCommon;
    using Tegra::Texture::SwizzleTexture;

    Fixture f;
    constexpr GPUVAddr gpu_addr = 0x100000;
    auto info = MakeInfo();
    Common::ScratchBuffer<u8> scratch;
    for (const bool compressed : {false, true}) {
        info.format = compressed ? PixelFormat::BC7_UNORM : PixelFormat::A8B8G8R8_UNORM;
        const u32 tiles = compressed ? 16 : 64;
        const u32 bytes_per_tile = compressed ? 16 : 4;
        const auto guest_size = CalculateGuestSizeInBytes(info);
        std::vector<u8> linear(CalculateUnswizzledSizeBytes(info));
        for (size_t i = 0; i < linear.size(); ++i) {
            linear[i] = static_cast<u8>(i ^ (i >> 8U));
        }
        const auto downloads = FullDownloadCopies(info);
        for (const bool native : {false, true}) {
            CAPTURE(compressed, native);
            info.native_tiling = native;
            std::vector<u8> expected(guest_size);
            SwizzleTexture(expected, linear, bytes_per_tile, tiles, tiles, 1, 0, 0, 0, native);
            f.memory->WriteBlockUnsafe(gpu_addr, expected.data(), expected.size());

            const std::array dirty{std::pair<u64, u64>{0, 512}};
            const auto rows = PlanUnswizzleImageRows(gpu_addr, info, dirty);
            REQUIRE(rows.layout_valid);
            REQUIRE(rows.runs.size() == 1);
            REQUIRE(rows.total_host_bytes == 512);
            std::vector<u8> row_output(rows.total_host_bytes);
            ExecuteUnswizzlePlan(*f.memory, rows, row_output);
            for (size_t y = 0; y < 8; ++y) {
                CHECK(std::equal(row_output.begin() + y * 64, row_output.begin() + (y + 1) * 64,
                                 linear.begin() + y * 256));
            }
            if (!compressed) {
                const std::array rects{LevelRect{.level = 0, .x = 0, .y = 0, .width = 16, .height = 8}};
                std::vector<u8> rect_output(UnswizzledRectsBytes(info, rects));
                REQUIRE(UnswizzleImageRects(*f.memory, gpu_addr, info, rects, rect_output).size() == 1);
                CHECK(rect_output == row_output);
            }

            std::vector<u8> actual(guest_size);
            f.memory->WriteBlockUnsafe(gpu_addr, actual.data(), actual.size());
            SwizzleImage(*f.memory, gpu_addr, info, downloads, linear, scratch);
            f.memory->ReadBlockUnsafe(gpu_addr, actual.data(), actual.size());
            CHECK(actual == expected);
            CHECK(actual[native ? 0x40 : 0x20] == linear[16]);
        }
    }
}

TEST_CASE("Hinted BC7 lookup waits for GPU-written native overlap bytes",
          "[video_core][texture_cache][gob_layout]") {
    using namespace VideoCommon;

    Fixture f;
    constexpr GPUVAddr gpu_addr = 0x100000;
    constexpr DAddr cpu_addr = 0x10000;
    auto native = MakeInfo();
    native.native_tiling = true;
    auto gob = native;
    gob.format = PixelFormat::BC7_UNORM;
    gob.native_tiling = false;
    gob.maxwell_gob_layout = true;
    const auto target = Access::Insert(*f.cache, native, gpu_addr, cpu_addr);
    const auto hinted = Access::Insert(*f.cache, gob, gpu_addr, cpu_addr);
    std::vector<u8> written(CalculateUnswizzledSizeBytes(native));
    written[16] = 0x5a;
    Access::WriteGpu(*f.cache, target, std::move(written));

    SECTION("An in-flight writeback preserves intervening guest writes") {
        REQUIRE(Access::Find(*f.cache, gob, gpu_addr) == NULL_IMAGE_ID);
        CHECK(f.runtime.downloads == 1);
        CHECK(f.bytes[0x40] == 0);
        CHECK(Access::Find(*f.cache, gob, gpu_addr) == NULL_IMAGE_ID);
        CHECK(f.runtime.downloads == 1);

        f.bytes[0x41] = 0x73;
        f.cache->WriteMemory(cpu_addr + 0x41, 1);
        f.runtime.completed_tick = 1;
        Access::PollWritebacks(*f.cache);
        CHECK(f.bytes[0x40] == 0x5a);
        CHECK(f.bytes[0x41] == 0x73);
        CHECK(f.bytes[0x20] == 0);
        CHECK(Access::Find(*f.cache, gob, gpu_addr) == hinted);
        CHECK(f.runtime.downloads == 1);
    }

    SECTION("Only a full guest overwrite releases the view before staging") {
        f.bytes[0x41] = 0x73;
        f.cache->WriteMemory(cpu_addr + 0x41, 1);
        CHECK(Access::Find(*f.cache, gob, gpu_addr) == NULL_IMAGE_ID);
        CHECK(f.runtime.downloads == 0);
        CHECK(f.bytes[0x40] == 0);
        CHECK(f.bytes[0x41] == 0x73);

        const auto guest_size = CalculateGuestSizeInBytes(native);
        const std::vector<u8> replacement(guest_size, 0x73);
        f.memory->WriteBlockUnsafe(gpu_addr, replacement.data(), replacement.size());
        f.cache->WriteMemory(cpu_addr, guest_size);

        CHECK(Access::Find(*f.cache, gob, gpu_addr) == hinted);
        CHECK(f.runtime.downloads == 0);
        CHECK(f.bytes[0x40] == 0x73);
    }
}

TEST_CASE("Cached XC2 LUT layout is invalidated by a conflicting write",
          "[video_core][texture_cache][gob_layout]") {
    using namespace VideoCommon;
    using namespace Tegra::Texture;

    Fixture f;
    const u64 saved_program_id = Settings::GetCurrentProgramID();
    SCOPE_EXIT { Settings::SetCurrentProgramID(saved_program_id); };
    Settings::SetCurrentProgramID(Xc2ProgramId);
    constexpr GPUVAddr gpu_addr = 0x100000;
    constexpr DAddr cpu_addr = 0x10000;
    TICEntry tic{};
    tic.address_low = gpu_addr;
    tic.format.Assign(TextureFormat::A8B8G8R8);
    tic.r_type.Assign(ComponentType::UNORM);
    tic.g_type.Assign(ComponentType::UNORM);
    tic.b_type.Assign(ComponentType::UNORM);
    tic.a_type.Assign(ComponentType::UNORM);
    tic.texture_type.Assign(TextureType::Texture3D);
    tic.header_version.Assign(TICHeaderVersion::BlockLinear);
    tic.load_store_hint.Assign(1);
    tic.width_minus_one.Assign(15);
    tic.height_minus_1.Assign(15);
    tic.depth_minus_1.Assign(15);
    auto native = ImageInfo{tic};
    native.native_tiling = true;
    auto maxwell = native;
    maxwell.native_tiling = false;
    maxwell.maxwell_gob_layout = true;
    auto& cache = *f.cache;
    const auto target = Access::Insert(cache, native, gpu_addr, cpu_addr);
    const auto lookup = Access::Insert(cache, maxwell, gpu_addr, cpu_addr);
    Access::LinkOverlap(cache, target, lookup);
    cache.MarkModification(target);
    Access::CacheLutView(cache, target, tic);
    const auto revision = Access::DescriptorRevision(cache);
    REQUIRE(Access::HasCachedView(cache, tic));

    SECTION("A render to the same native target keeps the selected view") {
        cache.MarkModification(target);
        CHECK(Access::HasCachedView(cache, tic));
        CHECK(Access::DescriptorRevision(cache) == revision);
    }
    SECTION("A guest write forces unchanged descriptors to resolve the layout again") {
        cache.WriteMemory(cpu_addr, CalculateGuestSizeInBytes(native));
        CHECK_FALSE(Access::HasCachedView(cache, tic));
        CHECK(Access::DescriptorRevision(cache) > revision);
    }
    SECTION("A GPU write to the Maxwell overlap invalidates the native view") {
        cache.MarkModification(lookup);
        CHECK_FALSE(Access::HasCachedView(cache, tic));
        CHECK(Access::DescriptorRevision(cache) > revision);
    }
    SECTION("A GPU write to another non-native overlap also invalidates the native view") {
        maxwell.maxwell_gob_layout = false;
        const auto other = Access::Insert(cache, maxwell, gpu_addr, cpu_addr);
        Access::LinkOverlap(cache, target, other);
        cache.MarkModification(other);
        CHECK_FALSE(Access::HasCachedView(cache, tic));
        CHECK(Access::DescriptorRevision(cache) > revision);
    }
    SECTION("A GPU write to the native overlap invalidates the Maxwell view") {
        cache.MarkModification(lookup);
        REQUIRE_FALSE(Access::HasCachedView(cache, tic));
        Access::CacheLutView(cache, lookup, tic);
        const auto maxwell_revision = Access::DescriptorRevision(cache);

        cache.MarkModification(lookup);
        CHECK(Access::HasCachedView(cache, tic));
        CHECK(Access::DescriptorRevision(cache) == maxwell_revision);

        cache.MarkModification(target);
        CHECK_FALSE(Access::HasCachedView(cache, tic));
        CHECK(Access::DescriptorRevision(cache) > maxwell_revision);
    }
}
