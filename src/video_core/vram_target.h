// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/settings_enums.h"

namespace VideoCommon {

/// Device memory the emulator aims to stay under for `mode`, given the driver's budget for the
/// process when the device is created (0 without VK_EXT_memory_budget). 0 means no target: the
/// caches follow the driver's budget alone. Conservative takes 85% of the DXGI device budget,
/// leaving the rest to the desktop and other programs (~6.1 GB on an 8 GB card with 7.1 GB budget).
/// Aggressive removes the soft target cap.
[[nodiscard]] constexpr u64 VramTarget(Settings::VramUsageMode mode, u64 device_budget) {
    switch (mode) {
    case Settings::VramUsageMode::Aggressive:
        return 0;
    case Settings::VramUsageMode::Conservative:
    default:
        return device_budget * 85 / 100;
    }
}

} // namespace VideoCommon
