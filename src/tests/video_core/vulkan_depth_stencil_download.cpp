// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/vk_depth_stencil_download.h"

TEST_CASE("Depth and stencil transfer regions keep separate tightly packed planes",
          "[video_core][vulkan]") {
    const VkBufferImageCopy copy{
        .bufferOffset = 128,
        .bufferRowLength = 9,
        .bufferImageHeight = 10,
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .mipLevel = 2,
                             .baseArrayLayer = 4, .layerCount = 3},
        .imageOffset = {1, 2, 0},
        .imageExtent = {5, 3, 1},
    };
    const auto split = Vulkan::SplitDepthStencilDownload(copy, 7);
    CHECK(split.texels == 45);
    CHECK(split.regions[0].bufferOffset == 8);
    CHECK(split.regions[1].bufferOffset == 188);
    CHECK(split.EndOffset() == 233);
    CHECK(split.regions[0].imageSubresource.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT);
    CHECK(split.regions[1].imageSubresource.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT);
    for (const auto& region : split.regions) {
        CHECK(region.bufferRowLength == 0);
        CHECK(region.bufferImageHeight == 0);
        CHECK(region.imageSubresource.mipLevel == 2);
        CHECK(region.imageSubresource.baseArrayLayer == 4);
        CHECK(region.imageSubresource.layerCount == 3);
        CHECK(region.imageOffset.y == 2);
        CHECK(region.imageExtent.height == 3);
    }
    CHECK(Vulkan::SplitDepthStencilDownload(copy, split.EndOffset()).regions[0].bufferOffset ==
          236);
    std::vector<VkBufferCopy> packed;
    split.AppendPackedCopies(
        {.buffer_offset = 128, .buffer_row_length = 9, .buffer_image_height = 10,
         .image_subresource = {.num_layers = 3}, .image_extent = {5, 3, 1}},
        32, packed);
    REQUIRE(packed.size() == 9);
    CHECK(packed[1].srcOffset == 52);
    CHECK(packed[1].dstOffset == 164);
    CHECK(packed[1].size == 20);
    CHECK(packed[3].srcOffset == 92);
    CHECK(packed[3].dstOffset == 488);
}

TEST_CASE("Packed Vulkan depth/stencil downloads restore both guest byte orders",
          "[video_core][vulkan]") {
    const auto split = Vulkan::SplitDepthStencilDownload(
        {.imageSubresource = {.layerCount = 1}, .imageExtent = {4, 1, 1}}, 0);
    const std::array<u8, 4> stencil{0x12, 0x34, 0x56, 0x78};
    for (const bool float_depth : {false, true}) {
        const auto depth = float_depth
                               ? std::array{std::bit_cast<u32>(-0.125f), std::bit_cast<u32>(0.5f),
                                            std::bit_cast<u32>(1.25f),
                                            std::bit_cast<u32>(std::numeric_limits<float>::quiet_NaN())}
                               : std::array{0xab00'0000U, 0x1280'0000U, 0x43ff'ffffU, 0x0100'0000U};
        std::array<u8, 20> staging{};
        std::memcpy(staging.data(), depth.data(), sizeof(depth));
        std::memcpy(staging.data() + sizeof(depth), stencil.data(), stencil.size());
        std::array<u32, 4> words{};
        const std::span<u8> output{reinterpret_cast<u8*>(words.data()), sizeof(words)};
        split.Pack(output, staging, float_depth, false);
        CHECK(words == std::array{0x1200'0000U, 0x3480'0000U, 0x56ff'ffffU, 0x7800'0000U});
        split.Pack(output, staging, float_depth, true);
        CHECK(words == std::array{0x0000'0012U, 0x8000'0034U, 0xffff'ff56U, 0x0000'0078U});
    }
}
