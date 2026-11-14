// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Optional hardware test, deliberately excluded from ctest. Does not boot the emulator.
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "video_core/neural_rendering/runtime.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"

#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"
#include "video_core/renderer_vulkan/vk_nvidia_dlss.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/ngx_dlss.h"
#include "video_core/vulkan_common/ngx_api.h"
#include "video_core/vulkan_common/nvidia_dlss.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace {
using namespace Vulkan;

/// Fail the hardware smoke test with a diagnostic when an invariant is violated.
void Require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct InputImage {
    vk::Image image;
    vk::ImageView view;
    NvidiaDlssUpscaler::Image resource;
};

InputImage MakeInput(const Device &device, MemoryAllocator &allocator, VkFormat format,
                     VkImageAspectFlags aspect, VkExtent2D extent) {
    InputImage result;
    result.image = allocator.CreateImage({
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    const VkImageSubresourceRange range{aspect, 0, 1, 0, 1};
    result.view = device.GetLogical().CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *result.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = range,
    });
    result.resource = {*result.view, *result.image, format, range, aspect};
    return result;
}

/// Check live neural controls, quality, history resets, rebinding and failures with concurrent submits.
void RenderCases(const Device& device, u32 scale, bool request_neural, bool expect_neural,
                 bool fail_creation, bool padded_depth) {
    MemoryAllocator allocator{device};
    StateTracker tracker;
    Scheduler scheduler{device, tracker};
    // Simulate presentation submissions during backend setup, resizing and teardown. Declaring
    // this before the upscaler keeps it running through upscaler destruction; use Vulkan validation
    // to detect queue calls that are not protected by the scheduler's submission lock.
    std::jthread present_thread{[&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            {
                std::scoped_lock queue_lock{scheduler.submit_mutex};
                Require(device.GetGraphicsQueue().Submit(
                            VkSubmitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO}) == VK_SUCCESS,
                        "Concurrent queue submission failed");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }};
    NvidiaDlssUpscaler upscaler{device, scheduler, allocator};
    const VkExtent2D input_extent{320 * scale, 180 * scale};
    auto color = MakeInput(device, allocator, VK_FORMAT_R16G16B16A16_SFLOAT,
                           VK_IMAGE_ASPECT_COLOR_BIT, input_extent);
    const VkExtent2D depth_extent =
        padded_depth ? VkExtent2D{input_extent.width + 32, input_extent.height + 16} : input_extent;
    auto depth = MakeInput(device, allocator, VK_FORMAT_D32_SFLOAT,
                           VK_IMAGE_ASPECT_DEPTH_BIT, depth_extent);
    auto motion = MakeInput(device, allocator, VK_FORMAT_R16G16_SFLOAT,
                            VK_IMAGE_ASPECT_COLOR_BIT, input_extent);
    auto alternate_motion_view = device.GetLogical().CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *motion.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = motion.resource.format,
        .subresourceRange = motion.resource.range,
    });
    const auto original_motion_view = motion.resource.view;
    auto &api = const_cast<Ngx::Api &>(Ngx::GetApi());
    const auto create = api.create;
    const auto evaluate = api.evaluate;
    static decltype(api.evaluate) forward_evaluate;
    static VkExtent2D observed_depth_extent;
    static VkExtent2D observed_render_extent;
    static VkOffset2D observed_depth_base;
    forward_evaluate = evaluate;
    const auto observe_evaluate = [](VkCommandBuffer command, const NVSDK_NGX_Handle* feature,
                                     const NVSDK_NGX_Parameter* parameters,
                                     PFN_NVSDK_NGX_ProgressCallback progress) {
        void* resource{};
        parameters->Get(NVSDK_NGX_Parameter_Depth, &resource);
        if (resource) {
            const auto& depth =
                static_cast<const NVSDK_NGX_Resource_VK*>(resource)->Resource.ImageViewInfo;
            observed_depth_extent = {depth.Width, depth.Height};
        }
        parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
                        &observed_render_extent.width);
        parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
                        &observed_render_extent.height);
        unsigned base_x = ~0u, base_y = ~0u;
        parameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, &base_x);
        parameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, &base_y);
        observed_depth_base = {static_cast<int>(base_x), static_cast<int>(base_y)};
        return forward_evaluate(command, feature, parameters, progress);
    };
    SCOPE_EXIT {
        api.create = create;
        api.evaluate = evaluate;
    };
    if (fail_creation) {
        api.create = [](VkDevice, VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                        NVSDK_NGX_Handle** handle) {
            *handle = nullptr;
            return NVSDK_NGX_Result_FAIL_PlatformError;
        };
    }
    using Quality = Settings::NvidiaDlssQuality;
    using Neural = Settings::NeuralRendering;
    constexpr std::array cases{
        // Match Ocarina's cold start; scale 8 exercises Native/M/Natural at 2560x1440.
        Settings::NvidiaDlssRuntimeSettings{Quality::Native, Neural::Natural, 100, 100, 100,
                                          true, Settings::NvidiaDlssPreset::M},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Standard},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Cinematic},
        Settings::NvidiaDlssRuntimeSettings{Quality::Native, Neural::Cinematic},
        Settings::NvidiaDlssRuntimeSettings{Quality::Native, Neural::Off},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Off},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural, 50},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural, 0},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural, 0},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural, 100, 0},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural, 100, 100, 0},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural},
        Settings::NvidiaDlssRuntimeSettings{Quality::Quality, Neural::Off, 100, 100, 100, true, Settings::NvidiaDlssPreset::K},
        Settings::NvidiaDlssRuntimeSettings{Quality::Balanced, Neural::Off, 100, 100, 100, true, Settings::NvidiaDlssPreset::M},
        Settings::NvidiaDlssRuntimeSettings{Quality::Performance, Neural::Off, 100, 100, 100, true, Settings::NvidiaDlssPreset::L},
        Settings::NvidiaDlssRuntimeSettings{Quality::UltraPerformance, Neural::Off, 100, 100, 100, true, Settings::NvidiaDlssPreset::J},
        Settings::NvidiaDlssRuntimeSettings{Quality::Native, Neural::Off, 100, 100, 100, true, Settings::NvidiaDlssPreset::NvidiaDefault},
        Settings::NvidiaDlssRuntimeSettings{Quality::Automatic, Neural::Natural}};
    u64 neural_frames{};
    std::array<double, 3> natural_reset{};
    for (size_t frame = 0; frame < cases.size(); ++frame) {
        Settings::values.nvidia_dlss_quality = cases[frame].quality;
        Settings::values.nvidia_dlss_preset = cases[frame].preset;
        Settings::values.neural_rendering = request_neural ? cases[frame].neural_rendering : Neural::Off;
        Settings::values.neural_rendering_strength = cases[frame].strength;
        Settings::values.neural_rendering_tone = cases[frame].tone;
        Settings::values.neural_rendering_structure = cases[frame].structure;
        Settings::RefreshNvidiaDlssRuntimeSettings();
        const auto options = Settings::GetNvidiaDlssRuntimeSettings();
        const bool neural_on = expect_neural && options.neural_rendering != Neural::Off;
        // Rebind while temporal history is live, without resizing the backend.
        motion.resource.view = frame == 1 ? *alternate_motion_view : original_motion_view;
        const VkExtent2D extent = options.quality == Quality::Native
                                      ? input_extent
                                      : VkExtent2D{480 * scale, 270 * scale};
        const bool inject_failure = frame == cases.size() - 1;
        observed_depth_extent = {};
        observed_render_extent = {};
        observed_depth_base = {-1, -1};
        api.evaluate = observe_evaluate;
        if (inject_failure) {
            api.evaluate = [](VkCommandBuffer, const NVSDK_NGX_Handle *,
                              const NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback) {
                return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
            };
        }
        const bool hdr = frame == 5 || frame == 10 || frame == 14;
        const std::array<float, 4> expected = frame == 14 ? std::array{2.0f, 0.25f, 0.5f, 1.0f}
                                            : hdr ? std::array{0.75f, 0.25f, 0.5f, 1.0f}
                                                  : std::array{0.25f, 0.5f, 0.75f, 1.0f};
        scheduler.Record([&, frame, expected](vk::CommandBuffer cmd) {
            for (const auto &image : {color.resource, depth.resource, motion.resource}) {
                cmd.PipelineBarrier(
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                    VkImageMemoryBarrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .srcAccessMask = frame == 0 ? 0u : VK_ACCESS_MEMORY_READ_BIT,
                        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                        .oldLayout =
                            frame == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = image.image,
                        .subresourceRange = image.range,
                    });
            }
            VkClearColorValue clear{};
            std::copy(expected.begin(), expected.end(), clear.float32);
            cmd.ClearColorImage(*color.image, VK_IMAGE_LAYOUT_GENERAL, clear, color.resource.range);
            cmd.ClearColorImage(*motion.image, VK_IMAGE_LAYOUT_GENERAL, {}, motion.resource.range);
            const VkClearDepthStencilValue clear_depth{0.5f, 0};
            const auto clear_fn = reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(
                device.GetDispatchLoader().vkGetDeviceProcAddr(*device.GetLogical(),
                                                               "vkCmdClearDepthStencilImage"));
            clear_fn(*cmd, *depth.image, VK_IMAGE_LAYOUT_GENERAL, &clear_depth, 1,
                     &depth.resource.range);
        });
        const auto before_status = NvidiaDlssUpscaler::Status();
        Require(upscaler.Upscale({color.resource,
                                  depth.resource,
                                  motion.resource,
                                  input_extent,
                                  extent,
                                  {0, 0},
                                  {1, 1},
                                  frame == 0,
                                  frame != 0 && !hdr,
                                  hdr,
                                  padded_depth ? depth_extent : VkExtent2D{}}),
                "Upscale refused a frame");
        auto readback = allocator.CreateBuffer(
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = VkDeviceSize{extent.width} * extent.height * 8,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            },
            MemoryUsage::Download);
        scheduler.Record([&, extent](vk::CommandBuffer cmd) {
            cmd.PipelineBarrier(
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT});
            cmd.CopyImageToBuffer(
                upscaler.OutputImage(), VK_IMAGE_LAYOUT_GENERAL, *readback,
                VkBufferImageCopy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {extent.width, extent.height, 1}});
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                                VK_ACCESS_HOST_READ_BIT});
        });
        scheduler.Finish();
        api.evaluate = evaluate;
        if (frame == 0 && request_neural && !expect_neural && !fail_creation) {
            Require(!VideoCore::NeuralRendering::GetStatus().error.empty(),
                    "Missing neural model was not diagnosed");
        }
        if (fail_creation) {
            Require(upscaler.Failed() && !Ngx::GetStatus().error.empty(),
                    "Initial feature creation failure was not diagnosed");
            Require(NvidiaDlssUpscaler::Status().evaluations == before_status.evaluations,
                    "Failed creation counted a DLSS evaluation");
            const auto neural = VideoCore::NeuralRendering::GetStatus();
            Require(!neural.ready && neural.submitted_frames == 0,
                    "Neural rendering started despite failed DLSS creation");
            Require(!upscaler.Upscale({}), "Failed creation did not request fallback");
            std::printf("PASS cold-start creation failure: no neural initialization\n");
            return;
        }
        Require(upscaler.Failed() == inject_failure, "Unexpected NGX evaluation result");
        const auto after_status = NvidiaDlssUpscaler::Status();
        Require(after_status.evaluations == before_status.evaluations + !inject_failure,
                "DLSS status counted an unsuccessful evaluation");
        if (!inject_failure) {
            Require(observed_depth_extent.width == depth_extent.width &&
                        observed_depth_extent.height == depth_extent.height,
                    "NGX received an incorrect depth image extent");
            Require(observed_render_extent.width == input_extent.width &&
                        observed_render_extent.height == input_extent.height &&
                        observed_depth_base.x == 0 && observed_depth_base.y == 0,
                    "NGX lost the top-left render subrect in the depth image");
            Require(after_status.requested == options.quality && after_status.preset == options.preset,
                    "DLSS status lost runtime quality or preset");
            Require(after_status.mode != Quality::Automatic, "No evaluated quality reported");
        }
        if (inject_failure) {
            Require(!Ngx::GetStatus().error.empty(), "Evaluation failure was not diagnosed");
            Require(!upscaler.Upscale({}), "Failed upscaler did not request fallback");
        }
        readback.Invalidate();
        const auto bytes = readback.Mapped();
        std::array<double, 3> sums{};
        const size_t pixels = size_t{extent.width} * extent.height;
        for (size_t i = 0; i < pixels; ++i) {
            for (size_t c = 0; c < sums.size(); ++c) {
                _Float16 half;
                std::memcpy(&half, bytes.data() + (i * 4 + c) * 2, sizeof(half));
                const float value = static_cast<float>(half);
                Require(std::isfinite(value), "Non-finite output");
                sums[c] += value;
            }
        }
        double style_difference{};
        for (size_t c = 0; c < sums.size(); ++c) {
            if (!neural_on || options.strength == 0 || inject_failure) {
                Require(std::abs(sums[c] / pixels - (inject_failure ? 0.0f : expected[c])) < 0.025,
                        "Readback differs from input; stale or incorrect output");
            } else {
                Require(sums[c] / pixels > 0.01 && sums[c] / pixels < 16.0,
                        "Neural output is empty or outside the expected HDR range");
            }
            if (expect_neural && frame == 2) {
                natural_reset[c] = sums[c] / pixels;
            }
            if (expect_neural && (frame == 6 || frame == 9 || frame == 16 || frame == 18)) {
                Require(std::abs(sums[c] / pixels - natural_reset[c]) < 0.001,
                        "Neural adjustment retained old history or differs from a fresh activation");
            }
            if (expect_neural && frame == 12) {
                const auto halfway = (expected[c] + natural_reset[c]) / 2;
                Require(std::abs(sums[c] / pixels - halfway) < 0.003,
                        "50% strength did not blend the original and neural images");
            }
            style_difference += std::abs(sums[c] / pixels - natural_reset[c]);
        }
        if (expect_neural && frame == 3) {
            Require(style_difference > 0.01, "Switching Natural to Cinematic did not change output");
        }
        if (expect_neural && (frame == 15 || frame == 17)) {
            Require(style_difference > 0.00001, "Tone/structure control did not change output");
        }
        neural_frames += neural_on && !inject_failure;
        const auto neural = VideoCore::NeuralRendering::GetStatus();
        Require(neural.submitted_frames == neural_frames, "Incorrect neural rendering submission count");
        Require(neural.ready == (neural_on && !inject_failure),
                "Neural readiness did not follow the live setting and DLSS result");
        std::printf("PASS frame %zu: %ux%u -> %ux%u; strength/tone/structure %u/%u/%u; "
                    "RGB %.4f %.4f %.4f\n", frame,
                    input_extent.width, input_extent.height, extent.width, extent.height,
                    options.strength, options.tone, options.structure,
                    sums[0] / pixels, sums[1] / pixels, sums[2] / pixels);
    }
    Require(Ngx::GetStatus().evaluations == cases.size() - 1, "Incorrect evaluation count");
}
} // namespace

