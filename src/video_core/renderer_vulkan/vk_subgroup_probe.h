// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Vulkan {

class Device;

/// Whether the device's compute subgroups are 32-lane warps numbered in invocation order, as the
/// staged software HMMA needs and as Apple GPUs were measured to run them (Device::
/// AssumesAppleComputeSubgroupsAreGuestWarps): with full subgroups, in every workgroup the probe
/// runs (Shader::Backend::SPIRV::IsSubgroupProbeShape, within the device's limits), every
/// invocation i must see SubgroupId i / 32 and lane i % 32 (and SubgroupSize 32, a constant on
/// MoltenVK). The staged HMMA then takes only those workgroups. Needs computeFullSubgroups and
/// SPIR-V 1.3.
///
/// It runs one workgroup of a straight-line shader: it checks how the device numbers subgroups,
/// not every way the driver could pack a shader with shared memory, barriers and branches.
[[nodiscard]] bool ProbeComputeSubgroupsAreGuestWarps(const Device& device);

} // namespace Vulkan
