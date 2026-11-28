// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

#include "common/common_types.h"

namespace Vulkan {

class Device;

/// Runs one workgroup of a compute shader, with full subgroups, on its own submission: `words` go
/// to a storage buffer at set 0 binding 0 and come back from it. For self-tests while the renderer
/// is being built, before the scheduler can flush and before anything else submits to the queue.
/// It waits for the dispatch without a bound: a GPU that cannot finish one workgroup runs nothing
/// else either. The shader must be valid with REQUIRE_FULL_SUBGROUPS (its x a multiple of the
/// subgroup size). Throws vk::Exception on a Vulkan error.
void RunComputeProbe(const Device& device, std::span<const u32> code, std::span<u32> words);

} // namespace Vulkan
