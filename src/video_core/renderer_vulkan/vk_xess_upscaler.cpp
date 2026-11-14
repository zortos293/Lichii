// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_xess_upscaler.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>

#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/xess_runtime.h"

namespace Vulkan {
namespace {

constexpr VkFormat COLOR_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkImageSubresourceRange COLOR_RANGE{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

#ifdef _WIN32
VkImageMemoryBarrier Barrier(VkImage image, VkImageSubresourceRange range,
                             VkAccessFlags src_access, VkAccessFlags dst_access,
                             VkImageLayout old_layout, VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = range,
    };
}

VkImageSubresourceRange Range(VkImageAspectFlags aspect) {
    return VkImageSubresourceRange{
        .aspectMask = aspect,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

void LogXeSS(const char* message, xess_logging_level_t level) {
    switch (level) {
    case XESS_LOGGING_LEVEL_ERROR:
        LOG_ERROR(Render_Vulkan, "XeSS: {}", message ? message : "");
        break;
    case XESS_LOGGING_LEVEL_WARNING:
        LOG_WARNING(Render_Vulkan, "XeSS: {}", message ? message : "");
        break;
    default:
        LOG_INFO(Render_Vulkan, "XeSS: {}", message ? message : "");
        break;
    }
}

xess_quality_settings_t ToXeSSQuality(Settings::XessQuality quality) {
    switch (quality) {
    case Settings::XessQuality::Quality:
        return XESS_QUALITY_SETTING_QUALITY;
    case Settings::XessQuality::Balanced:
        return XESS_QUALITY_SETTING_BALANCED;
    case Settings::XessQuality::Performance:
        return XESS_QUALITY_SETTING_PERFORMANCE;
    case Settings::XessQuality::UltraPerformance:
        return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
    case Settings::XessQuality::UltraQuality:
    default:
        return XESS_QUALITY_SETTING_ULTRA_QUALITY;
    }
}

xess_vk_image_view_info ImageInfo(VkImage image, VkImageView view, VkFormat format,
                                  VkExtent2D extent, VkImageAspectFlags aspect) {
    return xess_vk_image_view_info{
        .imageView = view,
        .image = image,
        .subresourceRange = Range(aspect),
        .format = format,
        .width = extent.width,
        .height = extent.height,
    };
}
#endif

} // namespace

struct XessUpscaler::Impl {
    Impl(const Device& device_, Scheduler& scheduler_, MemoryAllocator& allocator_,
         Settings::XessQuality quality_)
        : device{device_}, scheduler{scheduler_}, allocator{allocator_}, quality{quality_} {}

    ~Impl() {
#ifdef _WIN32
        scheduler.Finish();
        if (context) {
            const XeSS::Api* const api = XeSS::GetApi();
            xess_context_handle_t old_context = context;
            scheduler.Record([api, old_context](vk::CommandBuffer) {
                api->destroy_context(old_context);
            });
            scheduler.Finish();
            context = nullptr;
        }
#endif
    }

    void CreateOutput(VkExtent2D extent) {
        output_view = {};
        output_image = {};
        output_image = allocator.CreateImage(VkImageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = COLOR_FORMAT,
            .extent = {extent.width, extent.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        });
        output_view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = *output_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = COLOR_FORMAT,
            .components{},
            .subresourceRange = COLOR_RANGE,
        });
        output_size = extent;
#ifdef _WIN32
        output_initialized.store(false, std::memory_order_relaxed);
#endif
        ready.store(false, std::memory_order_relaxed);
        initialization_scheduled.store(false, std::memory_order_relaxed);
        failed.store(false, std::memory_order_relaxed);
    }

#ifdef _WIN32
    void Initialize(VkExtent2D input_size, VkExtent2D output_extent) {
        const XeSS::Api* const api = XeSS::GetApi();
        if (!api || (!context && api->create_context(
                 device.GetInstance(), static_cast<VkPhysicalDevice>(device.GetPhysical()),
                 *device.GetLogical(), &context) != XESS_RESULT_SUCCESS)) {
            Fail("could not create the Vulkan context");
            return;
        }
        api->set_logging_callback(context, XESS_LOGGING_LEVEL_WARNING, LogXeSS);
        const xess_2d_t output_resolution{output_extent.width, output_extent.height};
        const xess_quality_settings_t sdk_quality = ToXeSSQuality(quality);
        xess_2d_t optimal{};
        xess_2d_t minimum{};
        xess_2d_t maximum{};
        if (api->get_optimal_input_resolution(context, &output_resolution, sdk_quality, &optimal,
                                              &minimum, &maximum) != XESS_RESULT_SUCCESS) {
            Fail("could not determine the selected quality mode's input range");
            return;
        }
        minimum_input = minimum;
        maximum_input = maximum;
        if (!SupportsInput(input_size)) {
            LOG_INFO(Render_Vulkan,
                     "XeSS-SR quality mode does not support Ocarina input {}x{} for output {}x{} "
                     "(optimal {}x{}); using the selected fallback",
                     input_size.width, input_size.height, output_extent.width, output_extent.height,
                     optimal.x, optimal.y);
            initialization_scheduled.store(false, std::memory_order_release);
            return;
        }
        constexpr u32 flags = XESS_INIT_FLAG_LDR_INPUT_COLOR;
        if (api->build_pipelines(context, VK_NULL_HANDLE, false, flags) < 0) {
            Fail("pipeline build request failed");
            return;
        }
        const xess_vk_init_params_t init_params{
            .outputResolution = output_resolution,
            .qualitySetting = sdk_quality,
            .initFlags = flags,
            .creationNodeMask = 0,
            .visibleNodeMask = 0,
            .tempBufferHeap = VK_NULL_HANDLE,
            .bufferHeapOffset = 0,
            .tempTextureHeap = VK_NULL_HANDLE,
            .textureHeapOffset = 0,
            .pipelineCache = VK_NULL_HANDLE,
        };
        if (api->init(context, &init_params) < 0) {
            Fail("initialization failed");
            return;
        }
        api->set_jitter_scale(context, 1.0f, 1.0f);
        api->set_velocity_scale(context, 1.0f, 1.0f);
        if (api->is_optimal_driver(context) == XESS_RESULT_WARNING_OLD_DRIVER) {
            LOG_WARNING(Render_Vulkan, "XeSS-SR reports that the installed Intel driver is not optimal");
        }
        first_execution = true;
        ready.store(true, std::memory_order_release);
        LOG_INFO(Render_Vulkan, "XeSS-SR initialized for Ocarina at {}x{} quality={}",
                 output_extent.width, output_extent.height, static_cast<u32>(quality));
    }

