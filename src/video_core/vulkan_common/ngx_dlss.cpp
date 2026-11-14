// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <vector>
#include <fmt/format.h>
#include "common/dynamic_library.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "video_core/vulkan_common/ngx_api.h"
#include "video_core/vulkan_common/ngx_dlss.h"
#include "video_core/vulkan_common/nvidia_dlss.h"

namespace Vulkan {
namespace {

constexpr const char *ProjectId = "8d3e2f9a-6b41-4c7e-9a52-1f0c3b7d5e64";

struct State {
    Common::DynamicLibrary core;
    Ngx::Api api;
    std::vector<std::string> instance_extensions;
    std::vector<std::string> device_extensions;
    std::wstring model_directory;
    std::wstring cache_directory;
    VkDevice device{};
    bool started{};
    bool available{};
};

State &GetState() {
    static State state;
    return state;
}

std::mutex status_mutex;
Ngx::RuntimeStatus status;
std::atomic<u64> evaluations{};
PFN_vkGetDeviceProcAddr ngx_device_proc{};
VkDevice ngx_device{};

// DLSS 310.9 can clear an internal image after a transition whose destination scope
// only includes shader writes. This wrapper affects barriers requested by NGX only.
VKAPI_ATTR void VKAPI_CALL NgxPipelineBarrier(
    VkCommandBuffer command, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags flags, u32 memory_count, const VkMemoryBarrier* memories,
    u32 buffer_count, const VkBufferMemoryBarrier* buffers, u32 image_count,
    const VkImageMemoryBarrier* images) {
    const auto barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(
        ngx_device_proc(ngx_device, "vkCmdPipelineBarrier"));
    const auto needs_transfer = [](const VkImageMemoryBarrier& image) {
        return image.newLayout == VK_IMAGE_LAYOUT_GENERAL &&
               (image.dstAccessMask & VK_ACCESS_SHADER_WRITE_BIT);
    };
    if (!image_count || !std::any_of(images, images + image_count, needs_transfer)) {
        barrier(command, src, dst, flags, memory_count, memories, buffer_count, buffers,
                image_count, images);
        return;
    }
    std::vector<VkImageMemoryBarrier> patched(images, images + image_count);
    for (auto& image : patched) {
        if (needs_transfer(image)) {
            image.dstAccessMask |= VK_ACCESS_TRANSFER_WRITE_BIT;
        }
    }
    barrier(command, src, dst | VK_PIPELINE_STAGE_TRANSFER_BIT, flags, memory_count, memories,
            buffer_count, buffers, image_count, patched.data());
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL NgxGetDeviceProcAddr(VkDevice device, const char* name) {
    const auto original = ngx_device_proc(device, name);
    if (original && std::strcmp(name, "vkCmdPipelineBarrier") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(NgxPipelineBarrier);
    }
    return original;
}

std::wstring WidePath(const std::filesystem::path &path) {
    const auto utf32 = Common::UTF8ToUTF32(Common::FS::PathToUTF8String(path));
    return {utf32.begin(), utf32.end()};
}

/// Forward trimmed NGX messages to the emulator log only when NGX diagnostic logging is
/// enabled.
void LogFromNgx(const char *message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    if (const char *enabled = std::getenv("EDEN_NX2_DLSS_LOG"); enabled && enabled[0] == '1') {
        std::string_view text{message ? message : ""};
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.remove_suffix(1);
        }
        LOG_INFO(Render_Vulkan, "NGX: {}", text);
    }
}

} // namespace

namespace Ngx {

std::filesystem::path UserFolder() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "ngx";
}

/// Find the Linux DLSS library in override, user-data, and executable-relative runtime
/// locations.
std::filesystem::path FindModel() {
    std::vector<std::filesystem::path> dirs;
    if (const char *dir = std::getenv("LICHII_NGX_DIR"); dir && *dir) {
        dirs.emplace_back(dir);
    }
    dirs.push_back(UserFolder());
    std::error_code executable_error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", executable_error);
    if (!executable_error) {
        dirs.push_back(executable.parent_path() / "ngx");
    }
    for (const auto &dir : dirs) {
        std::error_code ec;
        for (const auto *name : {ModelFilename, "libnvidia-ngx-dlss.so"}) {
            const auto path = dir / name;
            if (std::filesystem::is_regular_file(path, ec)) {
                return path;
            }
        }
    }
    return {};
}

const Api &GetApi() { return GetState().api; }

RuntimeStatus GetStatus() {
    std::scoped_lock lock{status_mutex};
    auto snapshot = status;
    snapshot.evaluations = evaluations.load(std::memory_order_relaxed);
    return snapshot;
}

void ReportFailure(std::string reason) {
    LOG_WARNING(Render_Vulkan, "NVIDIA DLSS (NGX): {}", reason);
    std::scoped_lock lock{status_mutex};
    status.available = false;
    status.error = std::move(reason);
}

void ReportEvaluation() { evaluations.fetch_add(1, std::memory_order_relaxed); }

} // namespace Ngx

