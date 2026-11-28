// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace Common {

/// One logical processor as the host reports it.
struct LogicalProcessor {
    /// What the host's affinity call takes (a CPU set id on Windows).
    u32 id;
    /// The physical core: equal for SMT siblings.
    u32 core;
    /// Higher is faster. Equal on every processor of a CPU that is not hybrid.
    u8 efficiency_class;
};

/// Physical cores of the fastest class needed before the GPU threads are pinned.
constexpr std::size_t MinCoresForGpuThreadPinning = 6;

/// A CPU with exactly this many physical cores, all of one class and each with two SMT siblings
/// (4 cores, 8 threads, as the Steam Deck), gets a smaller plan: only the back gets a whole core,
/// and every other thread of the process is kept off it (see ReserveGpuBackCore in thread.h).
constexpr std::size_t CoresForGpuBackCorePinning = 4;

/// Logical processor ids for each pinned role. All three are empty when the host has fewer than
/// MinCoresForGpuThreadPinning performance cores and does not get the four-core plan.
struct GpuThreadPinning {
    std::size_t performance_cores{};
    std::vector<u32> front;
    std::vector<u32> back;
    std::vector<u32> guest;
    /// Set by the four-core plan: only the back is pinned, and while it runs every other thread
    /// of the process is kept off its core (ReserveGpuBackCore). front and guest list those other
    /// processors; on Linux they are not pinned on this plan, and Windows skips it.
    bool back_core_reserved{};
};

/// The split GPU thread's front and back each get a whole physical performance core, so no SMT
/// sibling shares it, and the guest cores get the remaining performance cores. Core 0 takes most
/// interrupts, so the GPU threads get the next two. On a CPU of CoresForGpuBackCorePinning cores
/// with SMT, only the back gets a core (the second one), and front and guest get the rest.
[[nodiscard]] GpuThreadPinning PlanGpuThreadPinning(std::span<const LogicalProcessor> processors);

/// The CPUs of a Linux sysfs CPU list such as "0-3,8,10-11", in order. Empty for an empty or
/// malformed list.
[[nodiscard]] std::vector<u32> ParseCpuList(std::string_view list);

} // namespace Common
