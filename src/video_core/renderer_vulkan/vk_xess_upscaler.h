// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>

#include "common/common_types.h"
#include "common/settings_enums.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class MemoryAllocator;
class Scheduler;

/// XeSS-SR for Ocarina's tonemapped LDR scene, using the host-reconstructed camera motion.
class XessUpscaler {
public:
    struct Inputs {
        VkImage color_image{};
        VkImageView color_view{};
        VkImage depth_image{};
        VkImageView depth_view{};
        VkImage motion_image{};
        VkImageView motion_view{};
        VkExtent2D render_size{};
        VkExtent2D depth_size{};
        VkExtent2D output_size{};
        std::array<f32, 2> jitter{};
        bool reset{};
    };

    XessUpscaler(const Device& device, Scheduler& scheduler, MemoryAllocator& memory_allocator,
                 Settings::XessQuality quality);
    ~XessUpscaler();

    [[nodiscard]] static bool IsSupported(const Device& device);
    [[nodiscard]] bool Failed() const noexcept;
    /// False while pipelines initialize or if this frame's input is outside XeSS's supported range.
    bool Upscale(const Inputs& inputs);
    [[nodiscard]] VkImage OutputImage() const noexcept;
    [[nodiscard]] VkImageView OutputView() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan