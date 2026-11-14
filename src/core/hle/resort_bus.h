// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Kernel {
class KProcess;
}

namespace Core::HLE::ResortBus {

constexpr u32 BusSvc = 0x7A1C;

enum class Op : u32 {
    Publish = 1,
    Poll = 2,
    Query = 3,
    RaiseFilter = 9,
    StationWait = 11,
};

constexpr s32 ResultInactive = -1;
constexpr s32 ResultBadOp = -2;
constexpr s32 ResultBadBuffer = -3;
constexpr s32 ResultTooSmall = -4;

s32 HandleSvc(Core::System& system, Kernel::KProcess& process, std::span<const u64, 8> args);

} // namespace Core::HLE::ResortBus
