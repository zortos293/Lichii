// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Compiled in the private inference target, whose Volk loader lives in its own namespace.
#include "backend.h"
#include "kernels.h"
#include "nr_graph.h"
#include "nr_model.h"
#include "reference.h"

namespace VideoCore::NeuralRendering {
namespace {
struct Params {
    uint32_t full_width, full_height, width, height, source_width, source_height;
    float auto_mask, paper_white, color_strength, intensity, tone, structure, skin_structure, style;
    uint32_t history_valid, seed, enabled, hdr;
    float blend_scale, padding;
    std::array<float, 2> motion_scale, motion_jitter;
    float exposure, contrast, gamma, saturation, hue, vibrance, strength;
    uint32_t custom_style;
};
static_assert(sizeof(Params) == 128);

void MemoryBarrier(VkCommandBuffer commands, VkPipelineStageFlags source,
                   VkPipelineStageFlags target, VkAccessFlags reads, VkAccessFlags writes) {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = reads;
    barrier.dstAccessMask = writes;
    vkCmdPipelineBarrier(commands, source, target, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

/// Transition a color image with full read/write ordering, treating an undefined layout as
/// uninitialized.
void ImageBarrier(VkCommandBuffer commands, VkImage image, VkImageLayout before,
                  VkImageLayout after) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = before == VK_IMAGE_LAYOUT_UNDEFINED
                                ? 0
                                : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = before;
    barrier.newLayout = after;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}
} // namespace

struct Backend::Impl {
    VkDevice device{};
    VkExtent2D extent{};
    VkImage color{};
    VkImageView color_view{}, motion_view{};
    std::unique_ptr<vk::Context> context;
    std::unique_ptr<nr::Model> model;
    std::unique_ptr<nr::Kernels> kernels;
    std::unique_ptr<nr::Graph> graph;
    nr::Activation* features{};
    nr::Geometry geometry{};
    struct Image {
        VkImage image{};
        VkImageView view{};
        VkDeviceMemory memory{};
    } history[2], output;
    vk::Buffer params[2];
    VkSampler nearest{}, linear{};
    VkDescriptorPool descriptor_pool{};
    VkDescriptorSetLayout set_layout{};
    VkPipelineLayout pipeline_layout{};
    VkPipeline preprocess{}, composite{};
    VkCommandPool command_pool{};
    VkCommandBuffer compute_commands[2]{};
    VkDescriptorSet sets[2][2]{};
    uint32_t frames{};
    bool history_valid{};
    Style previous_style{};
    float previous_tone{}, previous_structure{};
    bool previous_hdr{};

    /// Wait for GPU use and destroy backend resources; the caller must hold device queue
    /// synchronization.
    ~Impl() {
        if (!context)
            return;
        vkDeviceWaitIdle(device);
        vkDestroyCommandPool(device, command_pool, nullptr);
        vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        vkDestroyPipeline(device, preprocess, nullptr);
        vkDestroyPipeline(device, composite, nullptr);
        vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        vkDestroySampler(device, nearest, nullptr);
        vkDestroySampler(device, linear, nullptr);
        for (auto* image : {&history[0], &history[1], &output}) {
            vkDestroyImageView(device, image->view, nullptr);
            vkDestroyImage(device, image->image, nullptr);
            vkFreeMemory(device, image->memory, nullptr);
        }
        for (auto& buffer : params)
            context->destroyBuffer(buffer);
        // The graph, kernels and model are destroyed before their adopted context.
    }

    /// Allocate a device-local RGBA16F history or output image and its view at the backend
    /// extent.
    void CreateImage(Image& image) {
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        create.imageType = VK_IMAGE_TYPE_2D;
        create.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        create.extent = {extent.width, extent.height, 1};
        create.mipLevels = create.arrayLayers = 1;
        create.samples = VK_SAMPLE_COUNT_1_BIT;
        create.tiling = VK_IMAGE_TILING_OPTIMAL;
        create.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VK_CHECK(vkCreateImage(device, &create, nullptr, &image.image));
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device, image.image, &requirements);
        VkPhysicalDeviceMemoryProperties memory;
        vkGetPhysicalDeviceMemoryProperties(context->physical(), &memory);
        uint32_t type = 0;
        while (type < memory.memoryTypeCount &&
               (!(requirements.memoryTypeBits & (1u << type)) ||
                !(memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
            ++type;
        if (type == memory.memoryTypeCount)
            throw std::runtime_error("No NR image memory type");
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        VK_CHECK(vkAllocateMemory(device, &allocate, nullptr, &image.memory));
        VK_CHECK(vkBindImageMemory(device, image.image, image.memory, 0));
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = create.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device, &view, nullptr, &image.view));
    }

    void CreatePipeline(VkPipeline& pipeline, const char* name) {
        VkComputePipelineCreateInfo create{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        create.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        create.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        create.stage.module = context->loadShaderModule(name);
        create.stage.pName = "main";
        create.layout = pipeline_layout;
        VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &create, nullptr, &pipeline));
    }

