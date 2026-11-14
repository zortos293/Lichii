// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/vulkan_common/xess_runtime.h"

#include <mutex>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"

#ifdef _WIN32
#include <array>
#include <filesystem>
#include <windows.h>
#endif

namespace Vulkan::XeSS {
namespace {

#ifdef _WIN32
struct State {
    HMODULE module{};
    Api api{};
    bool started{};
    bool available{};
    u32 minimum_api_version{VK_API_VERSION_1_1};
    std::vector<std::string> instance_extensions;
};

State& GetState() {
    static State state;
    return state;
}

std::mutex& GetMutex() {
    static std::mutex mutex;
    return mutex;
}

template <typename Function>
bool Resolve(HMODULE module, const char* name, Function& function) {
    function = reinterpret_cast<Function>(GetProcAddress(module, name));
    if (!function) {
        LOG_WARNING(Render_Vulkan, "XeSS runtime is missing {}", name);
    }
    return function != nullptr;
}

#endif

} // namespace

bool IsRequested() {
    return Settings::values.ocarina_xess.GetValue();
}

bool Start() {
#ifdef _WIN32
    if (!IsRequested()) {
        return false;
    }
    std::scoped_lock lock{GetMutex()};
    State& state = GetState();
    if (state.started) {
        return state.available;
    }
    state.started = true;

    const std::filesystem::path user_runtime =
        Common::FS::GetEdenPath(Common::FS::EdenPath::XeSSDir) / L"libxess.dll";
    const std::filesystem::path executable_runtime =
        Common::FS::GetExeDirectory() / L"libxess.dll";
    std::error_code ec;
    std::filesystem::path runtime;
    if (std::filesystem::is_regular_file(user_runtime, ec)) {
        runtime = user_runtime;
    } else if (std::filesystem::is_regular_file(executable_runtime, ec)) {
        runtime = executable_runtime;
    }
    if (runtime.empty()) {
        LOG_INFO(Render_Vulkan,
                 "XeSS-SR runtime not found; get libxess.dll from Intel XeSS SDK 3.0.2 "
                 "(https://github.com/intel/xess/releases/tag/v3.0.2) and place it in {} "
                 "or next to the executable; using the selected fallback",
                 Common::FS::PathToUTF8String(user_runtime.parent_path()));
        return false;
    }

    state.module = LoadLibraryExW(runtime.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!state.module) {
        LOG_WARNING(Render_Vulkan, "Could not load XeSS runtime {} (error {})",
                    Common::FS::PathToUTF8String(runtime), GetLastError());
        return false;
    }

    Api& api = state.api;
    const HMODULE module = state.module;
    const bool resolved =
        Resolve(module, "xessGetVersion", api.get_version) &&
        Resolve(module, "xessSetLoggingCallback", api.set_logging_callback) &&
        Resolve(module, "xessIsOptimalDriver", api.is_optimal_driver) &&
        Resolve(module, "xessSetJitterScale", api.set_jitter_scale) &&
        Resolve(module, "xessSetVelocityScale", api.set_velocity_scale) &&
        Resolve(module, "xessDestroyContext", api.destroy_context) &&
        Resolve(module, "xessVKGetRequiredInstanceExtensions", api.get_instance_extensions) &&
        Resolve(module, "xessVKGetRequiredDeviceExtensions", api.get_device_extensions) &&
        Resolve(module, "xessVKGetRequiredDeviceFeatures", api.get_device_features) &&
        Resolve(module, "xessVKCreateContext", api.create_context) &&
        Resolve(module, "xessVKBuildPipelines", api.build_pipelines) &&
        Resolve(module, "xessVKInit", api.init) &&
        Resolve(module, "xessVKExecute", api.execute) &&
        Resolve(module, "xessGetOptimalInputResolution", api.get_optimal_input_resolution);
    if (!resolved) {
        FreeLibrary(module);
        state.module = nullptr;
        state.api = {};
        return false;
    }

    xess_version_t version{};
    if (api.get_version(&version) != XESS_RESULT_SUCCESS || version.major != 3) {
        LOG_WARNING(Render_Vulkan, "Unsupported XeSS runtime version {}.{}.{}; expected 3.x",
                    version.major, version.minor, version.patch);
        FreeLibrary(module);
        state.module = nullptr;
        state.api = {};
        return false;
    }

    uint32_t count{};
    const char* const* names{};
    if (api.get_instance_extensions(&count, &names, &state.minimum_api_version) !=
            XESS_RESULT_SUCCESS ||
        (count != 0 && names == nullptr)) {
        LOG_WARNING(Render_Vulkan, "XeSS runtime could not report its Vulkan instance requirements");
        FreeLibrary(module);
        state.module = nullptr;
        state.api = {};
        return false;
    }
    state.instance_extensions.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        state.instance_extensions.emplace_back(names[index]);
    }
    state.available = true;
    LOG_INFO(Render_Vulkan, "XeSS-SR runtime {}.{}.{} loaded from {}", version.major,
             version.minor, version.patch, Common::FS::PathToUTF8String(runtime));
    return true;
#else
    return false;
#endif
}

bool IsAvailable() {
#ifdef _WIN32
    return Start();
#else
    return false;
#endif
}

u32 RequiredInstanceApiVersion() {
#ifdef _WIN32
    if (!Start() || GetState().minimum_api_version < VK_API_VERSION_1_1) {
        return VK_API_VERSION_1_1;
    }
    return GetState().minimum_api_version;
#else
    return VK_API_VERSION_1_1;
#endif
}

std::vector<std::string> InstanceExtensions() {
#ifdef _WIN32
    if (!Start()) {
        return {};
    }
    return GetState().instance_extensions;
#else
    return {};
#endif
}

std::vector<std::string> DeviceExtensions(VkInstance instance, VkPhysicalDevice physical) {
#ifdef _WIN32
    if (!Start()) {
        return {};
    }
    uint32_t count{};
    const char* const* names{};
    const xess_result_t result = GetState().api.get_device_extensions(instance, physical, &count,
                                                                      &names);
    if (result != XESS_RESULT_SUCCESS || (count != 0 && names == nullptr)) {
        LOG_WARNING(Render_Vulkan, "XeSS runtime could not report its Vulkan device requirements");
        Disable("the runtime could not report its device requirements");
        return {};
    }
    std::vector<std::string> extensions;
    extensions.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        extensions.emplace_back(names[index]);
    }
    return extensions;
#else
    return {};
#endif
}

bool EnableRequiredDeviceFeatures(VkInstance instance, VkPhysicalDevice physical,
                                  void** features) {
#ifdef _WIN32
    if (!Start()) {
        return false;
    }
    if (GetState().api.get_device_features(instance, physical, features) == XESS_RESULT_SUCCESS) {
        return true;
    }
    LOG_INFO(Render_Vulkan, "XeSS-SR requirements are unsupported on this Vulkan device");
    return false;
#else
    (void)instance;
    (void)physical;
    (void)features;
    return false;
#endif
}

void Disable(std::string_view reason) {
#ifdef _WIN32
    std::scoped_lock lock{GetMutex()};
    State& state = GetState();
    if (state.available) {
        LOG_INFO(Render_Vulkan, "XeSS-SR unavailable: {}; using the selected fallback", reason);
    }
    state.available = false;
    state.instance_extensions.clear();
#else
    (void)reason;
#endif
}

#ifdef _WIN32
const Api* GetApi() {
    State& state = GetState();
    if (!state.started) {
        (void)Start();
    }
    return state.available ? &state.api : nullptr;
}
#endif

} // namespace Vulkan::XeSS