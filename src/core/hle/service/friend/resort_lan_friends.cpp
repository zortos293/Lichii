// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <string_view>

#include "common/game_presets.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/hle/service/friend/resort_doc_bus.h"
#include "core/hle/service/friend/resort_lan_friends.h"
#include "network/network.h"
#include "network/room_member.h"

namespace Service::Friend {

namespace {

constexpr std::string_view HostingTag = " [hosting Friend Lobby]";
constexpr std::array<u8, 0x10> HostingOnUid{0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5e, 0xed,
                                            0x00, 0x00, 0x00, 0x00, 0x54, 0x53, 0x4f, 0x48};
constexpr std::array<u8, 0x10> HostingOffUid{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5e, 0xed,
                                             0x00, 0x00, 0x00, 0x00, 0x54, 0x53, 0x4f, 0x48};

bool IsHostingVersion(std::string_view version) {
    return version.size() >= HostingTag.size() &&
           version.substr(version.size() - HostingTag.size()) == HostingTag;
}

constexpr s64 LobbyLeaveDelayMs = 2000;

std::atomic<u32> g_unicast_target{0};
std::atomic<s64> g_unicast_time{0};
std::atomic<u32> g_last_unicast_ip{0};
std::atomic<s64> g_last_unicast_time{0};
std::atomic<s64> g_lobby_leave_due{0};

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string StripHostingTag(const std::string& version) {
    if (!IsHostingVersion(version)) {
        return version;
    }
    return version.substr(0, version.size() - HostingTag.size());
}

} // Anonymous namespace

bool ResortLanFriendsActive() {
    return Settings::GetCurrentProgramID() == Settings::SwitchSportsResortProgramId;
}

bool HandleResortHostingSignal(const Common::UUID& uid) {
    if (!ResortLanFriendsActive()) {
        return false;
    }
    const bool hosting_on = uid.uuid == HostingOnUid;
    if (!hosting_on && uid.uuid != HostingOffUid) {
        return false;
    }

    if (auto room_member = Network::GetRoomMember().lock()) {
        auto info = room_member->GetCurrentGameInfo();
        std::string version = StripHostingTag(info.version);
        if (hosting_on) {
            version += HostingTag;
        }
        if (info.id == Settings::SwitchSportsResortProgramId && version != info.version) {
            info.version = std::move(version);
            room_member->SendGameInfo(info);
        }
    }

    LOG_INFO(Service_Friend, "Resort Friend Lobby hosting {} (game signal)",
             hosting_on ? "ON" : "OFF");
    if (hosting_on) {
        g_lobby_leave_due.store(0, std::memory_order_release);
        const auto status = ResortBus::EnterResortLobby(0);
        if (status != ResortBus::Status::Ok) {
            LOG_WARNING(Service_Friend, "ResortBus could not open the host lobby: status {}",
                        static_cast<u32>(status));
        }
    } else if (ResortBus::GetResortDocBus().IsActive()) {
        g_lobby_leave_due.store(NowMs() + LobbyLeaveDelayMs, std::memory_order_release);
    }
    return true;
}

bool TakeDueResortLobbyLeave() {
    const s64 due = g_lobby_leave_due.load(std::memory_order_acquire);
    if (due == 0 || NowMs() < due) {
        return false;
    }
    s64 expected = due;
    return g_lobby_leave_due.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
}

bool RecentResortUnicastTo(u32 ip, s64 within_ms) {
    return ip != 0 && g_last_unicast_ip.load(std::memory_order_acquire) == ip &&
           NowMs() - g_last_unicast_time.load(std::memory_order_acquire) <= within_ms;
}

bool IsResortHosting() {
    const auto room_member = Network::GetRoomMember().lock();
    return room_member && IsHostingVersion(room_member->GetCurrentGameInfo().version);
}

std::vector<ResortRoomFriend> CollectResortRoomHosts() {
    std::vector<ResortRoomFriend> hosts;
    auto room_member = Network::GetRoomMember().lock();
    if (!room_member || !room_member->IsConnected()) {
        return hosts;
    }

    const auto own_ip = room_member->GetFakeIpAddress();
    for (const auto& member : room_member->GetMemberInformation()) {
        if (member.fake_ip == own_ip || member.nickname.empty() ||
            member.game_info.id != Settings::SwitchSportsResortProgramId ||
            !IsHostingVersion(member.game_info.version)) {
            continue;
        }
        hosts.push_back({member.nickname, member.fake_ip});
    }

    std::ranges::sort(hosts, {}, &ResortRoomFriend::nickname);
    return hosts;
}

void NoteResortUnicastTarget(const Network::IPv4Address& ip) {
    const u32 value = static_cast<u32>(ip[0]) << 24 | static_cast<u32>(ip[1]) << 16 |
                      static_cast<u32>(ip[2]) << 8 | static_cast<u32>(ip[3]);
    const s64 now = NowMs();
    g_unicast_target.store(value, std::memory_order_relaxed);
    g_unicast_time.store(now, std::memory_order_release);
    g_last_unicast_ip.store(value, std::memory_order_relaxed);
    g_last_unicast_time.store(now, std::memory_order_release);
}

std::optional<Network::IPv4Address> TakeResortUnicastTarget() {
    const u32 ip = g_unicast_target.exchange(0, std::memory_order_acquire);
    if (ip == 0 || NowMs() - g_unicast_time.load(std::memory_order_acquire) > 10000) {
        return std::nullopt;
    }
    return Network::IPv4Address{static_cast<u8>(ip >> 24), static_cast<u8>(ip >> 16),
                                static_cast<u8>(ip >> 8), static_cast<u8>(ip)};
}

u64 ResortFriendAccountId(const Network::IPv4Address& ip) {
    return 0xEDE5000100000000ULL | static_cast<u64>(ip[0]) << 24 | static_cast<u64>(ip[1]) << 16 |
           static_cast<u64>(ip[2]) << 8 | static_cast<u64>(ip[3]);
}

} // namespace Service::Friend
