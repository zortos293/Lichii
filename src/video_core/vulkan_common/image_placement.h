// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"

namespace Vulkan {

/// One of MemoryAllocator::AllocateImage's attempts, in order.
struct ImagePlacementStep {
    bool within_budget;        ///< VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT
    bool require_device_local; ///< requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    bool prefer_host;          ///< VMA_MEMORY_USAGE_AUTO_PREFER_HOST, no preferred flags
};

/// The attempts: within the budget, over it, then host memory.
constexpr std::array<ImagePlacementStep, 3> ImagePlacementSteps(bool keep_in_video_memory) {
    return {{
        {.within_budget = true, .require_device_local = keep_in_video_memory, .prefer_host = false},
        {.within_budget = false, .require_device_local = keep_in_video_memory, .prefer_host = false},
        {.within_budget = false, .require_device_local = false, .prefer_host = true},
    }};
}

/// Whether some memory type is not device-local (the propertyFlags of each type). Without one, as
/// on an integrated GPU, there is no host memory to keep images out of, and requiring device-local
/// memory or a reserve for render targets would only make allocations fail.
constexpr bool HasHostMemoryApartFromVideo(const u32* type_flags, u32 count) {
    constexpr u32 DeviceLocal = 0x1; // VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    for (u32 type = 0; type < count; ++type) {
        if ((type_flags[type] & DeviceLocal) == 0) {
            return true;
        }
    }
    return false;
}

/// Video memory kept for render targets and depth buffers: 1536 MiB, at most half the budget.
/// What it holds (render targets, depth buffers, device-local buffers) grows with the title's
/// resolution, not with the card. On a 4 GB GTX 1650 (budget 3695 MiB), 512 left 6 targets in system memory
/// with the card full; 1536 and 2048 fit them all at the same frame rate. A 12 GB card reaches it
/// in the same load only while another program holds video memory (budget about 7.3 GiB): 265 to
/// 568 images yielded, 1.4 to 1.9 GiB, at the same frame rate. With the card to itself none does.
constexpr u64 RenderTargetReserveBytes(u64 heap_budget) {
    constexpr u64 Reserve = u64{1536} << 20;
    return heap_budget / 2 < Reserve ? heap_budget / 2 : Reserve;
}

/// Whether the stream buffer (staging uploads, uniform, index and storage buffers the CPU fills
/// every draw) goes to host memory instead of the device-local host-visible heap. Without
/// resizable BAR that heap is a 256 MiB window into video memory, and the CPU writes it at about
/// 1 GB/s (4 GB GTX 1650, PCIe 3.0 x8: 9 ms of every 40 ms frame went into those copies, against
/// 25 GB/s into host memory). The GPU reads host memory over PCIe instead, which it has time for.
/// `host_memory_apart`: HasHostMemoryApartFromVideo; `window_bytes`: the largest device-local
/// host-visible heap (0 if none). An integrated GPU keeps the old placement even when it reports
/// host memory apart and a heap that small: its CPU writes that heap at host memory speed, so
/// there is no slow window to avoid.
constexpr bool StreamBufferInHostMemory(bool host_memory_apart, u64 window_bytes,
                                        bool integrated) {
    if (!host_memory_apart || integrated) {
        return false;
    }
    constexpr u64 SmallWindow = u64{256} << 20;
    return window_bytes != 0 && window_bytes <= SmallWindow;
}

/// Whether the device has vkGetDeviceImageMemoryRequirements (Vulkan 1.3 core), which gives an
/// image's size without making one.
constexpr bool HasDeviceImageRequirements(u32 api_version) {
    constexpr u32 Vulkan13 = (1U << 22) | (3U << 12); // VK_API_VERSION_1_3
    return api_version >= Vulkan13;
}

/// Whether an image that is not a render target goes straight to host memory, so the last
/// `reserve` bytes of the device-local heap's budget stay for render targets. Counts the image
/// itself: without it a big texture made with the heap's usage low took the reserve (4 GB card).
constexpr bool YieldsToRenderTargets(bool render_target, u64 heap_usage, u64 image_bytes,
                                     u64 heap_budget, u64 reserve) {
    return !render_target && reserve != 0 && heap_usage + image_bytes + reserve > heap_budget;
}

/// Whether an image can yield to render targets as a texture. Depth and stencil images never do:
/// the GPU writes them, even those first made from a texture header (12 GB card: a 12288x12288 D16
/// shadow map made from one went to host memory as a texture, 288 MiB). Neither do single-level
/// color images that are not block-compressed: those are the GPU's own output far more often
/// than what a title loads (blur and bloom chains, compute results), and the GPU writes and reads
/// them every frame. A texture header can make them first, before any render target does (4 GB
/// card: two compute dispatches of a blur took 4.8 and 4.2 ms of a 45 ms frame over PCIe).
constexpr bool YieldsAsTexture(bool render_target, bool depth_or_stencil, u32 levels,
                               bool block_compressed) {
    const bool gpu_output = levels == 1 && !block_compressed;
    return !render_target && !depth_or_stencil && !gpu_output;
}

/// What an image counts for in YieldsToRenderTargets. The decode image for the BC encoder
/// (`scratch`) counts twice: until the next frame frees it, the textures made meanwhile need room
/// too (12 GB card: with two 914 MiB decodes in video memory, 110 textures yielded, 515 MiB).
constexpr u64 PlacementBytes(bool scratch, u64 image_bytes) {
    return scratch ? image_bytes * 2 : image_bytes;
}

/// Finished decode images for the BC encoder wait for the next texture of their shape up to this
/// much memory in all (TextureCacheRuntime::TickFrame); a bigger one is freed once the GPU is done.
inline constexpr u64 MaxKeptDecodeBytes = u64{256} << 20;

/// The RGBA8 decode of a 2D array for the BC encoder, every level, as TickFrame counts it.
constexpr u64 DecodeImageBytes(u32 width, u32 height, u32 levels, u32 layers) {
    u64 bytes = 0;
    for (u32 level = 0; level < levels; ++level) {
        const u64 level_width = (width >> level) > 0 ? (width >> level) : 1;
        const u64 level_height = (height >> level) > 0 ? (height >> level) : 1;
        bytes += level_width * level_height * 4;
    }
    return bytes * layers;
}

/// Whether a decode image counts twice against the render target reserve: only one that is never
/// kept (over MaxKeptDecodeBytes) lives on, freed, until the next frame. The kept ones follow the
/// textures' rule: counted twice, a 12 GB card sent its 512 to 1536 decodes to host memory.
constexpr bool DecodeImageCountsTwice(u64 decode_bytes) {
    return decode_bytes > MaxKeptDecodeBytes;
}

} // namespace Vulkan
