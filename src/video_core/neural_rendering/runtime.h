// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace VideoCore::NeuralRendering {
struct RuntimeStatus {
    bool ready{};
    std::uint64_t submitted_frames{};
    std::string error;
};

[[nodiscard]] std::filesystem::path UserFolder();
[[nodiscard]] RuntimeStatus GetStatus();
void ResetStatus();
void ReportReady();
/// Mark a user-disabled backend inactive while retaining its submitted-frame counter.
void ReportInactive();
void ReportFailure(std::string error);
void ReportFrame();
} // namespace VideoCore::NeuralRendering