/// Exercise NGX startup, model fallback, and repeated device lifecycles on the selected GPU.
int main(int argc, char **argv) try {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::fprintf(stderr, "Usage: ngx_smoke <Linux DLSS directory> [GPU index] [scale 1-8] [NR "
                             "model directory or missing]\n");
        return 2;
    }
    const auto scratch =
        std::filesystem::temp_directory_path() / ("lichii-ngx-smoke-" + std::to_string(getpid()));
    std::filesystem::create_directories(scratch / "log");
    std::filesystem::create_directories(scratch / "cache");
    Common::FS::SetEdenPath(Common::FS::EdenPath::EdenDir, scratch);
    Common::FS::SetEdenPath(Common::FS::EdenPath::CacheDir, scratch / "cache");
    Common::FS::SetEdenPath(Common::FS::EdenPath::LogDir, scratch / "log");
    Common::Log::Initialize();
    Common::Log::Start();
    Settings::values.nvidia_dlss = false;
    setenv("LICHII_NGX_DIR", scratch.c_str(), 1);
    Require(!NvidiaDlss::Start() && !Ngx::GetStatus().error.empty(), "Missing model not diagnosed");
    setenv("LICHII_NGX_DIR", std::filesystem::absolute(argv[1]).c_str(), 1);
    const size_t gpu = argc > 2 ? std::stoul(argv[2]) : 0;
    const auto scale = argc > 3 ? std::stoul(argv[3]) : 1;
    const bool request_neural = argc > 4;
    const bool expect_neural = request_neural && std::string_view{argv[4]} != "missing";
    if (expect_neural) {
        const auto target = VideoCore::NeuralRendering::UserFolder();
        std::filesystem::create_directories(target.parent_path());
        std::filesystem::create_directory_symlink(std::filesystem::absolute(argv[4]), target);
    }
    Require(scale >= 1 && scale <= 8, "Scale must be between 1 and 8 (4K output)");
    for (int run = 0; run < 3; ++run) {
        const bool fail_creation = run == 2;
        // Both DLSS and neural rendering may start off, then be enabled without a new device.
        Settings::values.nvidia_dlss = run != 0;
        Settings::values.neural_rendering = Settings::NeuralRendering::Off;
        Settings::RefreshNvidiaDlssRuntimeSettings();
        Require(NvidiaDlss::Start(), "NGX startup failed");
        {
            auto library = OpenLibrary();
            vk::InstanceDispatch dispatch;
            auto instance = CreateInstance(*library, dispatch, VK_API_VERSION_1_3,
                                           Core::Frontend::WindowSystemType::X11);
            const auto physical_devices = instance.EnumeratePhysicalDevices();
            Require(gpu < physical_devices.size(), "GPU index out of range");
            Device device{*instance, vk::PhysicalDevice{physical_devices[gpu], dispatch},
                          VK_NULL_HANDLE, dispatch};
            Require(NvidiaDlss::IsAvailable(), "Native DLSS unavailable on selected device");
            Require(!request_neural || device.SupportsNeuralRendering(),
                    "Neural rendering device features were not enabled");
            Require(!NvidiaDlss::WantsDevice(), "Attached runtime still requests a device");
            auto duplicate_dispatch = device.GetDispatchLoader();
            Require(!NvidiaDlss::AttachDevice(*instance, physical_devices[gpu],
                                              *device.GetLogical(), device.GetGraphicsFamily(), 0,
                                              duplicate_dispatch),
                    "NGX accepted a second device attachment");
            Require(NvidiaDlss::IsAvailable(), "Duplicate attachment disturbed the active device");
            Settings::values.nvidia_dlss = true;
            Settings::RefreshNvidiaDlssRuntimeSettings();
            RenderCases(device, static_cast<u32>(scale), request_neural, expect_neural,
                        fail_creation, run == 1);
        }
        Require(!NvidiaDlss::IsAvailable(), "Device teardown left NGX active");
        std::printf("PASS device lifecycle %d\n", run + 1);
    }
    NvidiaDlss::Stop();
    std::printf("Logs/cache: %s\n", scratch.c_str());
    return 0;
} catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
