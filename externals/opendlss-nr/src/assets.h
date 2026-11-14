// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace nr {
// Generated from the open-source kernels at build time; contains no model weights.
std::span<const std::byte> FindAsset(std::string_view path);
std::span<const std::byte> RequireAsset(std::string_view path);
}
