// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string_view>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/frame_gen/lossless_dll.h"
#include "video_core/renderer_vulkan/present/lsfg_shaders.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

namespace {

using VideoCore::FrameGen::LosslessStatus;
using VideoCore::FrameGen::ShaderVariant;

[[nodiscard]] std::string_view StatusText(LosslessStatus status) {
    switch (status) {
    case LosslessStatus::Ok:
        return "ok";
    case LosslessStatus::NotInstalled:
        return "Lossless.dll is not installed";
    case LosslessStatus::UnreadableFile:
        return "Lossless.dll can't be read";
    case LosslessStatus::NotPortableExecutable:
        return "Lossless.dll is not a Windows DLL";
    case LosslessStatus::MissingShaders:
        return "Lossless.dll has neither the LSFG SPIR-V nor the DXBC shaders";
    case LosslessStatus::TranslationFailed:
        return "Lossless.dll's LSFG shaders could not be prepared";
    case LosslessStatus::CacheUnusable:
        return "the LSFG shader cache could not be written";
    }
    return "unknown status";
}

[[nodiscard]] std::string_view SourceText(ShaderVariant variant) {
    switch (variant) {
    case ShaderVariant::NativeFp16:
    case ShaderVariant::NativeFp32:
        return "Lossless.dll SPIR-V";
    case ShaderVariant::TranslatedFp32:
        return "translated from Lossless.dll DXBC, which has no SPIR-V";
    }
    return "unknown";
}

[[nodiscard]] u32 MaxSpirvVersion(u32 api_version) {
    if (api_version >= VK_API_VERSION_1_3) {
        return 0x10600;
    }
    if (api_version >= VK_API_VERSION_1_2) {
        return 0x10500;
    }
    if (api_version >= VK_API_VERSION_1_1) {
        return 0x10300;
    }
    return 0x10000;
}

} // Anonymous namespace

LsfgShaders::LsfgShaders(const Device& device) {
    const bool wanted = Settings::values.frame_gen.GetValue();
    if (!device.IsVulkanMemoryModelSupported() || !device.HasNullDescriptor()) {
        if (wanted) {
            LOG_WARNING(Render_Vulkan, "LSFG unavailable: memory model {}, null descriptor {}",
                        device.IsVulkanMemoryModelSupported(), device.HasNullDescriptor());
        }
        return;
    }
    device_supported = true;

    const bool fp16 = device.IsHostFloat16Supported();
    VideoCore::FrameGen::LoadedShaders shaders;
    const auto status = VideoCore::FrameGen::LoadShaderModules(
        VideoCore::FrameGen::GetLosslessDllPath(), VideoCore::FrameGen::GetShaderCachePath(), fp16,
        shaders);
    if (status != LosslessStatus::Ok) {
        if (wanted) {
            LOG_WARNING(Render_Vulkan, "LSFG unavailable: {} (status {}, GPU float16 {})",
                        StatusText(status), static_cast<u32>(status), fp16);
        }
        return;
    }

    const u32 max_spirv = MaxSpirvVersion(device.ApiVersion());
    for (const auto& [id, words] : shaders.modules) {
        if (words[1] > max_spirv) {
            if (wanted) {
                LOG_WARNING(Render_Vulkan,
                            "LSFG unavailable: shader {} needs SPIR-V {}.{}, the driver's Vulkan "
                            "{}.{} takes up to {}.{}",
                            id, (words[1] >> 16) & 0xff, (words[1] >> 8) & 0xff,
                            VK_API_VERSION_MAJOR(device.ApiVersion()),
                            VK_API_VERSION_MINOR(device.ApiVersion()), (max_spirv >> 16) & 0xff,
                            (max_spirv >> 8) & 0xff);
            }
            device_supported = false;
            return;
        }
    }

    LOG_INFO(Render_Vulkan, "LSFG shaders loaded: {} {} modules ({}{})", shaders.modules.size(),
             shaders.variant == ShaderVariant::NativeFp16 ? "fp16" : "fp32",
             SourceText(shaders.variant), shaders.from_cache ? ", cached" : "");

    for (const auto& [id, words] : shaders.modules) {
        modules.emplace(id, CreateWrappedShaderModule(device, words));
    }
    valid = true;
}

VkShaderModule LsfgShaders::Get(u32 shader_id) const {
    const auto hit = modules.find(shader_id);
    return hit == modules.end() ? VK_NULL_HANDLE : *hit->second;
}

} // namespace Vulkan
