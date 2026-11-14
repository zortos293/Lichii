// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <vector>

#include "common/common_types.h"

namespace Core::Memory {
class Memory;
}

namespace Service::Friend::ResortBus {

s32 GameQuery(std::span<const u8> in);
s32 GamePublish(Core::Memory::Memory& memory, std::span<const u8> record);
s32 GamePoll(u64 out_address, u32 capacity, std::vector<u8>& out);
bool JoinedHuddleClosed();
void ResetGameAdapter();

} // namespace Service::Friend::ResortBus
