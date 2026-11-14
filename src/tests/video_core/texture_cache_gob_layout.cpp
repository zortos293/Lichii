// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <memory>
#include <optional>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"
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

namespace {

struct TestRuntime {
    bool HasBrokenTextureViewFormats() const {
        return false;
    }
    bool HasNativeBgr() const {
        return false;
    }
};

struct TestImage : VideoCommon::ImageBase {
    TestImage(TestRuntime&, const VideoCommon::ImageInfo& image_info, GPUVAddr gpu_address,
              DAddr cpu_address)
        : ImageBase{image_info, gpu_address, cpu_address} {}
    explicit TestImage(const VideoCommon::NullImageParams& params) : ImageBase{params} {}
    TestImage(TestImage&&) = default;
    TestImage& operator=(TestImage&&) = default;
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
struct TestAsyncBuffer {};
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
    static constexpr bool IMPLEMENTS_ASYNC_DOWNLOADS = false;
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
    template <class C>
    static ImageId Insert(C& cache, const ImageInfo& info, GPUVAddr gpu_addr, DAddr cpu_addr) {
        const auto id = cache.slot_images.insert(cache.runtime, info, gpu_addr, cpu_addr);
        cache.RegisterImage(id);
        return id;
    }
    template <class C>
    static ImageId Find(C& cache, const ImageInfo& info, GPUVAddr gpu_addr) {
        return cache.FindImage(info, gpu_addr, RelaxedOptions{});
    }
    template <class C>
    static const ImageInfo& Info(C& cache, ImageId id) {
        return cache.slot_images[id].info;
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
    std::optional<Tegra::MemoryManager> memory;
    Tegra::Control::ChannelState state{0};
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
        memory.emplace(system, device, 32, 1ULL << 31, 16);
        memory->BindRasterizer(&rasterizer);
        memory->EnableNativeTiling();
        memory->Map(0x100000, 0x10000, 0x10000, Tegra::PTEKind::PITCH, true);
        state.memory_manager = &*memory;
        state.payload.emplace(system, *memory, state);
        cache = std::make_unique<Cache>(runtime, device);
        cache->CreateChannel(state);
        cache->BindToChannel(0);
    }
    ~Fixture() {
        cache.reset();
        memory.reset();
        device.Unmap(0x10000, 0x40000);
        device.UnregisterProcess(asid);
        system.Kernel().UnregisterKernelObject(process.get());
    }
};

} // namespace

TEST_CASE("FindImage keeps GOB and native tiling images apart at one address",
          "[video_core][texture_cache][gob_layout]") {
    Fixture f;
    constexpr GPUVAddr gpu_addr = 0x100000;
    constexpr DAddr cpu_addr = 0x10000;

    auto gob = MakeInfo();
    gob.maxwell_gob_layout = true;
    gob.native_tiling = false;

    auto native = MakeInfo();
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