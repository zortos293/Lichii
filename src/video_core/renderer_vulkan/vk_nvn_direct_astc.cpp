// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef LICHII_NVN_DIRECT

#include <algorithm>
#include <array>
#include <cstddef>

#include "common/div_ceil.h"
#include "common/logging.h"
#include "video_core/host_shaders/astc_decoder_comp_spv.h"
#include "video_core/host_shaders/bc_encoder_comp_spv.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include <cstdlib>
#include <span>
#include <string>

#include "video_core/renderer_vulkan/vk_nvn_direct_astc.h"
#include "video_core/textures/astc.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {

// A GOB is 64 bytes (4 ASTC blocks) wide and 8 block rows high (astc_decoder.comp's
// GOB_SIZE_*_SHIFT); a block of GOBs is 1 << block_height_log2 GOBs high.
constexpr u32 GobBlocksX = 4;
constexpr u32 GobRows = 8;
constexpr u32 GobShift = 9;

/// Both shaders take one storage buffer and one storage image (push descriptors).
struct TileDescriptors {
    VkDescriptorBufferInfo buffer;
    VkDescriptorImageInfo image;
};

struct ComputeProgram {
    vk::DescriptorSetLayout set_layout;
    vk::PipelineLayout layout;
    vk::DescriptorUpdateTemplate update_template;
    vk::ShaderModule module;
    vk::Pipeline pipeline;
};

ComputeProgram MakeProgram(const Device& device, u32 buffer_binding, u32 image_binding,
                           u32 push_size, std::span<const u32> code) {
    const vk::Device& dev = device.GetLogical();
    const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
        {buffer_binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
         nullptr},
        {image_binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
         nullptr},
    }};
    ComputeProgram program;
    program.set_layout = dev.CreateDescriptorSetLayout({
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    });
    const VkPushConstantRange push_range{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size};
    program.layout = dev.CreatePipelineLayout({
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = program.set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    });
    const std::array<VkDescriptorUpdateTemplateEntry, 2> entries{{
        {buffer_binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         offsetof(TileDescriptors, buffer), sizeof(TileDescriptors)},
        {image_binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, offsetof(TileDescriptors, image),
         sizeof(TileDescriptors)},
    }};
    program.update_template = dev.CreateDescriptorUpdateTemplate({
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .descriptorUpdateEntryCount = static_cast<u32>(entries.size()),
        .pDescriptorUpdateEntries = entries.data(),
        .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR,
        .descriptorSetLayout = *program.set_layout,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE,
        .pipelineLayout = *program.layout,
        .set = 0,
    });
    program.module = dev.CreateShaderModule({
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = code.size_bytes(),
        .pCode = code.data(),
    });
    program.pipeline = dev.CreateComputePipeline(
        {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = *program.module,
                .pName = "main",
                .pSpecializationInfo = nullptr,
            },
            .layout = *program.layout,
            .basePipelineHandle = {},
            .basePipelineIndex = 0,
        },
        device.StaticPipelineCache());
    return program;
}

void MemoryBarrier(const vk::CommandBuffer& cmdbuf, VkPipelineStageFlags src,
                   VkAccessFlags src_access, VkPipelineStageFlags dst, VkAccessFlags dst_access) {
    cmdbuf.PipelineBarrier(src, dst, 0,
                           VkMemoryBarrier{
                               .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                               .pNext = nullptr,
                               .srcAccessMask = src_access,
                               .dstAccessMask = dst_access,
                           });
}

} // Anonymous namespace

NvnDirectAstc::NvnDirectAstc(const Device& device_, MemoryAllocator& memory_allocator_)
    : device{device_}, memory_allocator{memory_allocator_} {
    // astc_decoder.comp: input buffer at binding 0, output image at 1 (BINDING_*); bc_encoder.comp:
    // source image at 0, output buffer at 1.
    ComputeProgram decode = MakeProgram(device, 0, 1, sizeof(AstcPushConstants),
                                        ASTC_DECODER_COMP_SPV);
    ComputeProgram encode = MakeProgram(device, 1, 0, sizeof(BcEncoderPushConstants),
                                        BC_ENCODER_COMP_SPV);
    decode_set_layout = std::move(decode.set_layout);
    decode_layout = std::move(decode.layout);
    decode_template = std::move(decode.update_template);
    decode_module = std::move(decode.module);
    decode_pipeline = std::move(decode.pipeline);
    encode_set_layout = std::move(encode.set_layout);
    encode_layout = std::move(encode.layout);
    encode_template = std::move(encode.update_template);
    encode_module = std::move(encode.module);
    encode_pipeline = std::move(encode.pipeline);
}

