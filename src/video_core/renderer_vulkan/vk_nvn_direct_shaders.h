// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef LICHII_NVN_DIRECT

#include <unordered_map>

#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/nvn_direct/nvn_direct.h"

namespace Vulkan {

class Device;

/// NVN2 direct mode: the replay's shaders through Lichii's recompiler instead of
/// the offline library. A program stage's Ampere code is lowered to Maxwell code and translated as
/// the Maxwell path does it (GenericEnvironment::LowerNativeCode, PipelineCache), one stage at a
/// time: its SPIR-V numbers its descriptors from the replay's first binding of the stage, and its
/// descriptors are reported for the replay to bind (vka_recompile.c).
class NvnDirectShaders {
public:
    explicit NvnDirectShaders(const Device& device, const Shader::Profile& profile,
                              const Shader::HostTranslateInfo& host_info);

    /// nvn_direct_device::shader_translate and shader_free.
    static int Translate(void* context, const nvn_direct_shader_source* source,
                         nvn_direct_shader* shader);
    static void Free(void* context, nvn_direct_shader* shader);

    /// The pixel format of a VkFormat the replay gives a texture (the first Lichii format with it).
    [[nodiscard]] Shader::TexturePixelFormat PixelFormat(VkFormat format) const;

private:
    void TranslateStage(const nvn_direct_shader_source& source, nvn_direct_shader& shader) const;

    Shader::Profile profile;
    Shader::HostTranslateInfo host_info;
    std::unordered_map<VkFormat, Shader::TexturePixelFormat> pixel_formats;
};

} // namespace Vulkan

#endif
