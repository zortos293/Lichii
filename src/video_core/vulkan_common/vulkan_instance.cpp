// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <future>
#include <optional>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "common/dynamic_library.h"
#include "common/logging.h"
#include <ranges>
#include <vulkan/vulkan_core.h>
#include "core/frontend/emu_window.h"
#include "video_core/vulkan_common/vulkan_address_map.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#ifdef EGLANTINE_STREAMLINE
#include <algorithm>
#include <string>
#include "video_core/vulkan_common/streamline.h"
#endif

namespace Vulkan {
namespace {

/// Whether the driver lists an extension. Silent: an optional extension may be missing.
[[nodiscard]] bool HasExtension(std::vector<VkExtensionProperties> const& properties,
                                const char* extension) {
    return std::ranges::any_of(properties, [extension](const VkExtensionProperties& property) {
        return std::strcmp(extension, property.extensionName) == 0;
    });
}

[[nodiscard]] bool AreExtensionsSupported(std::vector<VkExtensionProperties> const& properties,
                                          std::span<const char* const> extensions) {
    for (const char* extension : extensions) {
        if (!HasExtension(properties, extension)) {
            LOG_ERROR(Render_Vulkan, "Required instance extension {} is not available", extension);
            return false;
        }
    }
    return true;
}

} // Anonymous namespace

std::vector<const char*> RequiredExtensions(std::vector<VkExtensionProperties> const& properties,
                                            Core::Frontend::WindowSystemType window_type,
                                            bool enable_validation) {
    std::vector<const char*> extensions;
    extensions.reserve(7);
    // Headless presents to VK_EXT_headless_surface where the driver has it (GPU trace replay).
    const bool headless_surface =
        window_type == Core::Frontend::WindowSystemType::Headless &&
        HasExtension(properties, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
    switch (window_type) {
    case Core::Frontend::WindowSystemType::Headless:
        if (headless_surface) {
            extensions.push_back(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
        }
        break;
#ifdef _WIN32
    case Core::Frontend::WindowSystemType::Windows:
        extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
        break;
#elif defined(__APPLE__)
    case Core::Frontend::WindowSystemType::Cocoa:
        extensions.push_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
        break;
#elif defined(__ANDROID__)
    case Core::Frontend::WindowSystemType::Android:
        extensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
        break;
#elif defined(__HAIKU__)
    case Core::Frontend::WindowSystemType::Xcb:
        extensions.push_back(VK_KHR_XCB_SURFACE_EXTENSION_NAME);
        break;
#else
    case Core::Frontend::WindowSystemType::X11:
        extensions.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
        break;
    case Core::Frontend::WindowSystemType::Wayland:
        extensions.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
        break;
#endif
    default:
        LOG_ERROR(Render_Vulkan, "Presentation not supported on this platform");
        break;
    }
    if (window_type != Core::Frontend::WindowSystemType::Headless || headless_surface) {
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        if (HasExtension(properties, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) {
            extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        }
        // Surface queries for present ids and present waits (VK_KHR_present_id2/wait2).
        if (HasExtension(properties, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        }
    }
    // Probe optional extensions against the same snapshot the caller verifies against, so the
    // check here and the verification in CreateInstance can never disagree (see TOCTOU note below).
#ifdef __APPLE__
    if (HasExtension(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif
    if ((enable_validation || Vulkan::AddressMap::Wanted()) &&
        HasExtension(properties, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#ifdef EGLANTINE_STREAMLINE
    // What NVIDIA's DLSS asks for through Streamline, where available. Static: the returned
    // pointers must outlive this call.
    static std::vector<std::string> streamline;
    streamline = Streamline::InstanceExtensions();
    for (const std::string& name : streamline) {
        const bool available = HasExtension(properties, name.c_str());
        const bool listed = std::ranges::any_of(extensions, [&](const char* e) { return name == e; });
        if (available && !listed) {
            extensions.push_back(name.c_str());
        }
    }
#endif
    return extensions;
}

namespace {

[[nodiscard]] std::vector<const char*> Layers(bool enable_validation) {
    std::vector<const char*> layers;
    if (enable_validation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }
    return layers;
}

void RemoveUnavailableLayers(const vk::InstanceDispatch& dld, std::vector<const char*>& layers) {
    const std::optional layer_properties = vk::EnumerateInstanceLayerProperties(dld);
    if (!layer_properties) {
        LOG_ERROR(Render_Vulkan, "Failed to query layer properties, disabling layers");
        layers.clear();
    }
    std::erase_if(layers, [&layer_properties](const char* layer) {
        const auto comp = [layer](const VkLayerProperties& layer_property) {
            return std::strcmp(layer, layer_property.layerName) == 0;
        };
        const auto it = std::ranges::find_if(*layer_properties, comp);
        if (it == layer_properties->end()) {
            LOG_ERROR(Render_Vulkan, "Layer {} not available, removing it", layer);
            return true;
        }
        return false;
    });
}
} // Anonymous namespace

vk::Instance CreateInstance(const Common::DynamicLibrary& library, vk::InstanceDispatch& dld,
                            u32 required_version, Core::Frontend::WindowSystemType window_type,
                            bool enable_validation) {
    if (!library.IsOpen()) {
        LOG_ERROR(Render_Vulkan, "Vulkan library not available");
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    if (!library.GetSymbol("vkGetInstanceProcAddr", &dld.vkGetInstanceProcAddr)) {
        LOG_ERROR(Render_Vulkan, "vkGetInstanceProcAddr not present in Vulkan");
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    if (!vk::Load(dld)) {
        LOG_ERROR(Render_Vulkan, "Failed to load Vulkan function pointers");
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    // Enumerate instance extensions exactly once. RequiredExtensions() used to enumerate a second
    // time internally; if the driver returned a different set between the two calls (a real TOCTOU
    // seen on some AMD iGPU drivers), an extension added to the list could be missing from the
    // verification snapshot, throwing EXTENSION_NOT_PRESENT on an otherwise valid launch. Sharing
    // one snapshot for both the optional-extension probe and the final check removes that window.
    auto const properties = vk::EnumerateInstanceExtensionProperties(dld);
    if (!properties) {
        LOG_ERROR(Render_Vulkan, "Failed to query instance extension properties");
        throw vk::Exception(VK_ERROR_EXTENSION_NOT_PRESENT);
    }
    std::vector<const char*> const extensions =
        RequiredExtensions(*properties, window_type, enable_validation);
    if (!AreExtensionsSupported(*properties, extensions))
        throw vk::Exception(VK_ERROR_EXTENSION_NOT_PRESENT);
    std::vector<const char*> layers = Layers(enable_validation);
    RemoveUnavailableLayers(dld, layers);

    const u32 available_version = vk::AvailableVersion(dld);
    if (available_version < required_version) {
        LOG_ERROR(Render_Vulkan, "Vulkan {}.{} is not supported, {}.{} is required",
                  VK_VERSION_MAJOR(available_version), VK_VERSION_MINOR(available_version),
                  VK_VERSION_MAJOR(required_version), VK_VERSION_MINOR(required_version));
        throw vk::Exception(VK_ERROR_INCOMPATIBLE_DRIVER);
    }
    vk::Instance instance =
        std::async([&] {
            return vk::Instance::Create(available_version, layers, extensions, dld);
        }).get();
    if (!vk::Load(*instance, dld)) {
        LOG_ERROR(Render_Vulkan, "Failed to load Vulkan instance function pointers");
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    // A loader may return entry points of extensions the instance did not enable.
    if (std::ranges::none_of(extensions, [](const char* name) {
            return std::strcmp(name, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) == 0;
        })) {
        dld.vkGetPhysicalDeviceSurfaceCapabilities2KHR = nullptr;
    }
    return instance;
}

} // namespace Vulkan
