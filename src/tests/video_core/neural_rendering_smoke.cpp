// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Opt-in GPU integration test; requires a locally imported model and RGBA32F fixture.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include "numeric.h"
#include "video_core/neural_rendering/backend.h"
#include "video_core/neural_rendering/model.h"
#include "vk_context.h"

namespace NR = VideoCore::NeuralRendering;

struct Image {
    vk::Context& context;
    VkImage image{};
    VkImageView view{};
    VkDeviceMemory memory{};
    /// Allocate an RGBA16F image and view for the standalone inference test.
    Image(vk::Context& context_, uint32_t width, uint32_t height) : context(context_) {
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        create.imageType = VK_IMAGE_TYPE_2D;
        create.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        create.extent = {width, height, 1};
        create.mipLevels = create.arrayLayers = 1;
        create.samples = VK_SAMPLE_COUNT_1_BIT;
        create.tiling = VK_IMAGE_TILING_OPTIMAL;
        create.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VK_CHECK(vkCreateImage(context.device(), &create, nullptr, &image));
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(context.device(), image, &requirements);
        VkPhysicalDeviceMemoryProperties properties;
        vkGetPhysicalDeviceMemoryProperties(context.physical(), &properties);
        uint32_t type = 0;
        while (type < properties.memoryTypeCount && !(requirements.memoryTypeBits & (1u << type)))
            ++type;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        VK_CHECK(vkAllocateMemory(context.device(), &allocate, nullptr, &memory));
        VK_CHECK(vkBindImageMemory(context.device(), image, memory, 0));
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = create.format;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(context.device(), &view_info, nullptr, &view));
        auto cmd = context.beginCommands();
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = view_info.subresourceRange;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
        VkClearColorValue zero{};
        vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1,
                             &view_info.subresourceRange);
        context.endAndSubmit(cmd, true);
    }
    ~Image() {
        vkDestroyImageView(context.device(), view, nullptr);
        vkDestroyImage(context.device(), image, nullptr);
        vkFreeMemory(context.device(), memory, nullptr);
    }
};

void Barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = barrier.dstAccessMask =
        VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0,
                         nullptr);
}

/// Run the native backend on a supplied float proxy image and write outputs for visual
/// comparison.
int main(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr,
                     "neural_rendering_smoke MODEL PROXY_RGBA32F WIDTH HEIGHT OUTPUT_PREFIX\n");
        return 2;
    }
    try {
        if (!NR::IsInstalled(argv[1]))
            throw std::runtime_error("Model validation failed");
        const uint32_t width = std::stoul(argv[3]), height = std::stoul(argv[4]);
        if (!width || !height || width > 7680 || height > 4320)
            throw std::runtime_error("Invalid extent");
        std::vector<float> floats(size_t(width) * height * 4);
        std::ifstream input(argv[2], std::ios::binary);
        if (!input.read(reinterpret_cast<char*>(floats.data()), floats.size() * sizeof(float)))
            throw std::runtime_error("Fixture read failed");
        std::vector<uint16_t> pixels(floats.size());
        std::transform(floats.begin(), floats.end(), pixels.begin(), num::f16Bits);
        vk::Context context;
        Image color(context, width, height), motion(context, width / 2, height / 2);
        auto source = context.createBuffer(pixels.size() * 2, false, "source fixture");
        context.upload(source, pixels.data(), pixels.size() * 2);
        auto readback = context.createBuffer(pixels.size() * 2, true, "output readback");
        auto queries = context.createTimestampPool(2);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        {
            NR::Backend backend(context.instance(), context.physical(), context.device(),
                                context.queueFamily(), argv[1], {width, height}, color.image,
                                color.view, motion.view);
            auto evaluate = [&](NR::Style style, bool reset, bool wait) {
                auto cmd = context.beginCommands();
                Barrier(cmd);
                vkCmdCopyBufferToImage(cmd, source.buffer, color.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                                       &copy);
                Barrier(cmd);
                vkCmdResetQueryPool(cmd, queries, 0, 2);
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
                NR::Frame frame;
                frame.style = style;
                frame.reset = reset;
                frame.motion_scale = {1, 1};
                backend.Record(cmd, frame);
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
                context.endAndSubmit(cmd, wait);
            };
            auto download = [&](const std::string& name) {
                auto cmd = context.beginCommands();
                Barrier(cmd);
                vkCmdCopyImageToBuffer(cmd, color.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer,
                                       1, &copy);
                Barrier(cmd);
                context.endAndSubmit(cmd, true);
                const auto* data = static_cast<uint16_t*>(readback.mapped);
                std::vector<uint16_t> result(data, data + pixels.size());
                for (auto value : result)
                    if (!std::isfinite(num::f16ToF32(value)))
                        throw std::runtime_error("Nonfinite output");
                std::ofstream output(std::string(argv[5]) + name + ".rgba16f", std::ios::binary);
                if (!output.write(reinterpret_cast<const char*>(result.data()), result.size() * 2))
                    throw std::runtime_error("Output write failed");
                if (backend.HadTimeout())
                    throw std::runtime_error("Kernel chain timed out");
                return result;
            };
            for (uint32_t index = 0; index < 3; ++index) {
                auto style = static_cast<NR::Style>(index);
                evaluate(style, true, true);
                const auto first = download("-style" + std::to_string(index) + "-first");
                for (int f = 0; f < 8; ++f)
                    evaluate(style, false, false);
                context.waitIdle();
                download("-style" + std::to_string(index) + "-history");
                const auto times = context.readTimestampsMs(queries, 2);
                evaluate(style, true, true);
                if (download("-style" + std::to_string(index) + "-reset") != first)
                    throw std::runtime_error("History reset is not repeatable");
                std::printf("style=%u reset=PASS batched_history=PASS "
                            "preprocess_network_composite_ms=%.3f\n",
                            index, times[1] - times[0]);
                std::fflush(stdout);
            }
        }
        vkDestroyQueryPool(context.device(), queries, nullptr);
        context.destroyBuffer(readback);
        context.destroyBuffer(source);
        if (vk::Context::validationErrors())
            throw std::runtime_error("Vulkan validation errors");
        std::puts("PASS");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