    void Execute(const Inputs& inputs) {
        const XeSS::Api* const api = XeSS::GetApi();
        if (!api || !context || failed.load(std::memory_order_acquire)) {
            return;
        }
        const bool reset_history =
            history_reset_pending.exchange(false, std::memory_order_acq_rel);
        constexpr VkAccessFlags WRITES = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                         VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        const std::array<VkImageMemoryBarrier, 4> to_read{
            Barrier(inputs.color_image, Range(VK_IMAGE_ASPECT_COLOR_BIT), WRITES,
                    VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(inputs.motion_image, Range(VK_IMAGE_ASPECT_COLOR_BIT), WRITES,
                    VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(inputs.depth_image, Range(VK_IMAGE_ASPECT_DEPTH_BIT), WRITES,
                    VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(*output_image, COLOR_RANGE,
                    output_initialized.load(std::memory_order_relaxed)
                        ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                        : 0,
                    VK_ACCESS_SHADER_WRITE_BIT,
                    output_initialized.load(std::memory_order_relaxed)
                        ? VK_IMAGE_LAYOUT_GENERAL
                        : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_GENERAL),
        };
        scheduler.Record([this, api, inputs, to_read, reset_history](vk::CommandBuffer cmdbuf) {
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, to_read);
            const VkExtent2D color_size = inputs.render_size;
            xess_vk_execute_params_t params{};
            params.colorTexture = ImageInfo(inputs.color_image, inputs.color_view, COLOR_FORMAT,
                                           color_size, VK_IMAGE_ASPECT_COLOR_BIT);
            params.velocityTexture = ImageInfo(inputs.motion_image, inputs.motion_view,
                                               VK_FORMAT_R16G16_SFLOAT, color_size,
                                               VK_IMAGE_ASPECT_COLOR_BIT);
            params.depthTexture = ImageInfo(inputs.depth_image, inputs.depth_view,
                                            VK_FORMAT_D32_SFLOAT, inputs.depth_size,
                                            VK_IMAGE_ASPECT_DEPTH_BIT);
            params.outputTexture = ImageInfo(*output_image, *output_view, COLOR_FORMAT,
                                             inputs.output_size, VK_IMAGE_ASPECT_COLOR_BIT);
            params.jitterOffsetX = inputs.jitter[0];
            params.jitterOffsetY = inputs.jitter[1];
            params.exposureScale = 1.0f;
            params.resetHistory = inputs.reset || first_execution || reset_history;
            params.inputWidth = color_size.width;
            params.inputHeight = color_size.height;
            const xess_result_t result = api->execute(context, *cmdbuf, &params);
            if (result < 0) {
                Fail("frame evaluation failed");
            } else {
                first_execution = false;
                output_initialized.store(true, std::memory_order_relaxed);
            }
            const std::array<VkImageMemoryBarrier, 4> back{
                Barrier(inputs.color_image, Range(VK_IMAGE_ASPECT_COLOR_BIT),
                        VK_ACCESS_SHADER_READ_BIT, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL),
                Barrier(inputs.motion_image, Range(VK_IMAGE_ASPECT_COLOR_BIT),
                        VK_ACCESS_SHADER_READ_BIT, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL),
                Barrier(inputs.depth_image, Range(VK_IMAGE_ASPECT_DEPTH_BIT),
                        VK_ACCESS_SHADER_READ_BIT, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL),
                Barrier(*output_image, COLOR_RANGE, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL),
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, {}, {}, back);
        });
    }

    bool SupportsInput(VkExtent2D input_size) const {
        return static_cast<u64>(input_size.width) * output_size.height ==
                   static_cast<u64>(input_size.height) * output_size.width &&
               input_size.width >= minimum_input.x && input_size.height >= minimum_input.y &&
               input_size.width <= maximum_input.x && input_size.height <= maximum_input.y;
    }

    void Fail(std::string_view reason) {
        if (!failed.exchange(true, std::memory_order_acq_rel)) {
            LOG_WARNING(Render_Vulkan, "XeSS-SR {}; using the selected fallback", reason);
        }
    }
#else
    void Initialize(VkExtent2D, VkExtent2D) {}
    void Execute(const Inputs&) {}
    void Fail(std::string_view) {
        failed.store(true, std::memory_order_release);
    }
#endif

    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& allocator;
    Settings::XessQuality quality;
    vk::Image output_image;
    vk::ImageView output_view;
    VkExtent2D output_size{};
    std::atomic<bool> ready{};
    std::atomic<bool> initialization_scheduled{};
    std::atomic<bool> failed{};
#ifdef _WIN32
    xess_context_handle_t context{};
    xess_2d_t minimum_input{};
    xess_2d_t maximum_input{};
    bool first_execution{true};
    std::atomic<bool> history_reset_pending{};
    std::atomic<bool> output_initialized{};
#endif
};

XessUpscaler::XessUpscaler(const Device& device, Scheduler& scheduler,
                           MemoryAllocator& memory_allocator, Settings::XessQuality quality)
    : impl{std::make_unique<Impl>(device, scheduler, memory_allocator, quality)} {}

XessUpscaler::~XessUpscaler() = default;

bool XessUpscaler::IsSupported(const Device& device) {
    if (!XeSS::IsAvailable() || !device.IsXeSSSupported()) {
        return false;
    }
    constexpr VkFormatFeatureFlags usage =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    return device.IsFormatSupported(COLOR_FORMAT, usage, FormatType::Optimal);
}

bool XessUpscaler::Failed() const noexcept {
    return impl->failed.load(std::memory_order_acquire);
}

bool XessUpscaler::Upscale(const Inputs& inputs) {
#ifdef _WIN32
    Impl& state = *impl;
    if (state.failed.load(std::memory_order_acquire)) {
        return false;
    }
    if (inputs.output_size.width == 0 || inputs.output_size.height == 0) {
        return false;
    }
    if (state.output_size.width != inputs.output_size.width ||
        state.output_size.height != inputs.output_size.height) {
        if (state.output_size.width != 0 || state.context) {
            state.scheduler.Finish();
            if (state.context) {
                const XeSS::Api* const api = XeSS::GetApi();
                const xess_context_handle_t context = state.context;
                state.scheduler.Record([api, context](vk::CommandBuffer) {
                    api->destroy_context(context);
                });
                state.scheduler.Finish();
                state.context = nullptr;
            }
        }
        state.CreateOutput(inputs.output_size);
    }
    if (!state.output_image) {
        state.CreateOutput(inputs.output_size);
    }
    const bool ready = state.ready.load(std::memory_order_acquire);
    if (ready && !state.SupportsInput(inputs.render_size)) {
        state.history_reset_pending.store(true, std::memory_order_release);
        return false;
    }
    if (!ready) {
        if (!state.initialization_scheduled.exchange(true, std::memory_order_acq_rel)) {
            state.scheduler.Record([&state, input_size = inputs.render_size,
                                    output_size = inputs.output_size](vk::CommandBuffer) {
                state.Initialize(input_size, output_size);
            });
        }
        return false;
    }
    state.Execute(inputs);
    return true;
#else
    (void)inputs;
    return false;
#endif
}

VkImage XessUpscaler::OutputImage() const noexcept {
    return impl->output_image ? *impl->output_image : VK_NULL_HANDLE;
}

VkImageView XessUpscaler::OutputView() const noexcept {
    return impl->output_view ? *impl->output_view : VK_NULL_HANDLE;
}

} // namespace Vulkan