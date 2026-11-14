// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// A Switch 2 (native NVN) program as the emulator lowers it, from a game's files: what
// nx2-precompile (video_core/nx2_precompile) scans each extracted program with.

#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/stage.h"

namespace VideoCommon::Nx2Lowering {

/// A native program as the runtime lowers it: program header, lowered words with scheduling slots,
/// padding and the native address map (GenericEnvironment::LowerNativeCode).
struct Lowered {
    std::vector<u64> code;
    u32 texture_bound{~0U};
    u64 hash{};       ///< the shader hash pipeline keys use
    u64 native_key{};
};

/// Captures and lowers a program from its control section and code area, as
/// GenericEnvironment::LowerNativeCode does. Empty, with the reason in `error`, when it doesn't.
[[nodiscard]] std::optional<Lowered> Lower(Shader::Stage stage, std::span<const u8> control,
                                           std::span<const u8> code, std::string& error);

/// GenericEnvironment's native shader hash of lowered code.
[[nodiscard]] u64 NativeShaderHash(std::span<const u64> code);

} // namespace VideoCommon::Nx2Lowering
