// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>

#include "common/common_types.h"

#include "dynarmic/frontend/A64/a64_location_descriptor.h"

namespace Dynarmic::Backend::Arm64 {

struct A64JitState {
    std::array<u64, 31> reg{};
    u64 sp = 0;
    u64 pc = 0;

    u32 cpsr_nzcv = 0;

    alignas(16) std::array<u64, 64> vec{};

    u32 exclusive_state = 0;

    u32 fpsr = 0;
    u32 fpcr = 0;

    // This Jit's own objects, read by emitted code so one block serves every Jit of a code cache.
    u64 callbacks = 0;  // A64::UserCallbacks*
    u64 user_config = 0;  // const A64::UserConfig*
    u64 jit_impl = 0;  // The dispatcher's block lookup argument
    u64 tpidr_el0 = 0;  // u64*
    u64 tpidrro_el0 = 0;  // const u64*

    IR::LocationDescriptor GetLocationDescriptor() const {
        const u64 fpcr_u64 = static_cast<u64>(fpcr & A64::LocationDescriptor::fpcr_mask) << A64::LocationDescriptor::fpcr_shift;
        const u64 pc_u64 = pc & A64::LocationDescriptor::pc_mask;
        return IR::LocationDescriptor{pc_u64 | fpcr_u64};
    }
};

}  // namespace Dynarmic::Backend::Arm64
