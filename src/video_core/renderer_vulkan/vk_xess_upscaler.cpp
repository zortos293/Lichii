// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_xess_upscaler.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <limits>
#include <mutex>

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

xess_quality_settings_t ToXeSSRatioQuality(VkExtent2D input, VkExtent2D output) {
    if (input.width == 0 || input.height == 0 || output.width == 0 || output.height == 0 ||
        (input.width == output.width && input.height == output.height)) {
        return XESS_QUALITY_SETTING_AA;
    }
    const double ratio_width = static_cast<double>(output.width) / input.width;
    const double ratio_height = static_cast<double>(output.height) / input.height;
    const double ratio = (ratio_width + ratio_height) * 0.5;
    struct Preset {
        double ratio;
        xess_quality_settings_t quality;
    };
    constexpr std::array presets{
        Preset{1.30, XESS_QUALITY_SETTING_ULTRA_QUALITY},
        Preset{1.50, XESS_QUALITY_SETTING_ULTRA_QUALITY},
        Preset{1.70, XESS_QUALITY_SETTING_QUALITY},
        Preset{2.00, XESS_QUALITY_SETTING_BALANCED},
        Preset{2.30, XESS_QUALITY_SETTING_PERFORMANCE},
        Preset{3.00, XESS_QUALITY_SETTING_ULTRA_PERFORMANCE},
    };
    const Preset* closest = &presets[0];
    double closest_difference = std::abs(ratio - closest->ratio);
    for (const auto& preset : presets) {
        const double difference = std::abs(ratio - preset.ratio);
        if (difference < closest_difference) {
            closest = &preset;
            closest_difference = difference;
        }
    }
    return closest->quality;
}

VkExtent2D ScaleExtentAtQualityRatio(VkExtent2D input, double scale) {
    if (input.width == 0 || input.height == 0 || !std::isfinite(scale) || scale < 1.0) {
        return {};
    }
    const double scaled_width = input.width * scale;
    const double scaled_height = input.height * scale;
    if (scaled_width > std::numeric_limits<s32>::max() ||
        scaled_height > std::numeric_limits<s32>::max()) {
        return {};
    }
    return {static_cast<u32>(std::llround(scaled_width)),
            static_cast<u32>(std::llround(scaled_height))};
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
    }

