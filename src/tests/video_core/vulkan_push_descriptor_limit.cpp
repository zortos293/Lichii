// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_device.h"

using Vulkan::MoltenVKMaxPushDescriptors;
using Vulkan::UsableMaxPushDescriptors;

TEST_CASE("Push descriptors: MoltenVK's reported limit is held to 16", "[video_core]") {
    // maxPerStageResources on Apple GPUs, which MoltenVK reports
    REQUIRE(UsableMaxPushDescriptors(VK_DRIVER_ID_MOLTENVK, 287) == MoltenVKMaxPushDescriptors);
    REQUIRE(MoltenVKMaxPushDescriptors == 16);
}

TEST_CASE("Push descriptors: a MoltenVK limit at or below 16 is kept", "[video_core]") {
    // Ryujinx's MoltenVK fork reports 16 already
    REQUIRE(UsableMaxPushDescriptors(VK_DRIVER_ID_MOLTENVK, 16) == 16);
    REQUIRE(UsableMaxPushDescriptors(VK_DRIVER_ID_MOLTENVK, 8) == 8);
}

TEST_CASE("Push descriptors: other drivers keep their limit", "[video_core]") {
    REQUIRE(UsableMaxPushDescriptors(VK_DRIVER_ID_NVIDIA_PROPRIETARY, 32) == 32);
    REQUIRE(UsableMaxPushDescriptors(VK_DRIVER_ID_MESA_RADV, 4096) == 4096);
    REQUIRE(UsableMaxPushDescriptors(VK_DRIVER_ID_MESA_HONEYKRISP, 287) == 287);
}
