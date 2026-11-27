// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

#include "common/common_types.h"
#include "common/settings.h"

namespace VideoCommon::WritebackSwitches {

inline constexpr u64 DuskbloodsNetworkTestProgramId = 0x0400D1D02399A000ULL;
inline constexpr u64 KirbyForgottenLandNs2eProgramId = 0x04004D300C5AE001ULL;
inline constexpr u64 OcarinaOfTimeProgramId = 0x0400486024DEC000ULL;

/// Titles whose collector and join write-backs and stale-page uploads are on: they sample
/// GPU-written images the collector would drop, which otherwise show recycled host memory
/// (The Duskbloods' rainbow-noise characters, Kirby's garbled loading text, #2422).
[[nodiscard]] constexpr bool WritebackDefault(u64 program_id) {
    return program_id == DuskbloodsNetworkTestProgramId ||
           program_id == KirbyForgottenLandNs2eProgramId;
}

/// EDEN_NX2_OOT_STALE_WRITEBACK, Ocarina of Time only (#2056 test): 1 writes back the BadOverlap
/// images the collector drops that a later frame sampled after their last write (the pause
/// menu's item preview), 2 turns on the WritebackDefault set. Unset or anything else: off.
enum class OcarinaStaleWriteback : u8 { Off, Sampled, Default };

[[nodiscard]] constexpr OcarinaStaleWriteback ParseOcarinaStaleWriteback(const char* value) {
    if (value == nullptr || value[0] == '\0' || value[1] != '\0') {
        return OcarinaStaleWriteback::Off;
    }
    return value[0] == '1'   ? OcarinaStaleWriteback::Sampled
           : value[0] == '2' ? OcarinaStaleWriteback::Default
                             : OcarinaStaleWriteback::Off;
}

[[nodiscard]] inline OcarinaStaleWriteback RequestedOcarinaStaleWriteback() {
    static const OcarinaStaleWriteback mode =
        ParseOcarinaStaleWriteback(std::getenv("EDEN_NX2_OOT_STALE_WRITEBACK"));
    return mode;
}

[[nodiscard]] inline OcarinaStaleWriteback OcarinaStaleWritebackMode() {
    return Settings::GetCurrentProgramID() == OcarinaOfTimeProgramId
               ? RequestedOcarinaStaleWriteback()
               : OcarinaStaleWriteback::Off;
}

[[nodiscard]] inline bool WritebackDefault() {
    return WritebackDefault(Settings::GetCurrentProgramID()) ||
           OcarinaStaleWritebackMode() == OcarinaStaleWriteback::Default;
}

/// The collector writes back only the BadOverlap images sampled after their last write.
[[nodiscard]] inline bool SampledWriteback() {
    return OcarinaStaleWritebackMode() == OcarinaStaleWriteback::Sampled;
}

[[nodiscard]] inline bool AnyWritebackOn() {
    return WritebackDefault() || SampledWriteback();
}

[[nodiscard]] inline bool UploadStale() {
    return WritebackDefault();
}

} // namespace VideoCommon::WritebackSwitches
