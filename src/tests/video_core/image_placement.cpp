// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/image_placement.h"

TEST_CASE("Images stay in device-local memory until the host fallback",
          "[video_core][vulkan][image_placement]") {
    // GTX 1650 (4 GB): VMA moved 805 MiB of images to the system memory type during a title's
    // load, without a warning, because device-local memory was only preferred.
    constexpr auto steps = Vulkan::ImagePlacementSteps(true);
    REQUIRE(steps[0].within_budget);
    REQUIRE(steps[0].require_device_local);
    REQUIRE_FALSE(steps[0].prefer_host);
    REQUIRE_FALSE(steps[1].within_budget);
    REQUIRE(steps[1].require_device_local);
    REQUIRE_FALSE(steps[1].prefer_host);
    REQUIRE_FALSE(steps[2].require_device_local);
    REQUIRE(steps[2].prefer_host);
}

TEST_CASE("Where host memory is not a separate type, images only prefer device-local memory",
          "[video_core][vulkan][image_placement]") {
    constexpr auto steps = Vulkan::ImagePlacementSteps(false);
    REQUIRE(steps[0].within_budget);
    REQUIRE_FALSE(steps[1].within_budget);
    for (const auto& step : steps) {
        REQUIRE_FALSE(step.require_device_local);
    }
    REQUIRE_FALSE(steps[0].prefer_host);
    REQUIRE_FALSE(steps[1].prefer_host);
    REQUIRE(steps[2].prefer_host);
}

namespace {
constexpr u64 MiB = u64{1} << 20;
} // namespace

TEST_CASE("Textures leave the last 512 MiB of video memory to render targets",
          "[video_core][vulkan][image_placement]") {
    // GTX 1650 (4 GB): the device-local heap's budget is 3696 MiB. The textures of a title's load
    // filled it, and the depth buffer and render targets made after them went to system memory.
    constexpr u64 reserve = 512 * MiB;
    REQUIRE(Vulkan::YieldsToRenderTargets(false, 3200 * MiB, 0, 3696 * MiB, reserve));
    REQUIRE_FALSE(Vulkan::YieldsToRenderTargets(true, 3200 * MiB, 0, 3696 * MiB, reserve));
    REQUIRE_FALSE(Vulkan::YieldsToRenderTargets(false, 1045 * MiB, 0, 3696 * MiB, reserve));
    REQUIRE_FALSE(Vulkan::YieldsToRenderTargets(false, 3184 * MiB, 0, 3696 * MiB, reserve));
    REQUIRE(Vulkan::YieldsToRenderTargets(false, 3185 * MiB, 0, 3696 * MiB, reserve));
}

TEST_CASE("A texture that would end inside the reserve goes to host memory",
          "[video_core][vulkan][image_placement]") {
    // GTX 1650 (4 GB), reserve 1024 MiB: the depth buffers and targets made after a big texture
    // still went to system memory. Checking the heap's usage without the image let a big one in
    // with the usage low, and it took the reserve.
    constexpr u64 reserve = 1024 * MiB;
    constexpr u64 budget = 3696 * MiB;
    // 1800 + 1024 fits, 1800 + 894 + 1024 does not.
    REQUIRE_FALSE(Vulkan::YieldsToRenderTargets(false, 1800 * MiB, 0, budget, reserve));
    REQUIRE(Vulkan::YieldsToRenderTargets(false, 1800 * MiB, 894 * MiB, budget, reserve));
    REQUIRE_FALSE(Vulkan::YieldsToRenderTargets(false, 1778 * MiB, 894 * MiB, budget, reserve));
    REQUIRE(Vulkan::YieldsToRenderTargets(false, 1779 * MiB, 894 * MiB, budget, reserve));
    // Render targets never yield, whatever their size.
    REQUIRE_FALSE(Vulkan::YieldsToRenderTargets(true, 3600 * MiB, 8 * MiB, budget, reserve));
    // The reserve off: no texture yields.
    REQUIRE_FALSE(Vulkan::YieldsToRenderTargets(false, 3600 * MiB, 894 * MiB, budget, 0));
}

TEST_CASE("The decode image for the BC encoder follows the textures' rule",
          "[video_core][vulkan][image_placement]") {
    // GTX 1650 (4 GB), reserve 512: the two 914 MiB decodes of a 1365x1365x90 array came with
    // video memory at 2526 and 2915 of 3704 MiB; with the reserve at 1536 they still go to host
    // memory.
    constexpr u64 reserve = Vulkan::RenderTargetReserveBytes(3704 * MiB);
    REQUIRE(Vulkan::YieldsToRenderTargets(false, 2526 * MiB, 914 * MiB, 3704 * MiB, reserve));
    // 12 GB card (budget 7388 MiB): at 2818 and 3207 MiB they fit, even with the first one alive.
    constexpr u64 twelve_gb = Vulkan::RenderTargetReserveBytes(7388 * MiB);
    REQUIRE_FALSE(
        Vulkan::YieldsToRenderTargets(false, 2818 * MiB, 914 * MiB, 7388 * MiB, twelve_gb));
    REQUIRE_FALSE(Vulkan::YieldsToRenderTargets(false, (3207 + 914) * MiB, 914 * MiB, 7388 * MiB,
                                                twelve_gb));
}

