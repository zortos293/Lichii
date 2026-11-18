// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef LICHII_NVN_DIRECT

#include <array>
#include <mutex>
#include <vector>

#include "video_core/nvn_direct/nvn_direct.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class MemoryAllocator;

/// NVN2 direct mode: the replay's ASTC textures on a device without ASTC,
/// decoded by Lichii's astc_decoder.comp and re-compressed by its bc_encoder.comp, the same
/// shaders and push constants as ASTCDecoderPass::AssembleInto and BcEncoderPass::Encode.
/// Recorded straight into the replay's command buffer, not through a Scheduler: Lichii's
/// Scheduler has one owner (the GPU thread), and running the decode there could deadlock with
/// AccelerateDisplay, which waits on that thread for the replay's present fence.
///
/// A level goes in tiles of at most TileWidth x TileHeight texels through one decode image and
/// one BC buffer of their own, which bounds the memory a burst of level loads takes. The replay
/// records every decode into its upload batches, which all go to one queue, so each tile only
/// needs barriers against the tile before it.
///
/// A level that fits one SmallTile x SmallTile tile (most mip levels and array layers) is decoded
/// into a slot of its own instead: up to SmallSlots of them are decoded back to
/// back, then encoded and copied, with three barriers for the lot, at Flush (the replay's upload
/// batch end) or when the slots run out. One tile at a time, with a decode, an encode and a copy
/// each behind a barrier, Mario Kart World's race start (about 12,800 levels) kept the GPU in
/// its barriers for ~200 ms while the replay waited for its upload batches.
class NvnDirectAstc {
public:
    explicit NvnDirectAstc(const Device& device, MemoryAllocator& memory_allocator);
    ~NvnDirectAstc();

    /// nvn_direct_device::astc_to_bc.
    static int Record(void* context, VkCommandBuffer cmdbuf, const nvn_direct_astc_level* level);
    /// nvn_direct_device::astc_flush.
    static void Flush(void* context, VkCommandBuffer cmdbuf);

    /// nvn_direct_device::astc_opaque: ASTC::IsOpaque, as the texture cache's BC1 choice
    /// (EDEN_NX2_ASTC_BC1=0 keeps every image BC3).
    static int IsOpaque(const void* blocks, size_t size);

private:
    static constexpr u32 TileWidth = 2048;
    static constexpr u32 TileHeight = 2048;

    static constexpr u32 SmallTile = 256;
    static constexpr u32 SmallSlots = 64;

    /// A small level decoded into its slot, waiting for its encode and copy.
    struct PendingSmall {
        u32 slot;
        nvn_direct_astc_level level;
    };

    bool RecordLevel(VkCommandBuffer cmdbuf, const nvn_direct_astc_level& level);
    bool RecordSmall(VkCommandBuffer cmdbuf, const nvn_direct_astc_level& level);
    void FlushSmall(VkCommandBuffer cmdbuf);
    bool MakeTileResources();
    bool MakeSmallResources();

    const Device& device;
    MemoryAllocator& memory_allocator;
    std::mutex mutex;

    vk::DescriptorSetLayout decode_set_layout;
    vk::PipelineLayout decode_layout;
    vk::DescriptorUpdateTemplate decode_template;
    vk::ShaderModule decode_module;
    vk::Pipeline decode_pipeline;
    vk::DescriptorSetLayout encode_set_layout;
    vk::PipelineLayout encode_layout;
    vk::DescriptorUpdateTemplate encode_template;
    vk::ShaderModule encode_module;
    vk::Pipeline encode_pipeline;

    bool tile_resources_tried = false;
    bool tile_image_initialized = false;
    vk::Image tile_image;
    vk::ImageView tile_view;
    vk::Buffer tile_blocks;

    bool small_resources_tried = false;
    bool small_image_initialized = false;
    vk::Image small_image; ///< SmallSlots layers of SmallTile x SmallTile
    std::array<vk::ImageView, SmallSlots> small_views;
    vk::Buffer small_blocks; ///< SmallSlots regions of SmallTile x SmallTile bytes
    std::vector<PendingSmall> pending;
    VkCommandBuffer pending_cmdbuf = VK_NULL_HANDLE;
};

} // namespace Vulkan

#endif
