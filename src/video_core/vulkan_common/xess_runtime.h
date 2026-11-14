// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

#include "common/common_types.h"

#ifdef _WIN32
#include <xess/xess.h>
#include <xess/xess_vk.h>
#endif

namespace Vulkan::XeSS {

#ifdef _WIN32
struct Api {
    decltype(&xessGetVersion) get_version{};
    decltype(&xessSetLoggingCallback) set_logging_callback{};
    decltype(&xessIsOptimalDriver) is_optimal_driver{};
    decltype(&xessSetJitterScale) set_jitter_scale{};
    decltype(&xessSetVelocityScale) set_velocity_scale{};
    decltype(&xessDestroyContext) destroy_context{};
    decltype(&xessVKGetRequiredInstanceExtensions) get_instance_extensions{};
    decltype(&xessVKGetRequiredDeviceExtensions) get_device_extensions{};
    decltype(&xessVKGetRequiredDeviceFeatures) get_device_features{};
    decltype(&xessVKCreateContext) create_context{};
    decltype(&xessVKBuildPipelines) build_pipelines{};
    decltype(&xessVKInit) init{};
    decltype(&xessVKExecute) execute{};
    decltype(&xessGetOptimalInputResolution) get_optimal_input_resolution{};
};

[[nodiscard]] const Api* GetApi();
#endif

[[nodiscard]] bool IsRequested();
[[nodiscard]] bool Start();
[[nodiscard]] bool IsAvailable();
[[nodiscard]] u32 RequiredInstanceApiVersion();
[[nodiscard]] std::vector<std::string> InstanceExtensions();
[[nodiscard]] std::vector<std::string> DeviceExtensions(VkInstance instance,
                                                         VkPhysicalDevice physical);
[[nodiscard]] bool EnableRequiredDeviceFeatures(VkInstance instance, VkPhysicalDevice physical,
                                               void** features);
void Disable(std::string_view reason);

} // namespace Vulkan::XeSS