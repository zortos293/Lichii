// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stop_token>

#include "common/common_types.h"

// The end of the guest that NativeCloudSync.beginGuest began (frontend_common/cloud_sync), for the
// emulation run in native.cpp.
namespace AndroidCloudSync {

// The guest loaded program_id, which hasn't run yet: its saves are pushed once it stops. After a
// program change to another program id, the previous program's saves are pushed and the next one's
// pulled first, until stop.
void GuestLoaded(u64 program_id, std::stop_token stop);

// The guest stopped, other than for a program change: ends BeginGuest, and has the app push the
// saves if it loaded.
void GuestStopped();

} // namespace AndroidCloudSync
