// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Vulkan {

// One descriptor set per graphics stage, including holes for absent stages.
constexpr u32 GraphicsPipelineLibraryDescriptorSetCount = 5;

// Only fragment inputs carry Flat or NoPerspective, so the libraries also need the driver to
// accept outputs of the last pre-rasterization stage without matching interpolation decorations.
constexpr bool CanUseGraphicsPipelineLibraries(bool feature, bool fast_linking, u32 max_sets,
                                               bool independent_interpolation) {
    return feature && fast_linking && max_sets >= GraphicsPipelineLibraryDescriptorSetCount &&
           independent_interpolation;
}

} // namespace Vulkan
