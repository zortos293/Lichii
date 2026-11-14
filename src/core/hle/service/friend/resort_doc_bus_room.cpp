// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <mutex>

#include "common/game_presets.h"
#include "common/logging.h"
#include "common/random.h"
#include "core/hle/service/friend/resort_doc_bus.h"
#include "core/hle/service/friend/resort_lan_friends.h"
#include "network/network.h"
#include "network/room_member.h"

namespace Service::Friend::ResortBus {

namespace {

u32 ToU32(const Network::IPv4Address& ip) {
    return static_cast<u32>(ip[0]) << 24 | static_cast<u32>(ip[1]) << 16 |
           static_cast<u32>(ip[2]) << 8 | static_cast<u32>(ip[3]);
}

Network::IPv4Address ToAddress(u32 ip) {
    return {static_cast<u8>(ip >> 24), static_cast<u8>(ip >> 16), static_cast<u8>(ip >> 8),
            static_cast<u8>(ip)};
}

bool IsJoined(const Network::RoomMember& member) {
    const auto state = member.GetState();
    return state == Network::RoomMember::State::Joined ||
           state == Network::RoomMember::State::Moderator;
}

class RoomLink final : public Link {
public:
    bool Connected() const override {
        const auto member = Network::GetRoomMember().lock();
        return member && IsJoined(*member);
    }

    u32 OwnIp() const override {
        const auto member = Network::GetRoomMember().lock();
        return member && IsJoined(*member) ? ToU32(member->GetFakeIpAddress()) : 0;
    }

    std::string Nickname() const override {
        const auto member = Network::GetRoomMember().lock();
        return member ? member->GetNickname() : std::string{};
    }

    void Send(std::vector<u8>&& message, std::optional<u32> target_ip) override {
        const auto member = Network::GetRoomMember().lock();
        if (!member || !IsJoined(*member)) {
            return;
        }
        Network::ProxyPacket packet{};
        packet.local_endpoint = {Network::Domain::INET, member->GetFakeIpAddress(), WirePort};
        packet.remote_endpoint = {
            Network::Domain::INET,
            target_ip ? ToAddress(*target_ip) : Network::IPv4Address{255, 255, 255, 255}, WirePort};
        packet.protocol = static_cast<Network::Protocol>(WireProtocol);
        packet.broadcast = !target_ip.has_value();
        packet.data = std::move(message);
        member->SendProxyPacket(packet);
    }
};

std::vector<RoomMemberView> CollectMembers(const Network::RoomMember& member) {
    std::vector<RoomMemberView> views;
    for (const auto& info : member.GetMemberInformation()) {
        views.push_back({ToU32(info.fake_ip), info.game_info.id, info.nickname});
    }
    return views;
}

struct Runtime {
    std::mutex lifecycle;
    RoomLink link;
    DocBus bus{link, Settings::SwitchSportsResortProgramId};
    std::weak_ptr<Network::RoomMember> bound;
    Network::RoomMember::CallbackHandle<Network::ProxyPacket> packet_handle;
    Network::RoomMember::CallbackHandle<Network::RoomInformation> info_handle;
    Network::RoomMember::CallbackHandle<Network::RoomMember::State> state_handle;
};

Runtime& GetRuntime() {
    static Runtime runtime;
    return runtime;
}

} // Anonymous namespace

DocBus& GetResortDocBus() {
    return GetRuntime().bus;
}

bool StartResortDocBus() {
    if (!ResortLanFriendsActive()) {
        return false;
    }
    auto& runtime = GetRuntime();
    std::scoped_lock lock{runtime.lifecycle};
    if (runtime.bus.IsActive()) {
        return true;
    }
    std::vector<RoomMemberView> members;
    if (const auto member = Network::GetRoomMember().lock()) {
        runtime.bound = member;
        runtime.packet_handle =
            member->BindOnProxyPacketReceived([&runtime](const Network::ProxyPacket& packet) {
                if (static_cast<u8>(packet.protocol) != WireProtocol ||
                    packet.remote_endpoint.portno != WirePort) {
                    return;
                }
                runtime.bus.OnDatagram(ToU32(packet.local_endpoint.ip), packet.data);
            });
        runtime.info_handle =
            member->BindOnRoomInformationChanged([&runtime](const Network::RoomInformation&) {
                if (const auto current = runtime.bound.lock()) {
                    runtime.bus.OnRoomMembers(CollectMembers(*current));
                }
            });
        runtime.state_handle =
            member->BindOnStateChanged([&runtime](const Network::RoomMember::State& state) {
                runtime.bus.OnConnection(state == Network::RoomMember::State::Joined ||
                                         state == Network::RoomMember::State::Moderator);
            });
        if (IsJoined(*member)) {
            members = CollectMembers(*member);
        }
    } else {
        LOG_WARNING(Service_Friend, "ResortBus starting without a room member");
    }
    runtime.bus.Start(Common::Random::Random64(0) | 1, members);
    return true;
}

void StopResortDocBus() {
    auto& runtime = GetRuntime();
    std::scoped_lock lock{runtime.lifecycle};
    runtime.bus.Stop();
    if (const auto member = runtime.bound.lock()) {
        if (runtime.packet_handle) {
            member->Unbind(runtime.packet_handle);
        }
        if (runtime.info_handle) {
            member->Unbind(runtime.info_handle);
        }
        if (runtime.state_handle) {
            member->Unbind(runtime.state_handle);
        }
    }
    runtime.packet_handle.reset();
    runtime.info_handle.reset();
    runtime.state_handle.reset();
    runtime.bound.reset();
}

std::optional<u64> ResolveResortLobbyScope() {
    const auto member = Network::GetRoomMember().lock();
    if (!member || !IsJoined(*member)) {
        return std::nullopt;
    }
    if (IsResortHosting()) {
        return LobbyScopeFromHostIp(ToU32(member->GetFakeIpAddress()));
    }
    const auto hosts = CollectResortRoomHosts();
    if (hosts.size() != 1) {
        return std::nullopt;
    }
    return LobbyScopeFromHostIp(ToU32(hosts.front().ip));
}

Status EnterResortLobby(u64 scope) {
    if (!StartResortDocBus()) {
        return Status::Inactive;
    }
    if (scope == 0) {
        const auto resolved = ResolveResortLobbyScope();
        if (!resolved) {
            LOG_WARNING(Service_Friend,
                        "ResortBus has no lobby to enter: no single Friend Lobby host in the room");
            return Status::NoLobby;
        }
        scope = *resolved;
    }
    return GetResortDocBus().EnterLobby(scope);
}

} // namespace Service::Friend::ResortBus
