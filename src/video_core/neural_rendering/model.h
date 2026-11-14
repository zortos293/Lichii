// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace VideoCore::NeuralRendering {

inline constexpr std::string_view ModelVersion = "310.8.0";
inline constexpr std::string_view ResourceSha256 =
    "836f445d06ecd2e59bb9f17b84b91c143396fd76ccda1c9dc7fe81d5edd548f4";
inline constexpr std::string_view PackedSha256 =
    "37a293b5b781b9351a8b4b5fbeb2ff68fbd8a84891378950812ff3b99d161116";

using Bytes = std::span<const std::uint8_t>;
using TensorMap = std::map<std::string, Bytes>;

// These parsers never execute the DLL. Returned spans borrow their input's storage.
// Malformed input raises std::runtime_error before any output is published.
[[nodiscard]] Bytes ReadWeightsResource(Bytes dll);
[[nodiscard]] TensorMap ReadPackedWeights(Bytes resource);

// Write the verified model into a new directory. Existing installations are left intact.
// The caller can select this directory only after the operation succeeds.
void ImportModel(const std::filesystem::path& dll, const std::filesystem::path& destination);
// Reads and verifies the complete installation. Run once when importing or starting the backend,
// never from a UI refresh timer.
[[nodiscard]] bool IsInstalled(const std::filesystem::path& directory);

} // namespace VideoCore::NeuralRendering
