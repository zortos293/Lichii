// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/socket_types.h"
#include "common/uuid.h"

namespace Service::Friend {

struct ResortRoomFriend {
    std::string nickname;
    Network::IPv4Address ip;
};

bool ResortLanFriendsActive();

bool HandleResortHostingSignal(const Common::UUID& uid);

bool IsResortHosting();

std::vector<ResortRoomFriend> CollectResortRoomHosts();

u64 ResortFriendAccountId(const Network::IPv4Address& ip);

void NoteResortUnicastTarget(const Network::IPv4Address& ip);

std::optional<Network::IPv4Address> TakeResortUnicastTarget();

bool TakeDueResortLobbyLeave();

bool RecentResortUnicastTo(u32 ip, s64 within_ms);

} // namespace Service::Friend
