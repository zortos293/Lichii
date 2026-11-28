// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <bit>
#include <cstring>
#include <span>

#include "common/alignment.h"
#include "common/common_types.h"
#include "video_core/texture_cache/types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

struct DepthStencilDownload {
    std::array<VkBufferImageCopy, 2> regions;
    size_t texels;

    [[nodiscard]] size_t EndOffset() const noexcept {
        return regions[1].bufferOffset + texels;
    }

    void Pack(std::span<u8> output, std::span<const u8> staging, bool float_depth,
              bool low_stencil) const {
        const u8* const depth = staging.data() + regions[0].bufferOffset;
        const u8* const stencil = staging.data() + regions[1].bufferOffset;
        for (size_t i = 0; i < texels; ++i) {
            u32 word;
            std::memcpy(&word, depth + i * sizeof(word), sizeof(word));
            u32 d24 = word & 0x00ff'ffff;
            if (float_depth) {
                const float value = std::bit_cast<float>(word);
                d24 = !(value > 0.0f) ? 0
                    : value >= 1.0f ? 0x00ff'ffff
                                    : static_cast<u32>(static_cast<double>(value) * 16777215.0 + 0.5);
            }
            const u32 s8 = stencil[i];
            word = low_stencil ? (d24 << 8) | s8 : d24 | (s8 << 24);
            std::memcpy(output.data() + i * sizeof(word), &word, sizeof(word));
        }
    }

    void AppendPackedCopies(const VideoCommon::BufferImageCopy& destination, size_t source_offset,
                            auto& copies) const {
        const auto [width, height, depth] = destination.image_extent;
        const u32 row = destination.buffer_row_length ? destination.buffer_row_length : width;
        const u32 rows = destination.buffer_image_height ? destination.buffer_image_height : height;
        const size_t slices = size_t{depth} * destination.image_subresource.num_layers;
        if (row == width && (slices == 1 || rows == height)) {
            copies.push_back({source_offset, destination.buffer_offset, texels * sizeof(u32)});
            return;
        }
        for (size_t layer = 0; layer < slices; ++layer) {
            for (size_t y = 0; y < height; ++y) {
                copies.push_back({
                    source_offset + (layer * height + y) * width * sizeof(u32),
                    destination.buffer_offset + (layer * rows + y) * row * sizeof(u32),
                    width * sizeof(u32),
                });
            }
        }
    }
};

[[nodiscard]] inline DepthStencilDownload SplitDepthStencilDownload(VkBufferImageCopy copy,
                                                                     size_t offset) {
    const auto [width, height, depth] = copy.imageExtent;
    const size_t texels = size_t{width} * height * depth * copy.imageSubresource.layerCount;
    copy.bufferOffset = Common::AlignUp(offset, size_t{4});
    copy.bufferRowLength = 0;
    copy.bufferImageHeight = 0;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    VkBufferImageCopy stencil = copy;
    stencil.bufferOffset += texels * sizeof(u32);
    stencil.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    return {{copy, stencil}, texels};
}

} // namespace Vulkan
