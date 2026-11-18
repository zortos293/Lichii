// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fmt/ranges.h>

#include "common/display_hdr.h"
#include "common/fs/fs.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include <ranges>
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core_timing.h"
#include "core/frontend/graphics_context.h"
#include "core/nvn_hook.h"
#include "video_core/capture.h"
#include "video_core/gpu.h"
#include "video_core/shader_notify.h"
#include "video_core/nx2_profile.h"
#include "video_core/present.h"
#include "video_core/render_trace.h"
#include "video_core/renderer_vulkan/present/util.h"
#ifdef HAS_RESHADE
#include "video_core/post_processing/fx_chain.h"
#endif
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/textures/decoders.h"
#include "video_core/vulkan_common/vk_enum_string_helper.h"
#include "video_core/vulkan_common/vulkan_address_map.h"
#include "video_core/vulkan_common/vulkan_debug_callback.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_surface.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#ifdef EGLANTINE_STREAMLINE
#include "video_core/renderer_vulkan/vk_streamline_dlss.h"
#include "video_core/vulkan_common/streamline.h"
#include "video_core/vulkan_common/streamline_frame_gen.h"
#endif
#ifdef __ANDROID__
#include <jni.h>
#endif
#ifdef LICHII_NVN_DIRECT
#include "video_core/nvn_direct/nvn_direct.h"
#include <chrono>
#include <mutex>
#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
#endif
#endif
namespace Vulkan {
namespace {

constexpr VkExtent2D CaptureImageSize{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
};

#ifdef HAS_LSFG
[[nodiscard]] VkExtent2D GuestExtent(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (framebuffers.empty()) {
        return VkExtent2D{};
    }

    const auto& framebuffer = framebuffers.front();
    if (framebuffer.crop_rect.IsEmpty()) {
        return VkExtent2D{.width = framebuffer.width, .height = framebuffer.height};
    }
    return VkExtent2D{
        .width = static_cast<u32>(framebuffer.crop_rect.GetWidth()),
        .height = static_cast<u32>(framebuffer.crop_rect.GetHeight()),
    };
}
#endif

constexpr VkExtent3D CaptureImageExtent{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
    .depth = VideoCore::Capture::LinearDepth,
};

constexpr VkFormat CaptureFormat = VK_FORMAT_A8B8G8R8_UNORM_PACK32;

std::string GetReadableVersion(u32 version) {
    return fmt::format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                       VK_VERSION_PATCH(version));
}

std::string GetDriverVersion(const Device& device) {
    // Extracted from
    // https://github.com/SaschaWillems/vulkan.gpuinfo.org/blob/5dddea46ea1120b0df14eef8f15ff8e318e35462/functions.php#L308-L314
    const u32 version = device.GetDriverVersion();

    if (device.GetDriverID() == VK_DRIVER_ID_NVIDIA_PROPRIETARY) {
        const u32 major = (version >> 22) & 0x3ff;
        const u32 minor = (version >> 14) & 0x0ff;
        const u32 secondary = (version >> 6) & 0x0ff;
        const u32 tertiary = version & 0x003f;
        return fmt::format("{}.{}.{}.{}", major, minor, secondary, tertiary);
    }
    if (device.GetDriverID() == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS) {
        const u32 major = version >> 14;
        const u32 minor = version & 0x3fff;
        return fmt::format("{}.{}", major, minor);
    }
    return GetReadableVersion(version);
}

std::string BuildCommaSeparatedExtensions(
    const std::set<std::string, std::less<>>& available_extensions) {
    return fmt::format("{}", fmt::join(available_extensions, ","));
}

// The swapchain format and colour space to present in: an HDR pair the surface lists while HDR
// output is on (Off/On/Auto with the monitor's state), else SDR.
VkSurfaceFormatKHR ChooseOutputFormat(const vk::PhysicalDevice& physical, VkSurfaceKHR surface,
                                      void* window) {
    auto& monitor = Common::DisplayHdrMonitor::Instance();
    monitor.Watch(window);
    const auto output = Common::ResolveHdrOutput(
        monitor.Current(), Settings::values.hdr_mode.GetValue(),
        Settings::values.hdr_peak_luminance.GetValue(),
        Settings::values.hdr_reference_white.GetValue());
    if (output.enabled) {
        const auto formats = physical.GetSurfaceFormatsKHR(surface);
        const auto hdr =
            ChooseHdrSurfaceFormat(formats, Settings::values.hdr_swapchain.GetValue());
        if (hdr) {
            return *hdr;
        }
        LOG_WARNING(Render_Vulkan,
                    "HDR output is on, but the surface lists no HDR10 or scRGB format");
    }
#ifdef __ANDROID__
    return {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
#else
    return {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
#endif
}

#ifdef LICHII_NVN_DIRECT
#ifdef _WIN32
// Under WDDM NVML has no figure for a process (nvml.h: usedGpuMemory is NVML_VALUE_NOT_AVAILABLE
// there, as Windows' kernel driver manages the memory): the memory budget extension's
class NvmlProcessMemory {
public:
    explicit NvmlProcessMemory(const Device&) {
        LOG_INFO(Render_Vulkan, "NVN2 direct mode: VRAM use from the memory budget extension (NVML "
                                "has no per-process figure under WDDM)");
    }
    u64 Get() {
        return 0;
    }
};
#else
/// NVN2 direct mode: NVML's figure for this process's video memory on the
/// device (NVIDIA only; 0 elsewhere or when NVML has no entry for it). VK_EXT_memory_budget's heap
/// usage leaves out what the driver keeps for itself: Mario Kart World's race showed 8.8 GiB of
/// heap usage while NVML had the process at 12.1 GiB, so a budget on the heap usage
/// never held the cap. Read at most every 250 ms (an NVML process query takes about a millisecond).
class NvmlProcessMemory {
public:
    NvmlProcessMemory(const NvmlProcessMemory&) = delete;
    NvmlProcessMemory& operator=(const NvmlProcessMemory&) = delete;
    ~NvmlProcessMemory() {
        if (nvml_initialized) {
            if (const auto shutdown = reinterpret_cast<int (*)()>(dlsym(library, "nvmlShutdown"))) {
                shutdown();
            }
        }
        if (library != nullptr) {
            dlclose(library);
        }
    }

    explicit NvmlProcessMemory(const Device& device) {
        library = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr) { // (NixOS keeps the driver's libraries here)
            library = dlopen("/run/opengl-driver/lib/libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
        }
        if (library == nullptr) {
            LOG_INFO(Render_Vulkan, "NVN2 direct mode: VRAM use from the memory budget extension (no NVML)");
            return;
        }
        const auto load = [this](const char* name) { return dlsym(library, name); };
        const auto init = reinterpret_cast<int (*)()>(load("nvmlInit_v2"));
        const auto count = reinterpret_cast<int (*)(unsigned*)>(load("nvmlDeviceGetCount_v2"));
        const auto by_index = reinterpret_cast<int (*)(unsigned, void**)>(load("nvmlDeviceGetHandleByIndex_v2"));
        const auto uuid_of = reinterpret_cast<int (*)(void*, char*, unsigned)>(load("nvmlDeviceGetUUID"));
        processes = reinterpret_cast<int (*)(void*, unsigned*, ProcessInfo*)>(
            load("nvmlDeviceGetGraphicsRunningProcesses_v3"));
        unsigned devices = 0;
        nvml_initialized = init && count && by_index && uuid_of && processes && init() == 0;
        if (!nvml_initialized || count(&devices) != 0) {
            processes = nullptr;
            return;
        }
        // The NVML device with the Vulkan device's UUID ("GPU-" and the 16 bytes as 8-4-4-4-12).
        VkPhysicalDeviceIDProperties id{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                               .pNext = &id};
        device.GetPhysical().GetProperties2(properties);
        std::string want = "GPU-";
        for (size_t i = 0; i < VK_UUID_SIZE; ++i) {
            want += fmt::format("{:02x}", id.deviceUUID[i]);
            if (i == 3 || i == 5 || i == 7 || i == 9) {
                want += '-';
            }
        }
        for (unsigned index = 0; index < devices; ++index) {
            void* handle = nullptr;
            std::array<char, 96> uuid{};
            if (by_index(index, &handle) == 0 && uuid_of(handle, uuid.data(), 96) == 0 &&
                want == uuid.data()) {
                nvml_device = handle;
            }
        }
        LOG_INFO(Render_Vulkan, "NVN2 direct mode: VRAM use {} (NVML device {})",
                 nvml_device ? "from NVML for this process" : "from the memory budget extension",
                 nvml_device ? want : "not found");
    }

    /// Bytes, 0 when not known.
    u64 Get() {
        if (nvml_device == nullptr) {
            return 0;
        }
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock{mutex};
        if (now - read_at < std::chrono::milliseconds{250}) {
            return value;
        }
        read_at = now;
        std::array<ProcessInfo, 64> table{};
        unsigned count = static_cast<unsigned>(table.size());
        value = 0;
        if (processes(nvml_device, &count, table.data()) == 0) {
            const unsigned self = static_cast<unsigned>(getpid());
            for (unsigned i = 0; i < count && i < table.size(); ++i) {
                if (table[i].pid == self) {
                    value = table[i].used;
                }
            }
        }
        return value;
    }

private:
    struct ProcessInfo { // nvmlProcessInfo_t (the _v3 calls)
        unsigned pid;
        unsigned long long used;
        unsigned gpu_instance, compute_instance;
    };
    void* library{};
    bool nvml_initialized{};
    void* nvml_device{};
    int (*processes)(void*, unsigned*, ProcessInfo*){};
    std::mutex mutex;
    std::chrono::steady_clock::time_point read_at{};
    u64 value{};
};

#endif

} // Anonymous namespace

/// The device memory figures the replay reads (nvn_direct_device.memory_usage / memory_budget).
struct NvnDirectMemory {
    const Device* device;
    NvmlProcessMemory nvml;
};

namespace {

/// NVN2 direct mode: the replay runs on this device. Its NVN queues take the
/// graphics family's queues from 1 on, its own work (uploads, sparse binds) queue 0 with Lichii; every
/// submission of the replay holds the scheduler's submit lock. Returns whether the replay is up.
bool AdoptNvnDirect(const vk::InstanceDispatch& dld, VkInstance instance, const Device& device,
                    Scheduler& scheduler, NvnDirectAstc* astc, NvnDirectShaders& shaders,
                    NvnDirectMemory& memory, VideoCore::ShaderNotify& shader_notify) {
    std::vector<const char*> extensions;
    for (const std::string& name : device.GetLoadedExtensions()) {
        extensions.push_back(name.c_str());
    }
    const std::span<const VkQueue> queues = device.GetNvnDirectQueues();
    nvn_direct_device adopted{
        .get_instance_proc_addr = dld.vkGetInstanceProcAddr,
        .instance = instance,
        .physical = device.GetPhysical(),
        .device = *device.GetLogical(),
        .queue_family = device.GetGraphicsFamily(),
        .queue_count = static_cast<uint32_t>(queues.size()),
        .queues = {},
        .extensions = extensions.data(),
        .extension_count = static_cast<uint32_t>(extensions.size()),
        .queue_lock = [](void* mutex) { static_cast<std::mutex*>(mutex)->lock(); },
        .queue_unlock = [](void* mutex) { static_cast<std::mutex*>(mutex)->unlock(); },
        .queue_context = &scheduler.submit_mutex,
        .astc_to_bc = astc != nullptr ? &NvnDirectAstc::Record : nullptr,
        .astc_flush = astc != nullptr ? &NvnDirectAstc::Flush : nullptr,
        .astc_context = astc,
        .astc_opaque = astc != nullptr ? &NvnDirectAstc::IsOpaque : nullptr,
        // NVML's figure when there is one (it counts what the driver keeps for itself), else the
        // heap usage.
        .memory_usage = [](void* context) -> uint64_t {
            auto* const memory = static_cast<NvnDirectMemory*>(context);
            const u64 nvml = memory->nvml.Get();
            return nvml != 0 ? nvml : memory->device->GetDeviceMemoryUsage();
        },
        .memory_budget = [](void* context) -> uint64_t {
            return static_cast<NvnDirectMemory*>(context)->device->GetDeviceMemoryBudget();
        },
        .memory_context = &memory,
        .pipeline_building = [](void* notify) {
            static_cast<VideoCore::ShaderNotify*>(notify)->MarkShaderBuilding();
        },
        .pipeline_built = [](void* notify) {
            static_cast<VideoCore::ShaderNotify*>(notify)->MarkShaderComplete();
        },
        .pipeline_context = &shader_notify,
        .async_pipelines = Settings::values.use_asynchronous_shaders.GetValue(),
        .shader_translate = &NvnDirectShaders::Translate,
        .shader_free = &NvnDirectShaders::Free,
        .shader_context = &shaders,
        .scale_up = Settings::values.resolution_info.up_scale,
        .scale_shift = Settings::values.resolution_info.down_shift,
        .debug_labels = device.HasDebuggingToolAttached() || Settings::values.renderer_debug.GetValue(),
    };
    for (size_t index = 0; index < queues.size(); ++index) {
        adopted.queues[index] = queues[(index + 1) % queues.size()];
    }
    // The replay keeps its pipeline cache, prewarm list, kept translations and the view formats
    // it learns in DM_TEMP (Banana Native's cache directory variable): unless it was set when the
    // process started, Lichii's cache directory, one folder per title named as the shader cache's
    // (nvn_direct/<16 hex digits of the program id>/), set again at each boot, never the working
    // directory or HOME, its fallback.
    static const bool temp_given = [] {
        const char* const temp = std::getenv("DM_TEMP");
        return temp != nullptr && *temp != '\0';
    }();
    if (!temp_given) {
        const u64 title = Core::NvnHook::DirectProgramId();
        auto dir = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "nvn_direct";
        if (title != 0) {
            dir /= fmt::format("{:016x}", title);
        }
        if (Common::FS::CreateDirs(dir)) {
#ifdef _WIN32
            _putenv_s("DM_TEMP", Common::FS::PathToUTF8String(dir).c_str()); // (getenv's environment)
#else
            setenv("DM_TEMP", Common::FS::PathToUTF8String(dir).c_str(), 1);
#endif
        }
    }
    const bool up = nvn_direct_adopt_device(&adopted) != 0;
    LOG_INFO(Render_Vulkan, "NVN2 direct mode: the replay {} on Lichii's device ({}, {} queues)",
             up ? "is up" : "failed to start", device.GetModelName(), queues.size());
    return up;
}
#endif

} // Anonymous namespace

Device CreateDevice(const vk::Instance& instance, const vk::InstanceDispatch& dld, VkSurfaceKHR surface) {
    const std::vector<VkPhysicalDevice> devices = instance.EnumeratePhysicalDevices();
    const u32 device_index = Settings::values.vulkan_device.GetValue();
    if (device_index >= u32(devices.size())) {
        LOG_ERROR(Render_Vulkan, "Invalid device index {}!", device_index);
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    const vk::PhysicalDevice physical_device(devices[device_index], dld);
    return Device(*instance, physical_device, surface, dld);
}

RendererVulkan::RendererVulkan(Core::Frontend::EmuWindow& emu_window,
                               Tegra::MaxwellDeviceMemoryManager& device_memory_,
                               Tegra::GPU& gpu_,
                               std::unique_ptr<Core::Frontend::GraphicsContext> context_)
try
    : RendererBase(emu_window, std::move(context_))
    , device_memory(device_memory_)
    , gpu(gpu_)
    , library(OpenLibrary(context.get()))
    , dld()
#ifdef EGLANTINE_STREAMLINE
    , streamline_started(Streamline::Start(render_window.GetWindowInfo().render_surface))
#endif
    // Create raw Vulkan instance first
    , instance(CreateInstance(*library,
                            dld,
                            VK_API_VERSION_1_1,
                            render_window.GetWindowInfo().type,
                            Settings::values.renderer_debug.GetValue()))
    // Create debug messenger if debug is enabled
    , debug_messenger(Settings::values.renderer_debug ? CreateDebugUtilsCallback(instance)
                                                    : vk::DebugUtilsMessenger{})
    , address_messenger(AddressMap::Wanted() ? CreateAddressBindingCallback(instance, dld)
                                             : vk::DebugUtilsMessenger{})
    // Create surface
    , surface(CreateSurface(instance, render_window.GetWindowInfo()))
    , device(CreateDevice(instance, dld, *surface))
    , memory_allocator(device)
    , state_tracker()
    , scheduler(device, state_tracker)
    , swapchain(PresentationSurface(),
                device,
                scheduler,
               render_window.GetFramebufferLayout().width,
               render_window.GetFramebufferLayout().height,
               ChooseOutputFormat(device.GetPhysical(), *surface,
                                  render_window.GetWindowInfo().render_surface))
    , present_manager(instance,
                      render_window,
                      device,
                      memory_allocator,
                      scheduler,
                      swapchain,
                      surface)
    , blit_swapchain(device_memory,
                   device,
                   memory_allocator,
                   present_manager,
                   scheduler,
                   PresentFiltersForDisplay)
    , blit_capture(device_memory,
                   device,
                   memory_allocator,
                   present_manager,
                   scheduler,
                   PresentFiltersForDisplay)
    , blit_applet(device_memory,
                  device,
                  memory_allocator,
                  present_manager,
                  scheduler,
                  PresentFiltersForAppletCapture)
    , rasterizer(render_window, gpu, device_memory, device, memory_allocator, state_tracker, scheduler)
#ifdef HAS_LSFG
    , frame_gen(memory_allocator, scheduler)
#endif
{

    if (Settings::values.renderer_force_max_clock.GetValue() && device.ShouldBoostClocks()) {
        turbo_mode.emplace(instance, dld);
        scheduler.RegisterOnSubmit([this] { turbo_mode->QueueSubmitted(); });
    }

#ifdef HAS_RESHADE
    VideoCore::FxChain::Instance().LoadFromSettings();
#endif

    device_info = {
        .model = std::string{device.GetModelName()},
        .driver = device.GetDriverVersion() != 0
                      ? fmt::format("{} {}", device.GetVendorName(), GetDriverVersion(device))
                      : std::string{},
        .api = "Vulkan",
        .api_version = GetReadableVersion(device.ApiVersion()),
        .device_local_memory = device.GetDeviceLocalMemory(),
        .extensions = {device.GetAvailableExtensions().begin(),
                       device.GetAvailableExtensions().end()},
        .features =
            {
                {"geometry_shader", device.IsGeometryShaderSupported()},
                {"tessellation", device.IsTessellationSupported()},
                {"mesh_shader", device.IsExtMeshShaderSupported()},
                {"descriptor_buffer", device.IsExtDescriptorBufferSupported()},
                {"buffer_device_address", device.IsBufferDeviceAddressSupported()},
                {"push_descriptor", device.IsKhrPushDescriptorSupported()},
                {"vertex_input_dynamic_state", device.IsExtVertexInputDynamicStateSupported()},
                {"extended_dynamic_state", device.IsExtExtendedDynamicStateSupported()},
                {"transform_feedback", device.IsExtTransformFeedbackSupported()},
                {"conservative_rasterization", device.IsExtConservativeRasterizationSupported()},
                {"line_rasterization", device.IsExtLineRasterizationSupported()},
                {"depth_clip_control", device.IsExtDepthClipControlSupported()},
                {"shader_float_controls", device.IsKhrShaderFloatControlsSupported()},
                {"subgroup_size_control", device.IsExtSubgroupSizeControlSupported()},
                {"vulkan_memory_model", device.IsVulkanMemoryModelSupported()},
                {"cooperative_hmma", device.IsCooperativeHmmaSupported()},
                {"sparse_binding", device.IsSparseBindingSupported()},
                {"optimal_astc", device.IsOptimalAstcSupported()},
                {"optimal_bcn", device.IsOptimalBcnSupported()},
                {"formatless_image_load", device.IsFormatlessImageLoadSupported()},
                {"pipeline_binary", device.IsKhrPipelineBinarySupported()},
            },
    };
    Report();
#ifdef LICHII_NVN_DIRECT
    if (device.IsNvnDirect()) {
        // ASTC the device cannot sample: decoded and re-compressed to BC on the GPU (.8).
        if (!device.IsOptimalAstcSupported() && device.IsKhrPushDescriptorSupported()) {
            nvn_direct_astc.emplace(device, memory_allocator);
        }
        const PipelineCache& pipeline_cache = rasterizer.GetPipelineCache();
        nvn_direct_shaders.emplace(device, pipeline_cache.GetProfile(), pipeline_cache.GetHostInfo());
        nvn_direct_memory.reset(new NvnDirectMemory{&device, NvmlProcessMemory{device}});
        if (!AdoptNvnDirect(dld, *instance, device, scheduler,
                            nvn_direct_astc ? &*nvn_direct_astc : nullptr, *nvn_direct_shaders,
                            *nvn_direct_memory, gpu_.ShaderNotify())) {
            // Before the game runs: it falls back to GPU emulation on this device rather than
            // run with nothing to show.
            nvn_direct_memory.reset();
            nvn_direct_shaders.reset();
            nvn_direct_astc.reset();
            device.EndNvnDirect();
            Core::NvnHook::DirectFailed();
        }
    }
#endif
} catch (const vk::Exception& exception) {
    LOG_ERROR(Render_Vulkan, "Vulkan initialization failed with error: {}", exception.what());
    throw std::runtime_error{fmt::format("Vulkan initialization error {}", exception.what())};
}

RendererVulkan::~RendererVulkan() {
    scheduler.RegisterOnSubmit([] {});
#ifdef LICHII_NVN_DIRECT
    // NVN2 direct mode: the replay's threads leave the device (and the ASTC
    // decode it calls back into) before both go away, and its objects go once the device is idle.
    if (device.IsNvnDirect()) {
        nvn_direct_park();
        void(device.GetLogical().WaitIdle());
        nvn_direct_destroy();
    }
#endif
    void(device.GetLogical().WaitIdle());
}

void RendererVulkan::Composite(std::span<const Tegra::FramebufferConfig> framebuffers) {
    const bool trace = Common::DkbFrameTrace::Enabled();
    const auto entered = trace ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
    SCOPE_EXIT {
        render_window.OnFrameDisplayed();
    };
#ifdef EGLANTINE_STREAMLINE
    // DLSS Frame Generation: this guest frame ends here, presented or not.
    Streamline::FrameGenBeginFrame();
    SCOPE_EXIT {
        Streamline::FrameGenNextFrame();
    };
#endif

    RenderAppletCaptureLayer(framebuffers);

    if (!render_window.IsShown()) {
        return;
    }

    if (VideoCommon::StutterTrace::Enabled()) {
        VideoCommon::StutterTrace::OnPresent();
    }
    RenderScreenshot(framebuffers);
    const auto acquiring = trace ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
    Frame* frame = [this] {
        const VideoCommon::Nx2Profile::Scope scope{VideoCommon::Nx2Profile::Zone::Present};
        return present_manager.GetRenderFrame();
    }();
    const auto acquired = trace ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};

    const HdrPresent output = UpdateHdrPresent();
    frame->color_space = output.color_space;

    scheduler.RequestOutsideRenderPassOperationContext();
    blit_swapchain.DrawToFrame(device, rasterizer, frame, framebuffers,
                              render_window.GetFramebufferLayout(),
                              present_manager.SwapchainImageCount(), output_format.format, output);

#ifdef HAS_LSFG
    void(frame_gen.WantedGenerations(present_manager.MaxExtraFrames(),
                                     present_manager.FreeFrames(),
                                     present_manager.TakeBlockedTime()));

    frame_gen.Process(device, frame, frame->image_format, GuestExtent(framebuffers));

    const size_t generated_frames = frame_gen.GeneratedFrameCount();
    // Each generated frame keeps the screen for an equal share of the real frame interval. Sent
    // back to back, a high refresh rate display shows it for one refresh only.
    const auto spacing = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<f32>(std::min(frame_gen.FrameInterval(), 0.1f) /
                                   static_cast<f32>(generated_frames + 1)));
    const u32 game_frame_presents = static_cast<u32>(generated_frames) + 1;
    frame->game_frame_presents = game_frame_presents;
    for (size_t generation = 0; generation < generated_frames; ++generation) {
        Frame* generated = present_manager.GetRenderFrame();
        generated->color_space = output.color_space;
        generated->present_spacing = spacing;
        generated->game_frame_presents = game_frame_presents;
        blit_swapchain.PrepareFrame(device, generated, render_window.GetFramebufferLayout());
        frame_gen.GenerateInto(device, generated, generation);
        scheduler.Flush(*generated->render_ready);
        present_manager.Present(generated);
    }
#endif

    if (const auto mark = scheduler.NextFrameMark(); mark != Scheduler::FrameMark::None) {
        scheduler.MarkFrameEnd(++trace_frame, mark);
    }
    scheduler.Flush(*frame->render_ready);
#ifdef EGLANTINE_STREAMLINE
    {
        const Streamline::FrameGenFrame ended = Streamline::FrameGenEndFrame();
        const Common::Rectangle<u32>& screen = render_window.GetFramebufferLayout().screen;
        frame->frame_gen = Streamline::FrameGenSession();
        frame->frame_gen_frame = ended;
        frame->guest_rect = VkRect2D{
            .offset = {static_cast<s32>(screen.left), static_cast<s32>(screen.top)},
            .extent = {screen.GetWidth(), screen.GetHeight()},
        };
    }
#endif

    const auto presenting = trace ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
    {
        const VideoCommon::Nx2Profile::Scope scope{VideoCommon::Nx2Profile::Zone::Present};
        present_manager.Present(frame);
    }
    const auto presented = trace ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
#ifdef HAS_LSFG
    scheduler.DispatchWork();
#endif

    gpu.RendererFrameEndNotify();
    rasterizer.TickFrame();
    if (trace) {
        const auto end = std::chrono::steady_clock::now();
        const auto ms = [](auto duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        };
        if (trace_cpu_rate.Ready(end)) {
            LOG_INFO(Render_Vulkan,
                     "DKB renderer renderer_frame={} host_pacing_mode={} vsync_mode={} "
                     "cpu_frame_wall_ms={:.3f} "
                     "cpu_composite_wall_ms={:.3f} capture_wall_ms={:.3f} "
                     "acquire_frame_wall_ms={:.3f} record_submit_wall_ms={:.3f} "
                     "present_call_wall_ms={:.3f} frame_end_wall_ms={:.3f}",
                     trace_frame, static_cast<int>(Settings::values.frame_pacing_mode.GetValue()),
                     static_cast<int>(Settings::values.vsync_mode.GetValue()),
                     trace_last_frame == std::chrono::steady_clock::time_point{}
                         ? -1.0 : ms(entered - trace_last_frame),
                     ms(end - entered), ms(acquiring - entered), ms(acquired - acquiring),
                     ms(presenting - acquired), ms(presented - presenting), ms(end - presented));
        }
        trace_last_frame = entered;
    }
}

HdrPresent RendererVulkan::UpdateHdrPresent() {
    const auto& monitor = Common::DisplayHdrMonitor::Instance();
    const u64 key = (monitor.Generation() << 8) |
                    (u64{static_cast<u8>(Settings::values.hdr_mode.GetValue())} << 4) |
                    static_cast<u8>(Settings::values.hdr_swapchain.GetValue());
    const u32 luminance_key = (u32{Settings::values.hdr_peak_luminance.GetValue()} << 16) |
                              Settings::values.hdr_reference_white.GetValue();
    if (key == output_format_key && luminance_key == output_luminance_key) {
        return {output_format.colorSpace, output_reference_white, output_peak};
    }
    output_format_key = key;
    output_luminance_key = luminance_key;
    const auto previous = output_format;
    output_format = ChooseOutputFormat(device.GetPhysical(), *surface,
                                       render_window.GetWindowInfo().render_surface);
    const auto told = CaptureHdrPresent();
    output_reference_white = told.reference_white;
    output_peak = told.peak;
    if (previous.format != output_format.format ||
        previous.colorSpace != output_format.colorSpace) {
        if (output_format.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            LOG_INFO(Render_Vulkan,
                     "HDR output: {} {}, reference white {} nits, peak {} nits. Anti-aliasing, "
                     "post-processing and upscaling filters are off",
                     string_VkFormat(output_format.format),
                     string_VkColorSpaceKHR(output_format.colorSpace), output_reference_white,
                     output_peak);
        } else {
            LOG_INFO(Render_Vulkan, "HDR output: SDR {}", string_VkFormat(output_format.format));
        }
    }
    return {output_format.colorSpace, output_reference_white, output_peak};
}

HdrPresent RendererVulkan::CaptureHdrPresent() const {
    // The luminances the game was told (nx2:vi2 QueryDisplayHdrStatus / PaperWhite).
    const auto told = Common::ResolveHdrOutput(Common::DisplayHdrMonitor::Instance().Current(),
                                               Settings::HdrMode::On,
                                               Settings::values.hdr_peak_luminance.GetValue(),
                                               Settings::values.hdr_reference_white.GetValue());
    return {VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, static_cast<f32>(told.reference_white),
            static_cast<f32>(told.peak_luminance)};
}

VkSurfaceKHR RendererVulkan::PresentationSurface() {
#ifdef EGLANTINE_STREAMLINE
    // DLSS-G looks the window up by the swapchain's surface: replace the surface made before the
    // device with one made through Streamline, for the same window. Present support stays valid,
    // as it belongs to the window and the queue family.
    const auto& window_info = render_window.GetWindowInfo();
    if (Streamline::FrameGenSession()) {
        const VkSurfaceKHR registered =
            window_info.type == Core::Frontend::WindowSystemType::Windows
                ? Streamline::CreateFrameGenSurface(*instance, window_info.render_surface)
                : VK_NULL_HANDLE;
        if (registered) {
            surface = vk::SurfaceKHR(registered, *instance, instance.Dispatch());
        } else {
            Streamline::DisableFrameGen("Streamline could not make the window's surface");
        }
    }
#endif
    return *surface;
}

void RendererVulkan::Report() const {
    using namespace Common::Literals;

    const std::string api_version = GetReadableVersion(device.ApiVersion());

    const std::string extensions = BuildCommaSeparatedExtensions(device.GetAvailableExtensions());

    const auto available_vram = static_cast<f64>(device.GetDeviceLocalMemory()) / f64{1_GiB};

    LOG_INFO(Render_Vulkan, "Driver: {} {}", device.GetVendorName(), GetDriverVersion(device));
    LOG_INFO(Render_Vulkan, "Device: {}", device_info.model);
    LOG_INFO(Render_Vulkan, "Vulkan: {}", api_version);
    LOG_INFO(Render_Vulkan, "Available VRAM: {:.2f} GiB", available_vram);
    if (VideoCommon::RenderTrace::Enabled()) {
        LOG_INFO(Render_Vulkan, "Render trace enabled: interval=600 frames, log_filter={}",
                 Settings::values.log_filter.GetValue());
    }
    LOG_DEBUG(Render_Vulkan,
              "Render trace capabilities: geometry_shader={} transform_feedback={} "
              "geometry_streams={} mesh_shader={}",
              device.GetPhysical().GetFeatures().geometryShader, device.IsExtTransformFeedbackSupported(),
              device.AreTransformFeedbackGeometryStreamsSupported(), device.IsExtMeshShaderSupported());
}

vk::Buffer RendererVulkan::RenderToBuffer(std::span<const Tegra::FramebufferConfig> framebuffers,
                                          const Layout::FramebufferLayout& layout, VkFormat format,
                                          VkDeviceSize buffer_size) {
    auto& frame = screenshot_frame;
    auto dst_buffer = CreateWrappedBuffer(memory_allocator, buffer_size, MemoryUsage::Download);
    blit_capture.DrawToFrame(device, rasterizer, &frame, framebuffers, layout, 1, format,
                             CaptureHdrPresent());

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *frame.image, *dst_buffer,
                           VkExtent3D{layout.width, layout.height, 1});
    });

    // Ensure the copy is fully completed before saving the capture
    scheduler.Finish();

    // Copy backing image data to the capture buffer
    dst_buffer.Invalidate();
    return dst_buffer;
}

