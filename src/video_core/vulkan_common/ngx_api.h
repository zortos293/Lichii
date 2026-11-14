// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vulkan/vulkan_core.h>
#include <nvsdk_ngx_vk.h>
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan::Ngx {

struct Api {
    // NGX core's init/shutdown ABI differs from the static SDK wrapper's entry points.
    using Init = NVSDK_NGX_Result (*)(const char *, NVSDK_NGX_EngineType, const char *,
                                      const wchar_t *, VkInstance, VkPhysicalDevice, VkDevice,
                                      PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr,
                                      NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo *);
    Init init{};
    NVSDK_NGX_Result (*shutdown)(VkDevice, int *){};
    decltype(&NVSDK_NGX_VULKAN_RequiredExtensions) required_extensions{};
    decltype(&NVSDK_NGX_VULKAN_GetCapabilityParameters) capabilities{};
    decltype(&NVSDK_NGX_VULKAN_AllocateParameters) allocate_parameters{};
    decltype(&NVSDK_NGX_VULKAN_DestroyParameters) destroy_parameters{};
    decltype(&NVSDK_NGX_VULKAN_CreateFeature1) create{};
    decltype(&NVSDK_NGX_VULKAN_ReleaseFeature) release{};
    decltype(&NVSDK_NGX_VULKAN_EvaluateFeature) evaluate{};
};

[[nodiscard]] const Api &GetApi();

} // namespace Vulkan::Ngx