namespace NvidiaDlss {

/// Prepare NGX before Vulkan device creation, even when DLSS will initially be disabled.
bool Start(void*) {
    Stop();
    {
        std::scoped_lock lock{status_mutex};
        status = {};
        evaluations.store(0, std::memory_order_relaxed);
    }
    State &state = GetState();
    const auto model = Ngx::FindModel();
    if (model.empty()) {
        Ngx::ReportFailure("Linux DLSS model not found; install it in Graphics settings");
        return false;
    }
    if (!state.core.Open("libnvidia-ngx.so.1")) {
        Ngx::ReportFailure("NGX core is missing from the NVIDIA driver installation");
        return false;
    }
    const auto resolve = [&](const char *name, auto &fn) {
        if (state.core.GetSymbol(name, &fn)) {
            return true;
        }
        Ngx::ReportFailure(fmt::format("NGX core has no {}", name));
        return false;
    };
    auto &api = state.api;
    if (!(resolve("NVSDK_NGX_VULKAN_Init_ProjectID_Ext", api.init) &&
          resolve("NVSDK_NGX_VULKAN_Shutdown1", api.shutdown) &&
          resolve("NVSDK_NGX_VULKAN_RequiredExtensions", api.required_extensions) &&
          resolve("NVSDK_NGX_VULKAN_GetCapabilityParameters", api.capabilities) &&
          resolve("NVSDK_NGX_VULKAN_AllocateParameters", api.allocate_parameters) &&
          resolve("NVSDK_NGX_VULKAN_DestroyParameters", api.destroy_parameters) &&
          resolve("NVSDK_NGX_VULKAN_CreateFeature1", api.create) &&
          resolve("NVSDK_NGX_VULKAN_ReleaseFeature", api.release) &&
          resolve("NVSDK_NGX_VULKAN_EvaluateFeature", api.evaluate))) {
        Stop();
        return false;
    }
    // The driver's feature-specific instance query currently returns NotImplemented.
    // Its SR extension query remains available, including on older supported drivers.
    unsigned instance_count{}, device_count{};
    const char **instance_names{}, **device_names{};
    const auto result =
        api.required_extensions(&instance_count, &instance_names, &device_count, &device_names);
    if (NVSDK_NGX_FAILED(result)) {
        Ngx::ReportFailure(fmt::format("extension query failed ({:#x})", static_cast<u32>(result)));
        Stop();
        return false;
    }
    for (unsigned i = 0; i < instance_count; ++i) {
        state.instance_extensions.emplace_back(instance_names[i]);
    }
    for (unsigned i = 0; i < device_count; ++i) {
        state.device_extensions.emplace_back(device_names[i]);
    }
    const auto cache = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "ngx";
    std::error_code ec;
    std::filesystem::create_directories(cache, ec);
    if (ec) {
        Ngx::ReportFailure(fmt::format("cannot create NGX cache: {}", ec.message()));
        Stop();
        return false;
    }
    state.model_directory = WidePath(model.parent_path());
    state.cache_directory = WidePath(cache);
    state.started = true;
    return true;
}

/// Report whether NGX startup succeeded and is still waiting for a Vulkan device attachment.
bool WantsDevice() {
    const auto& state = GetState();
    return state.started && !state.device;
}
bool IsAvailable() { return GetState().available; }
std::vector<std::string> InstanceExtensions() { return GetState().instance_extensions; }
std::vector<std::string> DeviceExtensions(VkInstance, VkPhysicalDevice) {
    return GetState().device_extensions;
}

/// Initialize NGX on one supported NVIDIA Vulkan device and verify DLSS availability.
bool AttachDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device, u32, u32,
                  vk::DeviceDispatch &dld) {
    State &state = GetState();
    if (!WantsDevice()) {
        return false;
    }
    VkPhysicalDeviceProperties props{};
    dld.vkGetPhysicalDeviceProperties(physical, &props);
    if (props.vendorID != 0x10de) {
        Ngx::ReportFailure("the selected Vulkan device is not an NVIDIA GPU");
        Stop();
        return false;
    }
    const wchar_t *paths[]{state.model_directory.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo = {paths, 1};
    info.LoggingInfo = {LogFromNgx, NVSDK_NGX_LOGGING_LEVEL_ON, true};
    ngx_device_proc = dld.vkGetDeviceProcAddr;
    ngx_device = device;
    const auto result =
        state.api.init(ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1", state.cache_directory.c_str(),
                       instance, physical, device, dld.vkGetInstanceProcAddr,
                       NgxGetDeviceProcAddr, NVSDK_NGX_Version_API, &info);
    if (NVSDK_NGX_FAILED(result)) {
        Ngx::ReportFailure(fmt::format("initialization failed ({:#x})", static_cast<u32>(result)));
        Stop();
        return false;
    }
    state.device = device;
    NVSDK_NGX_Parameter *caps{};
    const auto cap_result = state.api.capabilities(&caps);
    int available{};
    const bool supported =
        !NVSDK_NGX_FAILED(cap_result) && caps &&
        !NVSDK_NGX_FAILED(caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available)) &&
        available;
    if (caps) {
        state.api.destroy_parameters(caps);
    }
    if (!supported) {
        Ngx::ReportFailure("DLSS is unavailable on this GPU, driver, or model");
        Stop();
        return false;
    }
    state.available = true;
    {
        std::scoped_lock lock{status_mutex};
        status.available = true;
    }
    LOG_INFO(Render_Vulkan, "NVIDIA DLSS is available through native Linux NGX");
    return true;
}

/// Shut down the attached NGX runtime and unload its API after renderer-owned features have
/// been released.
void Stop() {
    State &state = GetState();
    if (state.device) {
        int in_use{};
        const auto result = state.api.shutdown(state.device, &in_use);
        LOG_INFO(Render_Vulkan, "Native NGX shutdown: {:#x}", static_cast<u32>(result));
    }
    state.device = VK_NULL_HANDLE;
    state.available = false;
    state.started = false;
    state.instance_extensions.clear();
    state.device_extensions.clear();
    state.api = {};
    state.core.Close();
    std::scoped_lock lock{status_mutex};
    status.available = false;
}

} // namespace NvidiaDlss
} // namespace Vulkan
