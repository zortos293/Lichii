// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

#include "common/common_types.h"
#include "common/settings.h"

/// Switches for the texture cache's write-backs of GPU-written overlaps. Each is on for The
/// Duskbloods (where it was tested) when unset; set it to 1 to turn it on for every title, or 0 to
/// turn it off.
namespace VideoCommon::WritebackSwitches {

inline constexpr u64 DuskbloodsNetworkTestProgramId = 0x0400D1D02399A000ULL;

/// A switch's setting: 1 on for every title, 0 off, -1 unset.
[[nodiscard]] inline int SwitchSetting(const char* name) {
    const char* const value = std::getenv(name);
    return value == nullptr ? -1 : value[0] == '1' ? 1 : value[0] == '0' ? 0 : -1;
}

[[nodiscard]] inline bool DuskbloodsDefault(int setting) {
    return setting >= 0 ? setting == 1
                        : Settings::GetCurrentProgramID() == DuskbloodsNetworkTestProgramId;
}

/// Whether a switch with this setting is on, for The Duskbloods or another title.
[[nodiscard]] constexpr bool SwitchOn(int setting, bool duskbloods) {
    return setting >= 0 ? setting == 1 : duskbloods;
}

/// Whether the collector's (EDEN_NX2_GC_DOWNLOAD_OVERLAPS) or joins' (EDEN_NX2_JOIN_WRITEBACK)
/// write-backs are on, from their settings. Either one queues write-backs, so whatever else
/// writes an image to guest memory has to be kept from them and noted while either is on.
[[nodiscard]] constexpr bool AnyWritebackOn(int collector_setting, int join_setting,
                                            bool duskbloods) {
    return SwitchOn(collector_setting, duskbloods) || SwitchOn(join_setting, duskbloods);
}

[[nodiscard]] inline bool AnyWritebackOn() {
    static const int collector_setting = SwitchSetting("EDEN_NX2_GC_DOWNLOAD_OVERLAPS");
    static const int join_setting = SwitchSetting("EDEN_NX2_JOIN_WRITEBACK");
    return AnyWritebackOn(collector_setting, join_setting,
                          Settings::GetCurrentProgramID() == DuskbloodsNetworkTestProgramId);
}

/// EDEN_NX2_UPLOAD_STALE: an image made over pages a dropped GPU-written image left stale loads
/// guest memory like any other, instead of keeping whatever its host memory held before. The
/// Duskbloods samples such images (small per-character render targets), which drew outfits,
/// blades and enemies in rainbow noise.
[[nodiscard]] inline bool UploadStale() {
    static const int setting = SwitchSetting("EDEN_NX2_UPLOAD_STALE");
    return DuskbloodsDefault(setting);
}

} // namespace VideoCommon::WritebackSwitches
