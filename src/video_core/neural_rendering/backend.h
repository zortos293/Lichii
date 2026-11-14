// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vulkan/vulkan_core.h>

namespace VideoCore::NeuralRendering {

enum class Style : std::uint32_t { Standard, Natural, Cinematic };

struct Frame {
    Style style = Style::Standard;
    float intensity = 1.0f;
    float tone = 1.0f;
    float structure = 1.0f;
    std::array<float, 2> motion_scale{};  // Stored motion vector to UV offset.
    std::array<float, 2> motion_jitter{}; // Motion sampling offset in output pixels.
    bool reset = false;
    bool hdr = false;
};

// Private native inference backend. Construction submits uploads and warms the graph: the caller
// must finish the renderer queue first and externally synchronize device queues throughout
// construction and destruction. Images/views must outlive the backend and remain GENERAL.
// The model directory must have passed IsInstalled(). No Vulkan loader symbols escape this target.
class Backend {
public:
    Backend(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
            std::uint32_t queue_family, const std::filesystem::path& model, VkExtent2D extent,
            VkImage color, VkImageView color_view, VkImageView motion_view);
    ~Backend();
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    // Record after DLSS, outside a render pass, on the renderer's graphics queue. No allocation,
    // queue submission, descriptor update or CPU image copy occurs here. Overwrites color last.
    void Record(VkCommandBuffer commands, const Frame& frame);

    // Rebind same-sized renderer images after finishing their queue. Keeps the model, pipelines
    // and temporal history resident; recorded commands are rebuilt because descriptors changed.
    void Rebind(VkImage color, VkImageView color_view, VkImageView motion_view);

    // Only call after the submitted frame has completed on the GPU.
    [[nodiscard]] bool HadTimeout() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace VideoCore::NeuralRendering