NvnDirectAstc::~NvnDirectAstc() = default;

int NvnDirectAstc::Record(void* context, VkCommandBuffer cmdbuf,
                          const nvn_direct_astc_level* level) {
    auto* const self = static_cast<NvnDirectAstc*>(context);
    std::scoped_lock lock{self->mutex};
    // Called from the replay's C code: nothing may throw through it.
    try {
        if (self->RecordSmall(cmdbuf, *level)) {
            return 1;
        }
        return self->RecordLevel(cmdbuf, *level) ? 1 : 0;
    } catch (const std::exception& exception) {
        LOG_ERROR(Render_Vulkan, "NVN2 direct mode: ASTC decode not recorded: {}",
                  exception.what());
        return 0;
    }
}

void NvnDirectAstc::Flush(void* context, VkCommandBuffer cmdbuf) {
    auto* const self = static_cast<NvnDirectAstc*>(context);
    std::scoped_lock lock{self->mutex};
    self->FlushSmall(cmdbuf);
}

bool NvnDirectAstc::MakeSmallResources() {
    if (small_resources_tried) {
        return static_cast<bool>(small_blocks);
    }
    small_resources_tried = true;
    small_image = memory_allocator.TryCreateImage({
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {SmallTile, SmallTile, 1},
        .mipLevels = 1,
        .arrayLayers = SmallSlots,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    if (!small_image) {
        LOG_ERROR(Render_Vulkan, "NVN2 direct mode: no memory for the ASTC small-level slots");
        return false;
    }
    try {
        for (u32 slot = 0; slot < SmallSlots; ++slot) {
            small_views[slot] = device.GetLogical().CreateImageView({
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .image = *small_image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
                .components{},
                .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, slot, 1},
            });
        }
        small_blocks = memory_allocator.TryCreateBuffer(
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = VkDeviceSize{SmallTile} * SmallTile * SmallSlots,
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            },
            MemoryUsage::DeviceLocal);
    } catch (const vk::Exception& exception) {
        LOG_ERROR(Render_Vulkan, "NVN2 direct mode: ASTC small-level slots: {}", exception.what());
    }
    // Ready only with the whole set: small_blocks stays empty otherwise.
    if (!small_blocks) {
        LOG_ERROR(Render_Vulkan, "NVN2 direct mode: no ASTC small-level slots");
        small_views = {};
        small_image = {};
        return false;
    }
    return true;
}

bool NvnDirectAstc::RecordSmall(VkCommandBuffer raw_cmdbuf, const nvn_direct_astc_level& level) {
    const u32 bw = level.block_width;
    const u32 bh = level.block_height;
    const u32 height_log2 = level.block_height_log2;
    if (bw == 0 || bh == 0 || level.width == 0 || level.height == 0 || level.width > SmallTile ||
        level.height > SmallTile || GobBlocksX * bw > SmallTile || GobRows * bh > SmallTile ||
        (level.source_offset % device.GetStorageBufferAlignment()) != 0) {
        return false;
    }
    // One tile from the level's start, as RecordLevel would cut it with SmallTile-sized tiles.
    const u32 gob_rows_per_block = 1U << height_log2;
    const u32 gob_columns = Common::DivCeil(Common::DivCeil(level.width, bw), GobBlocksX);
    const u32 gob_rows = Common::DivCeil(Common::DivCeil(level.height, bh), GobRows);
    const u32 tile_columns = std::max(1U, SmallTile / (GobBlocksX * bw));
    const u32 tile_rows = std::max(1U, SmallTile / (GobRows * bh));
    const u32 first_rows = tile_rows < gob_rows_per_block ? tile_rows
                                                          : tile_rows / gob_rows_per_block * gob_rows_per_block;
    if (gob_columns > tile_columns || gob_rows > first_rows || !MakeSmallResources()) {
        return false;
    }
    const vk::CommandBuffer cmdbuf{raw_cmdbuf, device.GetDispatchLoader()};
    if (!pending.empty() && pending_cmdbuf != raw_cmdbuf) {
        LOG_ERROR(Render_Vulkan, "NVN2 direct mode: {} ASTC levels left pending in an ended upload batch",
                  pending.size());
        pending.clear();
    }
    if (pending.size() == SmallSlots) {
        FlushSmall(raw_cmdbuf);
    }
    if (!small_image_initialized) {
        small_image_initialized = true;
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                               VkImageMemoryBarrier{
                                   .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .pNext = nullptr,
                                   .srcAccessMask = VK_ACCESS_NONE,
                                   .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = *small_image,
                                   .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, SmallSlots},
                               });
    }
    const u32 slot = static_cast<u32>(pending.size());
    const u32 x_shift = GobShift + height_log2;
    const TileDescriptors decode_descriptors{
        .buffer = {level.source, level.source_offset, level.source_size},
        .image = {VK_NULL_HANDLE, *small_views[slot], VK_IMAGE_LAYOUT_GENERAL},
    };
    const AstcPushConstants decode_constants{
        .blocks_dims = {bw, bh},
        .layer_stride = 0,
        .block_size = static_cast<u32>(u64{level.gobs_x} << x_shift),
        .x_shift = x_shift,
        .block_height = height_log2,
        .block_height_mask = gob_rows_per_block - 1,
        .native_tiling = level.switch2_gobs ? 1U : 0U,
    };
    cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *decode_pipeline);
    cmdbuf.PushDescriptorSetWithTemplateKHR(*decode_template, *decode_layout, 0, &decode_descriptors);
    cmdbuf.PushConstants(*decode_layout, VK_SHADER_STAGE_COMPUTE_BIT, decode_constants);
    cmdbuf.Dispatch(Common::DivCeil(Common::DivCeil(level.width, bw), 8U),
                    Common::DivCeil(Common::DivCeil(level.height, bh), 8U), 1);
    pending.push_back({slot, level});
    pending_cmdbuf = raw_cmdbuf;
    return true;
}

