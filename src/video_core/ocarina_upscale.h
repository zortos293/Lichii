// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string_view>

namespace VideoCommon {

enum class OcarinaUpscaleBackend { Native, Fsr, Dlss, Xess };

/// render_scaled: a DLSS render-scale patch shrank the scene to a size FSR 3.1 takes, so FSR 3.1
/// reconstructs it when DLSS can't run.
constexpr OcarinaUpscaleBackend SelectOcarinaUpscaler(bool fsr_enabled, bool dlss_enabled,
                                                     bool dlss_available, bool render_scaled,
                                                     std::string_view override,
                                                     bool xess_enabled = false,
                                                     bool xess_available = false) {
    if (override == "native") {
        return OcarinaUpscaleBackend::Native;
    }
    if (override == "fsr") {
        return OcarinaUpscaleBackend::Fsr;
    }
    if (override == "xess") {
        return xess_available ? OcarinaUpscaleBackend::Xess : OcarinaUpscaleBackend::Fsr;
    }
    if (override == "dlss") {
        return dlss_available ? OcarinaUpscaleBackend::Dlss
                              : (fsr_enabled || render_scaled ? OcarinaUpscaleBackend::Fsr
                                                              : OcarinaUpscaleBackend::Native);
    }
    if (dlss_enabled && dlss_available) {
        return OcarinaUpscaleBackend::Dlss;
    }
    if (xess_enabled) {
        return xess_available ? OcarinaUpscaleBackend::Xess : OcarinaUpscaleBackend::Fsr;
    }
    return fsr_enabled || render_scaled ? OcarinaUpscaleBackend::Fsr
                                        : OcarinaUpscaleBackend::Native;
}

/// Texture LOD bias added to Ocarina's mipmapped samplers while FSR upscales. AMD recommends
/// log2(render / output) - 1: -1.58 for docked 1440p -> 4K.
constexpr float OcarinaFsrLodBias(OcarinaUpscaleBackend backend) {
    return backend == OcarinaUpscaleBackend::Fsr ? -1.58f : 0.0f;
}

/// Whether a sampler with that bias keeps it for an image: only docked (handheld isn't measured),
/// not for images the GPU wrote (render targets would read a mip too sharp), and not once the
/// upscaler failed this session (the game's own pass runs again).
constexpr bool KeepOcarinaFsrLodBias(bool docked, bool upscaler_failed, bool gpu_written) {
    return docked && !upscaler_failed && !gpu_written;
}

} // namespace VideoCommon
