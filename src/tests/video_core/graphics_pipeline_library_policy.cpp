// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/graphics_pipeline_library_policy.h"

TEST_CASE("Graphics libraries require five descriptor sets and fast linking",
          "[video_core][gpl]") {
    using Vulkan::CanUseGraphicsPipelineLibraries;
    CHECK_FALSE(CanUseGraphicsPipelineLibraries(true, true, 4, true));
    CHECK(CanUseGraphicsPipelineLibraries(true, true, 5, true));
    CHECK(CanUseGraphicsPipelineLibraries(true, true, 32, true));
    CHECK_FALSE(CanUseGraphicsPipelineLibraries(false, true, 32, true));
    CHECK_FALSE(CanUseGraphicsPipelineLibraries(true, false, 32, true));
}

TEST_CASE("Graphics libraries require independent interpolation decoration",
          "[video_core][gpl]") {
    using Vulkan::CanUseGraphicsPipelineLibraries;
    CHECK_FALSE(CanUseGraphicsPipelineLibraries(true, true, 32, false));
}