void NvnDirectAstc::FlushSmall(VkCommandBuffer raw_cmdbuf) {
    if (pending.empty()) {
        return;
    }
    if (pending_cmdbuf != raw_cmdbuf) {
        LOG_ERROR(Render_Vulkan, "NVN2 direct mode: {} ASTC levels pending in another command buffer",
                  pending.size());
        pending.clear();
        return;
    }
    const vk::CommandBuffer cmdbuf{raw_cmdbuf, device.GetDispatchLoader()};
    MemoryBarrier(cmdbuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    constexpr VkDeviceSize region = VkDeviceSize{SmallTile} * SmallTile;
    cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *encode_pipeline);
    for (const PendingSmall& item : pending) {
        const u32 bc_x = Common::DivCeil(item.level.width, 4U);
        const u32 bc_y = Common::DivCeil(item.level.height, 4U);
        const TileDescriptors encode_descriptors{
            .buffer = {*small_blocks, region * item.slot, region},
            .image = {VK_NULL_HANDLE, *small_views[item.slot], VK_IMAGE_LAYOUT_GENERAL},
        };
        const BcEncoderPushConstants encode_constants{
            .blocks_x = bc_x,
            .blocks_y = bc_y,
            .width = item.level.width,
            .height = item.level.height,
            .bc3 = item.level.bc3 ? 1U : 0U,
            .out_offset = 0,
        };
        cmdbuf.PushDescriptorSetWithTemplateKHR(*encode_template, *encode_layout, 0, &encode_descriptors);
        cmdbuf.PushConstants(*encode_layout, VK_SHADER_STAGE_COMPUTE_BIT, encode_constants);
        cmdbuf.Dispatch(Common::DivCeil(bc_x, 8U), Common::DivCeil(bc_y, 8U), 1);
    }
    MemoryBarrier(cmdbuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    for (const PendingSmall& item : pending) {
        const u32 bc_x = Common::DivCeil(item.level.width, 4U);
        const u32 bc_y = Common::DivCeil(item.level.height, 4U);
        const VkBufferImageCopy copy{
            .bufferOffset = region * item.slot,
            .bufferRowLength = bc_x * 4,
            .bufferImageHeight = bc_y * 4,
            .imageSubresource{VK_IMAGE_ASPECT_COLOR_BIT, item.level.level, item.level.layer, 1},
            .imageOffset{0, 0, 0},
            .imageExtent{item.level.width, item.level.height, 1},
        };
        cmdbuf.CopyBufferToImage(*small_blocks, item.level.image, VK_IMAGE_LAYOUT_GENERAL, copy);
    }
    // The next round's decodes and encodes reuse the slots.
    MemoryBarrier(cmdbuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    pending.clear();
}

int NvnDirectAstc::IsOpaque(const void* blocks, size_t size) {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_ASTC_BC1");
        return value == nullptr || value[0] != '0';
    }();
    return enabled && Tegra::Texture::ASTC::IsOpaque(
                          std::span<const u8>{static_cast<const u8*>(blocks), size})
               ? 1
               : 0;
}

bool NvnDirectAstc::MakeTileResources() {
    if (tile_resources_tried) {
        return static_cast<bool>(tile_blocks);
    }
    tile_resources_tried = true;
    // The decoder writes rgba8 and the encoder reads it as rgba8 (layout(rgba8)): the storage
    // view must be R8G8B8A8_UNORM, sRGB textures included (their BC views decode sRGB).
    tile_image = memory_allocator.TryCreateImage({
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {TileWidth, TileHeight, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    if (!tile_image) {
        LOG_ERROR(Render_Vulkan, "NVN2 direct mode: no memory for the ASTC decode image");
        return false;
    }
    try {
        tile_view = device.GetLogical().CreateImageView({
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = *tile_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .components{},
            .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        });
        // BC3 takes one byte a texel, BC1 half of it.
        tile_blocks = memory_allocator.TryCreateBuffer(
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = VkDeviceSize{TileWidth} * TileHeight,
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            },
            MemoryUsage::DeviceLocal);
    } catch (const vk::Exception& exception) {
        LOG_ERROR(Render_Vulkan, "NVN2 direct mode: ASTC decode tile: {}", exception.what());
    }
    // Ready only with the whole set: tile_blocks stays empty otherwise.
    if (!tile_blocks) {
        LOG_ERROR(Render_Vulkan, "NVN2 direct mode: no ASTC decode tile");
        tile_view = {};
        tile_image = {};
        return false;
    }
    return true;
}

bool NvnDirectAstc::RecordLevel(VkCommandBuffer raw_cmdbuf, const nvn_direct_astc_level& level) {
    const u32 bw = level.block_width;
    const u32 bh = level.block_height;
    const u32 height_log2 = level.block_height_log2;
    const u64 alignment = device.GetStorageBufferAlignment();
    if (bw == 0 || bh == 0 || level.width == 0 || level.height == 0 ||
        GobBlocksX * bw > TileWidth || GobRows * bh > TileHeight ||
        (level.source_offset % alignment) != 0 || !MakeTileResources()) {
        return false;
    }
    const vk::CommandBuffer cmdbuf{raw_cmdbuf, device.GetDispatchLoader()};
    // The decode reads the level's block-linear bytes as Lichii's swizzle parameters describe
    // them (MakeBlockLinearSwizzle2DParams), but with the stride the NVN layout gives (gobs_x:
    // a sparse texture's tiled levels pad their rows to whole 64 KiB tiles).
    const u32 x_shift = GobShift + height_log2;
    const u64 block_size = u64{level.gobs_x} << x_shift;
    const u32 gob_rows_per_block = 1U << height_log2;
    const u32 blocks_x = Common::DivCeil(level.width, bw);
    const u32 blocks_y = Common::DivCeil(level.height, bh);
    const u32 gob_columns = Common::DivCeil(blocks_x, GobBlocksX);
    const u32 gob_rows = Common::DivCeil(blocks_y, GobRows);
    // A tile is whole GOBs: up to TileWidth texels of GOB columns and TileHeight of GOB rows.
    // Its rows are whole blocks of GOBs from a block's start, or GOB rows within one block
    // (the decoder finds a block row's start from the tile's first GOB row).
    const u32 tile_columns = std::max(1U, TileWidth / (GobBlocksX * bw));
    const u32 tile_rows = std::max(1U, TileHeight / (GobRows * bh));
    if (!tile_image_initialized) {
        tile_image_initialized = true;
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                               VkImageMemoryBarrier{
                                   .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .pNext = nullptr,
                                   .srcAccessMask = VK_ACCESS_NONE,
                                   .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = *tile_image,
                                   .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               });
    }
    for (u32 row = 0; row < gob_rows;) {
        const u32 in_block = row % gob_rows_per_block;
        u32 rows = in_block != 0 || tile_rows < gob_rows_per_block
                       ? std::min(tile_rows, gob_rows_per_block - in_block)
                       : tile_rows / gob_rows_per_block * gob_rows_per_block;
        rows = std::min(rows, gob_rows - row);
        for (u32 column = 0; column < gob_columns; column += tile_columns) {
            const u32 columns = std::min(tile_columns, gob_columns - column);
            const u32 x = column * GobBlocksX * bw;
            const u32 y = row * GobRows * bh;
            const u32 width = std::min(columns * GobBlocksX * bw, level.width - x);
            const u32 height = std::min(rows * GobRows * bh, level.height - y);
            const u64 offset = u64{row / gob_rows_per_block} * block_size +
                               (u64{in_block} << GobShift) + (u64{column} << x_shift);
            if (offset >= level.source_size) {
                return false;
            }
            // The previous tile's encode and copy are done with the image and the buffer.
            MemoryBarrier(cmdbuf,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                              VK_ACCESS_TRANSFER_READ_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            const TileDescriptors decode_descriptors{
                .buffer = {level.source, level.source_offset + offset,
                           level.source_size - offset},
                .image = {VK_NULL_HANDLE, *tile_view, VK_IMAGE_LAYOUT_GENERAL},
            };
            const AstcPushConstants decode_constants{
                .blocks_dims = {bw, bh},
                .layer_stride = 0,
                .block_size = static_cast<u32>(block_size),
                .x_shift = x_shift,
                .block_height = height_log2,
                .block_height_mask = gob_rows_per_block - 1,
                .native_tiling = level.switch2_gobs ? 1U : 0U,
            };
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *decode_pipeline);
            cmdbuf.PushDescriptorSetWithTemplateKHR(*decode_template, *decode_layout, 0,
                                                    &decode_descriptors);
            cmdbuf.PushConstants(*decode_layout, VK_SHADER_STAGE_COMPUTE_BIT, decode_constants);
            cmdbuf.Dispatch(Common::DivCeil(Common::DivCeil(width, bw), 8U),
                            Common::DivCeil(Common::DivCeil(height, bh), 8U), 1);
            MemoryBarrier(cmdbuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_READ_BIT);
            const u32 bc_x = Common::DivCeil(width, 4U);
            const u32 bc_y = Common::DivCeil(height, 4U);
            const TileDescriptors encode_descriptors{
                .buffer = {*tile_blocks, 0, VK_WHOLE_SIZE},
                .image = {VK_NULL_HANDLE, *tile_view, VK_IMAGE_LAYOUT_GENERAL},
            };
            const BcEncoderPushConstants encode_constants{
                .blocks_x = bc_x,
                .blocks_y = bc_y,
                .width = width,
                .height = height,
                .bc3 = level.bc3 ? 1U : 0U,
                .out_offset = 0,
            };
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *encode_pipeline);
            cmdbuf.PushDescriptorSetWithTemplateKHR(*encode_template, *encode_layout, 0,
                                                    &encode_descriptors);
            cmdbuf.PushConstants(*encode_layout, VK_SHADER_STAGE_COMPUTE_BIT, encode_constants);
            cmdbuf.Dispatch(Common::DivCeil(bc_x, 8U), Common::DivCeil(bc_y, 8U), 1);
            MemoryBarrier(cmdbuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT);
            const VkBufferImageCopy copy{
                .bufferOffset = 0,
                .bufferRowLength = bc_x * 4,
                .bufferImageHeight = bc_y * 4,
                .imageSubresource{VK_IMAGE_ASPECT_COLOR_BIT, level.level, level.layer, 1},
                .imageOffset{static_cast<s32>(x), static_cast<s32>(y), 0},
                .imageExtent{width, height, 1},
            };
            cmdbuf.CopyBufferToImage(*tile_blocks, level.image, VK_IMAGE_LAYOUT_GENERAL, copy);
        }
        row += rows;
    }
    return true;
}

} // namespace Vulkan

#endif