void RendererVulkan::RenderScreenshot(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (!renderer_settings.screenshot_requested) {
        return;
    }

    const auto screenshot_layers = Tegra::FilterLayerStack(
        framebuffers, renderer_settings.screenshot_layer_stack, screenshot_layer_scratch);

    const auto& layout{renderer_settings.screenshot_framebuffer_layout};
    const auto dst_buffer = RenderToBuffer(screenshot_layers, layout, VK_FORMAT_B8G8R8A8_UNORM,
                                           layout.width * layout.height * 4);

    std::memcpy(renderer_settings.screenshot_bits, dst_buffer.Mapped().data(),
                dst_buffer.Mapped().size());
    renderer_settings.screenshot_complete_callback(false);
    renderer_settings.screenshot_requested = false;
}

std::vector<u8> RendererVulkan::GetAppletCaptureBuffer() {
    using namespace VideoCore::Capture;

    std::vector<u8> out(VideoCore::Capture::TiledSize);

    if (!applet_frame.image) {
        return out;
    }

    const auto dst_buffer =
        CreateWrappedBuffer(memory_allocator, VideoCore::Capture::TiledSize, MemoryUsage::Download);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *applet_frame.image, *dst_buffer, CaptureImageExtent);
    });

    // Ensure the copy is fully completed before writing the capture
    scheduler.Finish();

    // Swizzle image data to the capture buffer
    dst_buffer.Invalidate();
    Tegra::Texture::SwizzleTexture(out, dst_buffer.Mapped(), BytesPerPixel, LinearWidth,
                                   LinearHeight, LinearDepth, BlockHeight, BlockDepth);

    return out;
}

void RendererVulkan::RenderAppletCaptureLayer(
    std::span<const Tegra::FramebufferConfig> framebuffers) {
    const auto capture_layers = Tegra::FilterLayerStack(
        framebuffers, Service::Nvnflinger::LayerStackId::LastFrame, applet_capture_layers);

    if (capture_layers.empty())
        return;

    if (!applet_frame.image) {
        applet_frame.image = CreateWrappedImage(memory_allocator, CaptureImageSize, CaptureFormat);
        applet_frame.image_view = CreateWrappedImageView(device, applet_frame.image, CaptureFormat);
        applet_frame.framebuffer = blit_applet.CreateFramebuffer(device,
            VideoCore::Capture::Layout, *applet_frame.image_view, CaptureFormat);
    }

    scheduler.RequestOutsideRenderPassOperationContext();
    blit_applet.DrawToFrame(device, rasterizer, &applet_frame, capture_layers, VideoCore::Capture::Layout, 1,
                            CaptureFormat, CaptureHdrPresent());
}

#ifdef EGLANTINE_STREAMLINE
VideoCore::DlssStatus RendererVulkan::GetDlssStatus() const {
    return StreamlineDlssUpscaler::Status();
}
#endif

} // namespace Vulkan