TEST_CASE("The reserve is 1536 MiB, at most half the budget",
          "[video_core][vulkan][image_placement]") {
    // GTX 1650 (4 GB, budget 3695 MiB): 512 left 6 targets in system memory with video memory at
    // 3691 of 3695 MiB; from 1536 on all fit.
    REQUIRE(Vulkan::RenderTargetReserveBytes(3695 * MiB) == 1536 * MiB);
    // 12 GB card (budget 7388 MiB): the same 1536; in the same load 265 to 568 images yielded
    // to it.
    REQUIRE(Vulkan::RenderTargetReserveBytes(7388 * MiB) == 1536 * MiB);
    // A 2 GB card keeps half its budget for textures.
    REQUIRE(Vulkan::RenderTargetReserveBytes(1900 * MiB) == 950 * MiB);
}

TEST_CASE("Image sizes come from the device without a probe image from Vulkan 1.3 on",
          "[video_core][vulkan][image_placement]") {
    // VK_MAKE_API_VERSION(0, 1, minor, 0); current NVIDIA drivers report 1.4, and the instance
    // asks 1.3.
    constexpr auto version = [](u32 minor) { return (1U << 22) | (minor << 12); };
    REQUIRE(Vulkan::HasDeviceImageRequirements(version(3)));
    REQUIRE(Vulkan::HasDeviceImageRequirements(version(4)));
    REQUIRE_FALSE(Vulkan::HasDeviceImageRequirements(version(2)));
}

TEST_CASE("Depth and stencil images never yield to render targets",
          "[video_core][vulkan][image_placement]") {
    // 12 GB card: a 12288x12288 D16 image (288 MiB), first made from a texture header and then
    // written as a depth buffer and sampled (a shadow map), went to host memory as a texture with
    // video memory at 5575 of 7382 MiB.
    REQUIRE_FALSE(Vulkan::YieldsAsTexture(false, true, 1, false));
    REQUIRE_FALSE(Vulkan::YieldsAsTexture(true, false, 1, false));
    REQUIRE_FALSE(Vulkan::YieldsAsTexture(true, true, 1, false));
    REQUIRE(Vulkan::YieldsAsTexture(false, false, 11, true));
}

TEST_CASE("Single-level uncompressed color images never yield to render targets",
          "[video_core][vulkan][image_placement]") {
    // 4 GB card: a 512x512 RGBA16F image and a 960x540 RGBA16F one, each first made from a texture
    // header and then written by compute shaders every frame (a blur chain), went to host memory
    // as textures; the two dispatches took 4.8 and 4.2 ms of a 45 ms frame over PCIe.
    REQUIRE_FALSE(Vulkan::YieldsAsTexture(false, false, 1, false));
    // What a title loads keeps yielding: block-compressed images, and mipmapped ones.
    REQUIRE(Vulkan::YieldsAsTexture(false, false, 1, true));
    REQUIRE(Vulkan::YieldsAsTexture(false, false, 11, true));
    REQUIRE(Vulkan::YieldsAsTexture(false, false, 11, false));
}

TEST_CASE("The decode image for the BC encoder counts twice its size",
          "[video_core][vulkan][image_placement]") {
    REQUIRE(Vulkan::PlacementBytes(false, 914 * MiB) == 914 * MiB);
    REQUIRE(Vulkan::PlacementBytes(true, 914 * MiB) == 1828 * MiB);
    // 12 GB card (budget 7388 MiB, reserve 1536): the first decode (video memory at 2818 MiB)
    // still fits; the second, with the first alive (3207 + 914), goes to host memory.
    constexpr u64 twelve_gb = Vulkan::RenderTargetReserveBytes(7388 * MiB);
    REQUIRE_FALSE(Vulkan::YieldsToRenderTargets(
        false, 2818 * MiB, Vulkan::PlacementBytes(true, 914 * MiB), 7388 * MiB, twelve_gb));
    REQUIRE(Vulkan::YieldsToRenderTargets(
        false, (3207 + 914) * MiB, Vulkan::PlacementBytes(true, 914 * MiB), 7388 * MiB, twelve_gb));
    // Then the textures made next (5836-5841 of 7373 MiB with both decodes alive) fit: one decode
    // less (914) and the 12288x12288 depth buffer in video memory (288), even after all 515 MiB
    // of the 110 textures that yielded.
    REQUIRE_FALSE(Vulkan::YieldsToRenderTargets(false, (5841 - 914 + 288) * MiB, 12 * MiB,
                                                7373 * MiB, twelve_gb));
    REQUIRE_FALSE(Vulkan::YieldsToRenderTargets(false, (5841 - 914 + 288 + 503) * MiB, 12 * MiB,
                                                7373 * MiB, twelve_gb));
    // GTX 1650 (4 GB, budget 3702 MiB): the decodes came at 2186 and 2442 MiB and went to host
    // memory counted once already; counted twice they still do.
    constexpr u64 four_gb = Vulkan::RenderTargetReserveBytes(3702 * MiB);
    REQUIRE(Vulkan::YieldsToRenderTargets(false, 2186 * MiB, 914 * MiB, 3702 * MiB, four_gb));
    REQUIRE(Vulkan::YieldsToRenderTargets(
        false, 2186 * MiB, Vulkan::PlacementBytes(true, 914 * MiB), 3702 * MiB, four_gb));
}

