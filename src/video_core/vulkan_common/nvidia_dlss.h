// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>
#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan::NvidiaDlss {

// Start before creating the renderer instance; attach after enabling the required extensions.
bool Start(void* window = nullptr);
[[nodiscard]] bool WantsDevice();
[[nodiscard]] std::vector<std::string> InstanceExtensions();
[[nodiscard]] std::vector<std::string> DeviceExtensions(VkInstance instance,
                                                        VkPhysicalDevice physical);
bool AttachDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  u32 graphics_family, u32 first_streamline_queue, vk::DeviceDispatch& dld);
[[nodiscard]] bool IsAvailable();
// Called after upscalers are destroyed and before the Vulkan device is destroyed.
void Stop();

} // namespace Vulkan::NvidiaDlss
