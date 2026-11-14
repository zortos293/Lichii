// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include "common/common_types.h"

namespace Vulkan::Ngx {

inline constexpr const char *ModelFilename = "libnvidia-ngx-dlss.so.310.9.16";
inline constexpr const char *ModelSha256 =
    "30982c937afed85c96b8d313323de9a650a2eec6de1bb0f714b21750f4ce98f6";
inline constexpr const char *SdkRevision = "9981c6350dc58c0ce21f024fd65e31e851c31689";

[[nodiscard]] std::filesystem::path UserFolder();
[[nodiscard]] std::filesystem::path FindModel();

struct RuntimeStatus {
    bool available{};
    u64 evaluations{};
    std::string error;
};
[[nodiscard]] RuntimeStatus GetStatus();
void ReportFailure(std::string reason);
void ReportEvaluation();

} // namespace Vulkan::Ngx