    /// Upload constants, warm the inference graph, and create frame resources while device
    /// queues are serialized.
    void Initialize() {
        geometry = nr::Geometry::fromValid(extent.width, extent.height);
        kernels = std::make_unique<nr::Kernels>(*context, "embedded");
        kernels->setSiluTable(ref::siluTable());
        graph =
            std::make_unique<nr::Graph>(*context, *model, *kernels, geometry, nr::Graph::Options{});
        features = graph->allocate("input features", geometry.fullWidth * geometry.fullHeight, 16,
                                   nr::Format::F32);
        context->fillZero(features->buffer);
        // First recording allocates tensors and uploads constants. Do this while the renderer is
        // idle.
        auto warmup = context->beginCommands();
        graph->record(warmup, *features);
        context->endAndSubmit(warmup, true);
        if (kernels->chainTimeouts().waits)
            throw std::runtime_error("NR kernel warmup timed out");

        for (auto* image : {&history[0], &history[1], &output})
            CreateImage(*image);
        auto commands = context->beginCommands();
        VkClearColorValue zero{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (auto* image : {&history[0], &history[1], &output}) {
            ImageBarrier(commands, image->image, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_GENERAL);
            vkCmdClearColorImage(commands, image->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        }
        context->endAndSubmit(commands, true);
        for (auto& buffer : params)
            buffer = context->createBuffer(sizeof(Params), false, "nr params",
                                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
        VK_CHECK(vkCreateSampler(device, &sampler, nullptr, &nearest));
        sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
        VK_CHECK(vkCreateSampler(device, &sampler, nullptr, &linear));
        const VkDescriptorSetLayoutBinding bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = 7;
        layout.pBindings = bindings;
        VK_CHECK(vkCreateDescriptorSetLayout(device, &layout, nullptr, &set_layout));
        VkPipelineLayoutCreateInfo pipeline{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeline.setLayoutCount = 1;
        pipeline.pSetLayouts = &set_layout;
        VK_CHECK(vkCreatePipelineLayout(device, &pipeline, nullptr, &pipeline_layout));
        CreatePipeline(preprocess, "nr_preprocess.spv");
        CreatePipeline(composite, "nr_composite.spv");
        const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12},
                                              {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
                                              {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4},
                                              {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 4;
        pool.poolSizeCount = 4;
        pool.pPoolSizes = sizes;
        VK_CHECK(vkCreateDescriptorPool(device, &pool, nullptr, &descriptor_pool));
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = descriptor_pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &set_layout;
        for (uint32_t h = 0; h < 2; ++h) {
            for (uint32_t pass = 0; pass < 2; ++pass) {
                VK_CHECK(vkAllocateDescriptorSets(device, &allocate, &sets[h][pass]));
                const VkDescriptorImageInfo images[] = {
                    {nearest, color_view, VK_IMAGE_LAYOUT_GENERAL},
                    {linear, history[h].view, VK_IMAGE_LAYOUT_GENERAL},
                    {nearest, motion_view, VK_IMAGE_LAYOUT_GENERAL},
                    {VK_NULL_HANDLE, output.view, VK_IMAGE_LAYOUT_GENERAL},
                    {VK_NULL_HANDLE, history[1 - h].view, VK_IMAGE_LAYOUT_GENERAL}};
                const VkDescriptorBufferInfo buffers[] = {
                    {pass ? graph->head().buffer.buffer : features->buffer.buffer, 0,
                     VK_WHOLE_SIZE},
                    {params[h].buffer, 0, sizeof(Params)}};
                VkWriteDescriptorSet writes[7]{};
                for (uint32_t i = 0; i < 7; ++i) {
                    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[i].dstSet = sets[h][pass];
                    writes[i].dstBinding = i;
                    writes[i].descriptorCount = 1;
                    writes[i].descriptorType = bindings[i].descriptorType;
                    if (i == 3 || i == 4)
                        writes[i].pBufferInfo = &buffers[i - 3];
                    else
                        writes[i].pImageInfo = &images[i < 3 ? i : i - 2];
                }
                vkUpdateDescriptorSets(device, 7, writes, 0, nullptr);
            }
        }
        VkCommandPoolCreateInfo command_pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        command_pool_info.queueFamilyIndex = context->queueFamily();
        VK_CHECK(vkCreateCommandPool(device, &command_pool_info, nullptr, &command_pool));
        VkCommandBufferAllocateInfo command_allocate{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command_allocate.commandPool = command_pool;
        command_allocate.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        command_allocate.commandBufferCount = 2;
        VK_CHECK(vkAllocateCommandBuffers(device, &command_allocate, compute_commands));
        RecordCommands();
    }

    /// Rebuild secondary commands for both history parities after previous backend commands
    /// have completed.
    void RecordCommands() {
        VK_CHECK(vkResetCommandPool(device, command_pool, 0));
        for (uint32_t h = 0; h < 2; ++h) {
            context->resetDescriptorPool(h);
            VkCommandBufferInheritanceInfo inheritance{
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
            begin.pInheritanceInfo = &inheritance;
            auto cmd = compute_commands[h];
            VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, preprocess);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                                    &sets[h][0], 0, nullptr);
            vkCmdDispatch(cmd, (geometry.fullWidth + 7) / 8, (geometry.fullHeight + 7) / 8, 1);
            context->computeBarrier(cmd);
            graph->record(cmd, *features);
            MemoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                                    &sets[h][1], 0, nullptr);
            vkCmdDispatch(cmd, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
            VK_CHECK(vkEndCommandBuffer(cmd));
        }
    }
};

/// Adopt renderer resources and initialize a preverified model; queues must be idle and
/// externally synchronized.
Backend::Backend(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                 uint32_t queue_family, const std::filesystem::path& model, VkExtent2D extent,
                 VkImage color, VkImageView color_view, VkImageView motion_view)
    : impl{std::make_unique<Impl>()} {
    if (!extent.width || !extent.height || extent.width > 7680 || extent.height > 4320)
        throw std::runtime_error("Unsupported neural rendering extent");
    impl->device = device;
    impl->extent = extent;
    impl->color = color;
    impl->color_view = color_view;
    impl->motion_view = motion_view;
    impl->context = std::make_unique<vk::Context>(instance, physical, device, queue_family);
    // The caller already verified the installation with IsInstalled() (see backend.h).
    impl->model = std::make_unique<nr::Model>(*impl->context, model.string(), false);
    impl->Initialize();
}

Backend::~Backend() = default;

/// Replace color and motion descriptors and rerecord commands without resetting history; prior
/// GPU use must finish.
void Backend::Rebind(VkImage color, VkImageView color_view, VkImageView motion_view) {
    auto& state = *impl;
    state.color = color;
    state.color_view = color_view;
    state.motion_view = motion_view;
    const VkDescriptorImageInfo images[] = {{state.nearest, color_view, VK_IMAGE_LAYOUT_GENERAL},
                                            {state.nearest, motion_view, VK_IMAGE_LAYOUT_GENERAL}};
    for (auto& parity : state.sets) {
        for (auto set : parity) {
            VkWriteDescriptorSet writes[2]{};
            for (uint32_t i = 0; i < 2; ++i) {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = set;
                writes[i].dstBinding = i * 2;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].pImageInfo = &images[i];
            }
            vkUpdateDescriptorSets(state.device, 2, writes, 0, nullptr);
        }
    }
    state.RecordCommands();
}

/// Append inference and output copy commands, resetting history when conditioning or HDR changes.
void Backend::Record(VkCommandBuffer commands, const Frame& frame) {
    auto& state = *impl;
    const uint32_t h = state.frames & 1u;
    const bool reset = frame.reset || !state.history_valid || frame.style != state.previous_style ||
                       frame.tone != state.previous_tone ||
                       frame.structure != state.previous_structure || frame.hdr != state.previous_hdr;
    Params params{};
    params.full_width = state.geometry.fullWidth;
    params.full_height = state.geometry.fullHeight;
    params.width = params.source_width = state.extent.width;
    params.height = params.source_height = state.extent.height;
    params.auto_mask = params.paper_white = params.color_strength = 1.0f;
    params.intensity = frame.intensity;
    params.tone = frame.tone;
    params.structure = frame.structure;
    params.skin_structure = -1.0f;
    params.style = static_cast<float>(frame.style);
    params.history_valid = !reset;
    params.seed = reset ? 0 : state.frames;
    params.enabled = 1;
    params.hdr = frame.hdr;
    params.blend_scale = 1.0f;
    params.motion_scale = frame.motion_scale;
    params.motion_jitter = frame.motion_jitter;
    // Serialize reuse of all graph scratch, parameter and history resources, including prior
    // frames.
    MemoryBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                  VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                  VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT);
    vkCmdUpdateBuffer(commands, state.params[h].buffer, 0, sizeof(params), &params);
    MemoryBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_UNIFORM_READ_BIT);
    vkCmdExecuteCommands(commands, 1, &state.compute_commands[h]);
    ImageBarrier(commands, state.output.image, VK_IMAGE_LAYOUT_GENERAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ImageBarrier(commands, state.color, VK_IMAGE_LAYOUT_GENERAL,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {state.extent.width, state.extent.height, 1};
    vkCmdCopyImage(commands, state.output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, state.color,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    ImageBarrier(commands, state.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_GENERAL);
    ImageBarrier(commands, state.output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_IMAGE_LAYOUT_GENERAL);
    state.history_valid = true;
    state.previous_style = frame.style;
    state.previous_tone = frame.tone;
    state.previous_structure = frame.structure;
    state.previous_hdr = frame.hdr;
    ++state.frames;
}

bool Backend::HadTimeout() const {
    return impl->kernels->chainTimeouts().waits != 0;
}
} // namespace VideoCore::NeuralRendering