#ifdef _WIN32
    void CreateIntermediate(VkExtent2D extent) {
        intermediate_view = {};
        intermediate_image = {};
        intermediate_image = allocator.CreateImage(VkImageCreateInfo{
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
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        });
        intermediate_view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = *intermediate_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = COLOR_FORMAT,
            .components{},
            .subresourceRange = COLOR_RANGE,
        });
        intermediate_initialized.store(false, std::memory_order_relaxed);
    }

    void DestroyContext() {
        scheduler.Finish();
        if (context) {
            const XeSS::Api* const api = XeSS::GetApi();
            const xess_context_handle_t old_context = context;
            scheduler.Record([api, old_context](vk::CommandBuffer) {
                api->destroy_context(old_context);
            });
            scheduler.Finish();
            context = nullptr;
        }
        intermediate_view = {};
        intermediate_image = {};
        intermediate_initialized.store(false, std::memory_order_relaxed);
        ready.store(false, std::memory_order_release);
        initialization_scheduled.store(false, std::memory_order_release);
        first_execution = true;
        rejected_input_size = {};
        rejected_output_size = {};
        xess_output_size = {};
    }

    void UpdateFallbackConfiguration(VkExtent2D input, VkExtent2D output, bool aa,
                                     bool intermediate) {
        std::scoped_lock lock{fallback_mutex};
        if (fallback_input.width != input.width || fallback_input.height != input.height ||
            fallback_output.width != output.width || fallback_output.height != output.height ||
            fallback_aa != aa || fallback_intermediate != intermediate) {
            fallback_input = input;
            fallback_output = output;
            fallback_aa = aa;
            fallback_intermediate = intermediate;
            fallback_logged = false;
        }
    }

    void LogFallback(VkExtent2D input, VkExtent2D output, bool aa, bool intermediate,
                     VkExtent2D xess_output, xess_2d_t optimal, xess_2d_t minimum,
                     xess_2d_t maximum, std::string_view reason) {
        std::scoped_lock lock{fallback_mutex};
        if (fallback_logged || fallback_input.width != input.width ||
            fallback_input.height != input.height || fallback_output.width != output.width ||
            fallback_output.height != output.height || fallback_aa != aa ||
            fallback_intermediate != intermediate) {
            return;
        }
        fallback_logged = true;
        LOG_INFO(Render_Vulkan,
                 "XeSS-SR fallback for Ocarina input {}x{}, window output {}x{}, XeSS output "
                 "{}x{} (optimal {}x{}, min {}x{}, max {}x{}; {}); using the selected fallback",
                 input.width, input.height, output.width, output.height, xess_output.width,
                 xess_output.height, optimal.x, optimal.y, minimum.x, minimum.y, maximum.x,
                 maximum.y, reason);
    }

    void Initialize(VkExtent2D input_size, VkExtent2D output_extent, bool aa,
                    bool use_intermediate) {
        const XeSS::Api* const api = XeSS::GetApi();
        if (!api || (!context && api->create_context(
                 device.GetInstance(), static_cast<VkPhysicalDevice>(device.GetPhysical()),
                 *device.GetLogical(), &context) != XESS_RESULT_SUCCESS)) {
            Fail("could not create the Vulkan context");
            return;
        }
        api->set_logging_callback(context, XESS_LOGGING_LEVEL_WARNING, LogXeSS);
        const xess_quality_settings_t sdk_quality =
            aa ? XESS_QUALITY_SETTING_AA : ToXeSSRatioQuality(input_size, output_extent);
        VkExtent2D xess_extent = output_extent;
        if (aa) {
            xess_extent = input_size;
        } else if (use_intermediate) {
            const xess_2d_t window_resolution{output_extent.width, output_extent.height};
            xess_2d_t window_optimal{};
            xess_2d_t ignored_minimum{};
            xess_2d_t ignored_maximum{};
            if (api->get_optimal_input_resolution(context, &window_resolution, sdk_quality,
                                                  &window_optimal, &ignored_minimum,
                                                  &ignored_maximum) != XESS_RESULT_SUCCESS ||
                window_optimal.x == 0 || window_optimal.y == 0) {
                Fail("could not determine the selected quality mode's scale ratio");
                return;
            }
            const double scale =
                (static_cast<double>(output_extent.width) / window_optimal.x +
                 static_cast<double>(output_extent.height) / window_optimal.y) * 0.5;
            xess_extent = ScaleExtentAtQualityRatio(input_size, scale);
            if (xess_extent.width == 0 || xess_extent.height == 0) {
                Fail("could not create an aspect-matched XeSS output extent");
                return;
            }
        }
        const xess_2d_t output_resolution{xess_extent.width, xess_extent.height};
        xess_2d_t optimal{};
        xess_2d_t minimum{};
        xess_2d_t maximum{};
        if (api->get_optimal_input_resolution(context, &output_resolution, sdk_quality, &optimal,
                                              &minimum, &maximum) != XESS_RESULT_SUCCESS) {
            Fail("could not determine the selected quality mode's input range");
            return;
        }
        optimal_input = optimal;
        minimum_input = minimum;
        maximum_input = maximum;
        xess_output_size = xess_extent;
        use_aa = aa;
        intermediate_output = use_intermediate;
        if (!SupportsInput(input_size)) {
            LogFallback(input_size, output_extent, aa, use_intermediate, xess_extent, optimal,
                        minimum, maximum,
                        InputMatchesAspect(input_size)
                            ? "input is outside XeSS's supported range"
                            : "input aspect ratio does not match the XeSS output");
            rejected_input_size = input_size;
            rejected_output_size = output_extent;
            initialization_scheduled.store(false, std::memory_order_release);
            return;
        }
        if (use_intermediate &&
            !device.IsFormatSupported(COLOR_FORMAT,
                VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                    VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
                FormatType::Optimal)) {
            LogFallback(input_size, output_extent, aa, use_intermediate, xess_extent, optimal,
                        minimum, maximum, "linear image blit is unsupported for the XeSS format");
            rejected_input_size = input_size;
            rejected_output_size = output_extent;
            initialization_scheduled.store(false, std::memory_order_release);
            return;
        }
        if (use_intermediate && !intermediate_image) {
            try {
                CreateIntermediate(xess_extent);
            } catch (const vk::Exception&) {
                intermediate_view = {};
                intermediate_image = {};
                LogFallback(input_size, output_extent, aa, use_intermediate, xess_extent, optimal,
                            minimum, maximum, "could not allocate the XeSS intermediate image");
                rejected_input_size = input_size;
                rejected_output_size = output_extent;
                initialization_scheduled.store(false, std::memory_order_release);
                return;
            }
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
        configured_input_size = input_size;
        configured_output_size = output_extent;
        configured_aa = aa;
        configured_intermediate = use_intermediate;
        ready.store(true, std::memory_order_release);
        LOG_INFO(Render_Vulkan,
             "XeSS-SR initialized for Ocarina input {}x{}, XeSS output {}x{}, window {}x{} "
             "quality={}", input_size.width, input_size.height, xess_extent.width,
             xess_extent.height, output_extent.width, output_extent.height,
             static_cast<u32>(sdk_quality));
    }

    void Execute(const Inputs& inputs) {
        const XeSS::Api* const api = XeSS::GetApi();
        if (!api || !context || failed.load(std::memory_order_acquire)) {
            return;
        }
        const bool reset_history =
            history_reset_pending.exchange(false, std::memory_order_acq_rel);
        const VkImage target_image = intermediate_output ? *intermediate_image : *output_image;
        const VkImageView target_view = intermediate_output ? *intermediate_view : *output_view;
        const bool target_initialized = intermediate_output
            ? intermediate_initialized.load(std::memory_order_relaxed)
            : output_initialized.load(std::memory_order_relaxed);
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
            Barrier(target_image, COLOR_RANGE,
                    target_initialized ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT : 0,
                    VK_ACCESS_SHADER_WRITE_BIT,
                    target_initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_GENERAL),
        };
        scheduler.Record([this, api, inputs, to_read, reset_history, target_image,
                  target_view](vk::CommandBuffer cmdbuf) {
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
            params.outputTexture = ImageInfo(target_image, target_view, COLOR_FORMAT,
                                             xess_output_size, VK_IMAGE_ASPECT_COLOR_BIT);
            params.jitterOffsetX = inputs.jitter[0];
            params.jitterOffsetY = inputs.jitter[1];
            params.exposureScale = 1.0f;
            const bool input_size_changed = last_input_size.width != 0 &&
                (last_input_size.width != color_size.width || last_input_size.height != color_size.height);
            params.resetHistory = inputs.reset || first_execution || reset_history || input_size_changed;
            params.inputWidth = color_size.width;
            params.inputHeight = color_size.height;
            const xess_result_t result = api->execute(context, *cmdbuf, &params);
            const bool succeeded = result >= 0;
            if (result < 0) {
                if (inputs.execution_failed) {
                    inputs.execution_failed->store(true, std::memory_order_release);
                }
                LogFallback(inputs.render_size, inputs.output_size, use_aa, intermediate_output,
                            xess_output_size, optimal_input, minimum_input, maximum_input,
                            "XeSS frame evaluation failed");
                failed.store(true, std::memory_order_release);
            } else {
                first_execution = false;
                last_input_size = color_size;
                if (intermediate_output) {
                    intermediate_initialized.store(true, std::memory_order_relaxed);
                } else {
                    output_initialized.store(true, std::memory_order_relaxed);
                }
            }
            const std::array<VkImageMemoryBarrier, 3> input_back{
                Barrier(inputs.color_image, Range(VK_IMAGE_ASPECT_COLOR_BIT),
                        VK_ACCESS_SHADER_READ_BIT, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL),
                Barrier(inputs.motion_image, Range(VK_IMAGE_ASPECT_COLOR_BIT),
                        VK_ACCESS_SHADER_READ_BIT, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL),
                Barrier(inputs.depth_image, Range(VK_IMAGE_ASPECT_DEPTH_BIT),
                        VK_ACCESS_SHADER_READ_BIT, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL),
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, {}, {}, input_back);
            if (!intermediate_output) {
                const VkImageMemoryBarrier output_back = Barrier(
                    target_image, COLOR_RANGE, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, {}, {}, output_back);
                return;
            }
            const VkImageMemoryBarrier intermediate_to_blit = Barrier(
                target_image, COLOR_RANGE, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            const VkImageMemoryBarrier output_to_blit = Barrier(
                *output_image, COLOR_RANGE,
                output_initialized.load(std::memory_order_relaxed)
                    ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT
                    : 0,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                output_initialized.load(std::memory_order_relaxed)
                    ? VK_IMAGE_LAYOUT_GENERAL
                    : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            const std::array to_blit{intermediate_to_blit, output_to_blit};
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, {}, {}, to_blit);
            if (succeeded) {
                const VkImageBlit region{
                    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                    .srcOffsets = {{0, 0, 0},
                                   {static_cast<s32>(xess_output_size.width),
                                    static_cast<s32>(xess_output_size.height), 1}},
                    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                    .dstOffsets = {{0, 0, 0},
                                   {static_cast<s32>(output_size.width),
                                    static_cast<s32>(output_size.height), 1}},
                };
                 const std::array regions{region};
                cmdbuf.BlitImage(target_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, *output_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, regions, VK_FILTER_LINEAR);
                output_initialized.store(true, std::memory_order_relaxed);
            }
            const std::array<VkImageMemoryBarrier, 2> after_blit{
                Barrier(target_image, COLOR_RANGE, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL),
                Barrier(*output_image, COLOR_RANGE, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, {}, {}, after_blit);
        });
    }

    bool SupportsInput(VkExtent2D input_size) const {
        return InputMatchesAspect(input_size) &&
               input_size.width >= minimum_input.x && input_size.height >= minimum_input.y &&
               input_size.width <= maximum_input.x && input_size.height <= maximum_input.y;
    }

    bool InputMatchesAspect(VkExtent2D input_size) const {
        const u64 input_cross = static_cast<u64>(input_size.width) * xess_output_size.height;
        const u64 output_cross = static_cast<u64>(input_size.height) * xess_output_size.width;
        const u64 difference = input_cross > output_cross ? input_cross - output_cross
                                                          : output_cross - input_cross;
        if (!intermediate_output || use_aa) {
            return difference == 0;
        }
        return difference <= std::max(input_size.width, input_size.height);
    }

    bool ConfigurationMatches(VkExtent2D input, VkExtent2D output, bool aa,
                              bool intermediate) const {
        return configured_output_size.width == output.width &&
               configured_output_size.height == output.height && configured_aa == aa &&
               configured_intermediate == intermediate &&
               (!intermediate || (configured_input_size.width == input.width &&
                                  configured_input_size.height == input.height));
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
    vk::Image intermediate_image;
    vk::ImageView intermediate_view;
    VkExtent2D output_size{};
    std::atomic<bool> ready{};
    std::atomic<bool> initialization_scheduled{};
    std::atomic<bool> failed{};
#ifdef _WIN32
    xess_context_handle_t context{};
    VkExtent2D xess_output_size{};
    VkExtent2D configured_input_size{};
    VkExtent2D configured_output_size{};
    VkExtent2D last_input_size{};
    xess_2d_t optimal_input{};
    xess_2d_t minimum_input{};
    xess_2d_t maximum_input{};
    VkExtent2D rejected_input_size{};
    VkExtent2D rejected_output_size{};
    bool intermediate_output{};
    bool use_aa{};
    bool configured_aa{};
    bool configured_intermediate{};
    std::mutex fallback_mutex;
    VkExtent2D fallback_input{};
    VkExtent2D fallback_output{};
    bool fallback_aa{};
    bool fallback_intermediate{};
    bool fallback_logged{};
    bool first_execution{true};
    std::atomic<bool> history_reset_pending{};
    std::atomic<bool> output_initialized{};
    std::atomic<bool> intermediate_initialized{};
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
    const bool input_larger = inputs.render_size.width > inputs.output_size.width ||
                              inputs.render_size.height > inputs.output_size.height;
    const bool use_aa = inputs.render_size.width == inputs.output_size.width &&
                        inputs.render_size.height == inputs.output_size.height;
    const bool aspect_matches =
        static_cast<u64>(inputs.render_size.width) * inputs.output_size.height ==
        static_cast<u64>(inputs.render_size.height) * inputs.output_size.width;
    const bool use_intermediate = input_larger || !aspect_matches;
    state.UpdateFallbackConfiguration(inputs.render_size, inputs.output_size, use_aa,
                                     use_intermediate);
    if (state.output_size.width != inputs.output_size.width ||
        state.output_size.height != inputs.output_size.height) {
        if (state.output_size.width != 0 || state.context) {
            state.DestroyContext();
        }
        state.CreateOutput(inputs.output_size);
    }
    if (!state.output_image) {
        state.CreateOutput(inputs.output_size);
    }
    bool ready = state.ready.load(std::memory_order_acquire);
    if (ready && !state.ConfigurationMatches(inputs.render_size, inputs.output_size,
                                             use_aa, use_intermediate)) {
        state.DestroyContext();
        ready = false;
    }
    if (ready && !state.SupportsInput(inputs.render_size)) {
        state.history_reset_pending.store(true, std::memory_order_release);
        state.LogFallback(inputs.render_size, inputs.output_size, use_aa, use_intermediate,
                          state.xess_output_size, state.optimal_input, state.minimum_input,
                          state.maximum_input,
                          state.InputMatchesAspect(inputs.render_size)
                              ? "input is outside XeSS's supported range"
                              : "input aspect ratio does not match the XeSS output");
        return false;
    }
    if (!ready) {
        const bool same_rejected_dimensions =
            !state.initialization_scheduled.load(std::memory_order_acquire) &&
            state.rejected_input_size.width == inputs.render_size.width &&
            state.rejected_input_size.height == inputs.render_size.height &&
            state.rejected_output_size.width == inputs.output_size.width &&
            state.rejected_output_size.height == inputs.output_size.height;
        if (same_rejected_dimensions) {
            return false;
        }
        if (!state.initialization_scheduled.exchange(true, std::memory_order_acq_rel)) {
            state.scheduler.Record([&state, input_size = inputs.render_size,
                                    output_size = inputs.output_size, use_aa,
                                    use_intermediate](vk::CommandBuffer) {
                state.Initialize(input_size, output_size, use_aa, use_intermediate);
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