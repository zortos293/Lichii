// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <future>
#include <memory>

#include "common/scope_exit.h"
#include "common/thread_worker.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

struct FastLinkedGraphicsPipeline {
    std::array<vk::Pipeline, 4> libraries;
    vk::Pipeline pipeline;
};

[[nodiscard]] inline FastLinkedGraphicsPipeline BuildFastLinkedGraphicsPipeline(
    const vk::Device& device, VkPipelineCache cache, const VkGraphicsPipelineCreateInfo& whole,
    Common::ThreadWorker& fragment_workers) {
    FastLinkedGraphicsPipeline result;
    const auto create = [&](const VkGraphicsPipelineCreateInfo& ci) {
        auto pipeline = device.CreateGraphicsPipeline(ci, cache);
        if (!pipeline) {
            throw vk::Exception{VK_ERROR_INITIALIZATION_FAILED};
        }
        return pipeline;
    };
    const auto build_library = [&](VkGraphicsPipelineLibraryFlagBitsEXT subset) {
        const bool vertex_input = subset == VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
        const bool pre_raster = subset == VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT;
        const bool fragment = subset == VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
        const bool output = subset == VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
        const VkGraphicsPipelineLibraryCreateInfoEXT library_ci{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
            .flags = subset,
        };
        auto rasterization = *whole.pRasterizationState;
        rasterization.rasterizerDiscardEnable = VK_FALSE;
        return create({
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &library_ci,
            .flags = whole.flags | VK_PIPELINE_CREATE_LIBRARY_BIT_KHR,
            .stageCount = pre_raster || fragment ? 1U : 0U,
            .pStages = pre_raster ? &whole.pStages[0] : fragment ? &whole.pStages[1] : nullptr,
            .pVertexInputState = vertex_input ? whole.pVertexInputState : nullptr,
            .pInputAssemblyState = vertex_input ? whole.pInputAssemblyState : nullptr,
            .pViewportState = pre_raster ? whole.pViewportState : nullptr,
            .pRasterizationState = pre_raster ? &rasterization : nullptr,
            .pMultisampleState = fragment || output ? whole.pMultisampleState : nullptr,
            .pDepthStencilState = fragment ? whole.pDepthStencilState : nullptr,
            .pColorBlendState = output ? whole.pColorBlendState : nullptr,
            .pDynamicState = whole.pDynamicState,
            .layout = pre_raster || fragment ? whole.layout : VK_NULL_HANDLE,
            .renderPass = vertex_input ? VK_NULL_HANDLE : whole.renderPass,
            .subpass = vertex_input ? 0U : whole.subpass,
        });
    };
    result.libraries[0] = build_library(VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT);
    const auto build_fragment = [&] {
        return build_library(VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT);
    };
    auto task = std::make_shared<std::packaged_task<vk::Pipeline()>>(build_fragment);
    auto fragment = task->get_future();
    if (!fragment_workers.TryQueueWork([task] { (*task)(); })) {
        fragment = {};
    }
    SCOPE_EXIT {
        if (fragment.valid()) {
            fragment.wait();
        }
    };
    result.libraries[1] =
        build_library(VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT);
    result.libraries[2] = fragment.valid() ? fragment.get() : build_fragment();
    result.libraries[3] = build_library(VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT);

    const std::array handles{*result.libraries[0], *result.libraries[1], *result.libraries[2],
                             *result.libraries[3]};
    const VkPipelineLibraryCreateInfoKHR link_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
        .libraryCount = static_cast<u32>(handles.size()),
        .pLibraries = handles.data(),
    };
    result.pipeline = create({
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &link_ci,
        .flags = whole.flags,
        .layout = whole.layout,
        .renderPass = whole.renderPass,
        .subpass = whole.subpass,
    });
    return result;
}

} // namespace Vulkan
