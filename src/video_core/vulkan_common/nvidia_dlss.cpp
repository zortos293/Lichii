// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/vulkan_common/nvidia_dlss.h"
#include "video_core/vulkan_common/streamline.h"

namespace Vulkan::NvidiaDlss {

bool Start(void* window) { return Streamline::Start(window); }
bool WantsDevice() { return Streamline::WantsDevice(); }
std::vector<std::string> InstanceExtensions() { return Streamline::InstanceExtensions(); }
std::vector<std::string> DeviceExtensions(VkInstance, VkPhysicalDevice) {
    return Streamline::DeviceExtensions();
}
bool AttachDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  u32 graphics_family, u32 first_streamline_queue, vk::DeviceDispatch& dld) {
    return Streamline::AttachDevice(instance, physical, device, graphics_family,
                                    first_streamline_queue, dld);
}
bool IsAvailable() { return Streamline::IsDlssAvailable(); }
void Stop() { Streamline::Stop(); }

} // namespace Vulkan::NvidiaDlss