TEST_CASE("Only the decode images that are not kept count twice",
          "[video_core][vulkan][image_placement]") {
    // The 1365x1365x90 decode (11 levels): 893,912,400 bytes (852.5 MiB; VMA gives it 914 MiB with
    // its alignment), freed once the BC encoder is done.
    const u64 big = Vulkan::DecodeImageBytes(1365, 1365, 11, 90);
    REQUIRE(big == 893912400);
    REQUIRE(Vulkan::DecodeImageCountsTwice(big));
    // 12 GB card: the 512 to 1536 decodes, kept for the next texture of their shape, went to host
    // memory counted twice with video memory at 5831-5836 of 7373 MiB. They follow the textures'
    // rule.
    REQUIRE_FALSE(Vulkan::DecodeImageCountsTwice(Vulkan::DecodeImageBytes(1536, 1536, 11, 1)));
    REQUIRE_FALSE(Vulkan::DecodeImageCountsTwice(Vulkan::DecodeImageBytes(512, 512, 10, 1)));
    // The cut is the texture cache's: up to 256 MiB they are kept.
    REQUIRE(Vulkan::MaxKeptDecodeBytes == 256 * MiB);
    REQUIRE_FALSE(Vulkan::DecodeImageCountsTwice(256 * MiB));
    REQUIRE(Vulkan::DecodeImageCountsTwice(256 * MiB + 1));
}

TEST_CASE("Without a reserve nothing yields; over the budget only textures do",
          "[video_core][vulkan][image_placement]") {
    // A heap with no budget reported has no reserve.
    REQUIRE(Vulkan::RenderTargetReserveBytes(0) == 0);
    REQUIRE_FALSE(Vulkan::YieldsToRenderTargets(false, 3600 * MiB, 0, 3696 * MiB, 0));
    // Over the budget already: textures still go to host memory, render targets do not.
    REQUIRE(Vulkan::YieldsToRenderTargets(false, 3800 * MiB, 0, 3696 * MiB, 512 * MiB));
    REQUIRE_FALSE(Vulkan::YieldsToRenderTargets(true, 3800 * MiB, 0, 3696 * MiB, 512 * MiB));
}

TEST_CASE("Images require video memory only where host memory is a separate type",
          "[video_core][vulkan][image_placement]") {
    constexpr u32 DeviceLocal = 0x1; // VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    constexpr u32 HostVisible = 0x2; // VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
    // A discrete card: one device-local type and one host type.
    constexpr std::array<u32, 2> discrete{DeviceLocal, HostVisible};
    REQUIRE(Vulkan::HasHostMemoryApartFromVideo(discrete.data(), 2));
    // An integrated GPU: every type is device-local, so there is nothing to keep images out of.
    constexpr std::array<u32, 2> integrated{DeviceLocal, DeviceLocal | HostVisible};
    REQUIRE_FALSE(Vulkan::HasHostMemoryApartFromVideo(integrated.data(), 2));
    REQUIRE_FALSE(Vulkan::HasHostMemoryApartFromVideo(nullptr, 0));
}

TEST_CASE("The stream buffer goes to host memory where the CPU writes video memory through a small window",
          "[video_core][vulkan][image_placement]") {
    // A 4 GB GTX 1650 without resizable BAR: the device-local host-visible heap is 246 MiB.
    REQUIRE(Vulkan::StreamBufferInHostMemory(true, 246 * MiB, false));
    REQUIRE(Vulkan::StreamBufferInHostMemory(true, 256 * MiB, false));
    // With resizable BAR the whole of video memory is host visible: keep it there.
    REQUIRE_FALSE(Vulkan::StreamBufferInHostMemory(true, 12 * 1024 * MiB, false));
    // No device-local host-visible heap at all: the allocator falls back to host memory anyway.
    REQUIRE_FALSE(Vulkan::StreamBufferInHostMemory(true, 0, false));
    // An integrated GPU has no host memory apart from video memory.
    REQUIRE_FALSE(Vulkan::StreamBufferInHostMemory(false, 246 * MiB, false));
    // An integrated GPU that does report host memory apart and a 256 MiB device-local heap: the
    // CPU already writes that heap at host memory speed.
    REQUIRE_FALSE(Vulkan::StreamBufferInHostMemory(true, 256 * MiB, true));
}
