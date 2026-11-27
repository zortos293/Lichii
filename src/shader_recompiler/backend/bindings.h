// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"

namespace Shader::Backend {

struct Bindings {
    u32 unified{};
    u32 uniform_buffer{};
    u32 storage_buffer{};
    u32 texture{};
    u32 image{};
    u32 texture_scaling_index{};
    u32 image_scaling_index{};
    /// Push-constant offset of the render area: 0 its own block, else a member at that offset,
    /// in the rescaling block when the stage uses both (NVN direct mode's layout)
    u32 render_area_offset{};
    /// The descriptor set and bindings of the texture, sampler and image pools, arrays indexed by
    /// texture and sampler ID, for textures and images read from them (TextureDescriptor::is_pool,
    /// ImageDescriptor::is_pool)
    u32 pool_set{};
    u32 pool_texture_binding{};
    u32 pool_sampler_binding{};
    u32 pool_image_binding{};
};

} // namespace Shader::Backend
