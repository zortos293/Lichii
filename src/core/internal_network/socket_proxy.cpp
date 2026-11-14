// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>

#include "common/assert.h"
#include "common/logging.h"
#include "common/zstd_compression.h"
#include "core/hle/service/friend/resort_lan_friends.h"
#include "core/internal_network/network.h"
#include "core/internal_network/network_interface.h"
#include "core/internal_network/socket_proxy.h"
#include "network/network.h"

#ifdef __unix__
#include <sys/socket.h>
#endif

namespace Network {

namespace {

u16 InternetChecksum(std::span<const u8> data) {
    u32 sum = 0;
    for (std::size_t i = 0; i + 1 < data.size(); i += 2) {
        sum += static_cast<u32>(data[i] << 8 | data[i + 1]);
    }
    if (data.size() % 2 != 0) {
        sum += static_cast<u32>(data.back() << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return static_cast<u16>(~sum);
}

std::vector<u8> MakeIcmpEchoReply(std::span<const u8> request, const IPv4Address& from,
                                  const IPv4Address& to) {
    constexpr std::size_t IpHeaderSize = 20;
    std::vector<u8> packet(IpHeaderSize + request.size());
    const auto total_length = static_cast<u16>(packet.size());
    packet[0] = 0x45;
    packet[2] = static_cast<u8>(total_length >> 8);
    packet[3] = static_cast<u8>(total_length);
    packet[8] = 64;
    packet[9] = 1;
    std::copy(from.begin(), from.end(), packet.begin() + 12);
    std::copy(to.begin(), to.end(), packet.begin() + 16);
    const u16 header_checksum = InternetChecksum(std::span(packet).first(IpHeaderSize));
    packet[10] = static_cast<u8>(header_checksum >> 8);
    packet[11] = static_cast<u8>(header_checksum);

    std::copy(request.begin(), request.end(), packet.begin() + IpHeaderSize);
    packet[IpHeaderSize] = 0;
    packet[IpHeaderSize + 1] = 0;
    packet[IpHeaderSize + 2] = 0;
    packet[IpHeaderSize + 3] = 0;
    const u16 icmp_checksum = InternetChecksum(std::span(packet).subspan(IpHeaderSize));
    packet[IpHeaderSize + 2] = static_cast<u8>(icmp_checksum >> 8);
    packet[IpHeaderSize + 3] = static_cast<u8>(icmp_checksum);
    return packet;
}

} // Anonymous namespace

ProxySocket::ProxySocket() noexcept {}

ProxySocket::~ProxySocket() {
    if (fd == INVALID_SOCKET) {
        return;
    }
    fd = INVALID_SOCKET;
}

bool ProxySocket::MatchesPacket(const ProxyPacket& packet) const {
    if (protocol != packet.protocol || local_endpoint.portno != packet.remote_endpoint.portno ||
        closed) {
        return false;
    }
    if (!broadcast && packet.broadcast) {
        LOG_INFO(Network, "Received broadcast packet, but not configured for broadcast mode");
        return false;
    }
    return true;
}

bool ProxySocket::AcceptsProxyPacket(const ProxyPacket& packet) {
    std::lock_guard guard(packets_mutex);
    return MatchesPacket(packet);
}

void ProxySocket::HandleProxyPacket(const ProxyPacket& packet) {
    {
        // Runs on the room thread, while the game thread may bind, close or reconfigure the socket.
        // Check again: the socket may have changed since AcceptsProxyPacket.
        std::lock_guard guard(packets_mutex);
        if (!MatchesPacket(packet)) {
            return;
        }
        received_packets.push(packet);
    }
    // A MSG_PEEK reader leaves the packet queued, so every waiter must see it
    packets_cv.notify_all();
}

std::optional<PollEvents> ProxySocket::EmulatedPoll(PollEvents events) {
    std::lock_guard guard(packets_mutex);
    PollEvents revents{};
    // A queued packet reads at once. So does a closed socket, whose reads fail, and a stream,
    // whose reads report its end while nothing is queued (see Recv).
    if (closed || !received_packets.empty() || socket_type == Type::STREAM) {
        revents |= events & (PollEvents::In | PollEvents::RdNorm);
    }
    // Sends go to the room without waiting.
    revents |= events & PollEvents::Out;
    return revents;
}

template <typename T>
Errno ProxySocket::SetSockOpt(SOCKET fd_, int option, T value) {
    LOG_DEBUG(Network, "(STUBBED) called");
    return Errno::SUCCESS;
}

Errno ProxySocket::Initialize(Domain domain, Type type, Protocol socket_protocol) {
    {
        std::lock_guard guard(packets_mutex);
        socket_type = type;
        protocol = socket_protocol;
    }
    SetSockOpt(fd, SO_TYPE, type);

    return Errno::SUCCESS;
}

std::pair<ProxySocket::AcceptResult, Errno> ProxySocket::Accept() {
    LOG_WARNING(Network, "(STUBBED) called");
    return {AcceptResult{}, Errno::SUCCESS};
}

Errno ProxySocket::Connect(SockAddrIn addr_in) {
    LOG_WARNING(Network, "(STUBBED) called");
    return Errno::SUCCESS;
}

std::pair<SockAddrIn, Errno> ProxySocket::GetPeerName() {
    LOG_WARNING(Network, "(STUBBED) called");
    return {SockAddrIn{}, Errno::SUCCESS};
}

std::pair<SockAddrIn, Errno> ProxySocket::GetSockName() {
    LOG_WARNING(Network, "(STUBBED) called");
    return {SockAddrIn{}, Errno::SUCCESS};
}

Errno ProxySocket::Bind(SockAddrIn addr) {
    std::lock_guard guard(packets_mutex);
    if (is_bound) {
        LOG_WARNING(Network, "Rebinding Socket is unimplemented!");
        return Errno::SUCCESS;
    }
    local_endpoint = addr;
    is_bound = true;

    return Errno::SUCCESS;
}

Errno ProxySocket::Listen(s32 backlog) {
    LOG_WARNING(Network, "(STUBBED) called");
    return Errno::SUCCESS;
}

Errno ProxySocket::Shutdown(ShutdownHow how) {
    LOG_WARNING(Network, "(STUBBED) called");
    return Errno::SUCCESS;
}

std::pair<s32, Errno> ProxySocket::Recv(int flags, std::span<u8> message) {
    u32 msg_flags = 0;
    return RecvMsg(flags, message, nullptr, msg_flags);
}

std::pair<s32, Errno> ProxySocket::RecvFrom(int flags, std::span<u8> message, SockAddrIn* addr) {
    u32 msg_flags = 0;
    return Receive(flags, message, addr, msg_flags);
}

std::pair<s32, Errno> ProxySocket::RecvMsg(int flags, std::span<u8> message, SockAddrIn* addr,
                                           u32& msg_flags) {
    msg_flags = 0;
    {
        // Room TCP sockets have no peer (Connect and Send are stubs). Report an empty queue as the
        // end of the stream, as before, instead of blocking the bsd service until the timeout.
        std::lock_guard guard(packets_mutex);
        if (socket_type == Type::STREAM && received_packets.empty()) {
            return {0, Errno::SUCCESS};
        }
    }
    return Receive(flags, message, addr, msg_flags);
}

std::pair<s32, Errno> ProxySocket::Receive(int flags, std::span<u8> message, SockAddrIn* addr,
                                           u32& msg_flags) {
    ASSERT((flags & ~(FLAG_MSG_PEEK | FLAG_MSG_DONTWAIT)) == 0);
    ASSERT(message.size() < static_cast<size_t>((std::numeric_limits<int>::max)()));

    std::unique_lock lock(packets_mutex);
    if (closed) {
        return {-1, Errno::BADF};
    }
    if (received_packets.empty()) {
        if (!blocking || (flags & FLAG_MSG_DONTWAIT) != 0) {
            return {-1, Errno::AGAIN};
        }
        // TODO (flTobi): Verify the timeout behavior and break when connection is lost
        // When receive_timeout is set to zero, the socket is supposed to wait indefinitely until a
        // packet arrives. In order to prevent lost packets from hanging the emulation thread, we set
        // the timeout to 5s instead. An expired wait is reported like an SO_RCVTIMEO timeout.
        const u32 indefinite_wait_cap_ms = SwitchSportsLanFixesEnabled() ? 250 : 5000;
        const u32 timeout = receive_timeout == 0 ? indefinite_wait_cap_ms : receive_timeout;
        if (!packets_cv.wait_for(lock, std::chrono::milliseconds{timeout},
                                 [this] { return closed || !received_packets.empty(); })) {
            return {-1, Errno::AGAIN};
        }
        if (closed) {
            return {-1, Errno::BADF};
        }
    }

    return ReceivePacket(flags, message, addr, message.size(), msg_flags);
}

std::pair<s32, Errno> ProxySocket::ReceivePacket(int flags, std::span<u8> message, SockAddrIn* addr,
                                                 std::size_t max_length, u32& msg_flags) {
    const ProxyPacket& packet = received_packets.front();
    if (addr) {
        addr->family = Domain::INET;
        addr->ip = packet.local_endpoint.ip;         // The senders ip address
        addr->portno = packet.local_endpoint.portno; // The senders port number
    }

    const std::span<const u8> unread = std::span{packet.data}.subspan(front_read_offset);
    const std::size_t read_bytes = (std::min)(unread.size(), max_length);
    std::memcpy(message.data(), unread.data(), read_bytes);

    // Unlike a stream's, the part of a datagram that does not fit is never read: report it.
    if (socket_type != Type::STREAM && read_bytes < unread.size()) {
        msg_flags |= FLAG_MSG_TRUNC;
    }

    if ((flags & FLAG_MSG_PEEK) != 0) {
        return {static_cast<s32>(read_bytes), Errno::SUCCESS};
    }

    if (socket_type == Type::STREAM && read_bytes < unread.size()) {
        // Keep the unread part of the stream for the next call
        front_read_offset += read_bytes;
    } else {
        // Like BSD recv(), the rest of a datagram that does not fit is discarded
        received_packets.pop();
        front_read_offset = 0;
    }

    return {static_cast<s32>(read_bytes), Errno::SUCCESS};
}

std::pair<s32, Errno> ProxySocket::Send(std::span<const u8> message, int flags) {
    LOG_WARNING(Network, "(STUBBED) called");
    ASSERT(message.size() < static_cast<size_t>((std::numeric_limits<int>::max)()));
    ASSERT((flags & ~FLAG_MSG_DONTWAIT) == 0);

    return {static_cast<s32>(0), Errno::SUCCESS};
}

void ProxySocket::SendPacket(ProxyPacket& packet) {
    if (auto room_member = Network::GetRoomMember().lock()) {
        if (room_member->IsConnected()) {
            packet.data = Common::Compression::CompressDataZSTDDefault(packet.data.data(),
                                                                       packet.data.size());
            if (packet.remote_endpoint.ip[0] == 127 && SwitchSportsLanFixesEnabled()) {
                packet.local_endpoint.ip = packet.remote_endpoint.ip;
                room_member->DeliverLoopbackProxyPacket(packet);
                return;
            }
            room_member->SendProxyPacket(packet);
        }
    }
}

std::pair<s32, Errno> ProxySocket::SendTo(u32 flags, std::span<const u8> message,
                                          const SockAddrIn* addr) {
    ASSERT((flags & ~FLAG_MSG_DONTWAIT) == 0);

    if (protocol == Protocol::ICMP && SwitchSportsLanFixesEnabled()) {
        constexpr u8 IcmpEchoRequest = 8;
        if (addr && message.size() >= 8 && message[0] == IcmpEchoRequest) {
            IPv4Address own_ip{127, 0, 0, 1};
            if (const auto room_ip = GetRoomIPv4Address()) {
                own_ip = *room_ip;
            } else if (const auto host_ip = GetHostIPv4Address()) {
                own_ip = *host_ip;
            }
            LOG_DEBUG(Network, "Answering ICMP echo to {}.{}.{}.{}", addr->ip[0], addr->ip[1],
                      addr->ip[2], addr->ip[3]);
            {
                std::lock_guard guard(packets_mutex);
                if (!closed) {
                    received_packets.push(ProxyPacket{
                        .local_endpoint = *addr,
                        .remote_endpoint = local_endpoint,
                        .protocol = protocol,
                        .broadcast = false,
                        .data = MakeIcmpEchoReply(message, addr->ip, own_ip),
                    });
                }
            }
            packets_cv.notify_all();
        }
        return {static_cast<s32>(message.size()), Errno::SUCCESS};
    }

    if (!is_bound && addr && SwitchSportsLanFixesEnabled()) {
        constexpr u32 EphemeralPortFirst = 50000;
        constexpr u32 EphemeralPortCount = 65536 - EphemeralPortFirst;
        static std::atomic<u32> next_ephemeral_port{0};
        SockAddrIn ephemeral{};
        ephemeral.family = Domain::INET;
        ephemeral.portno = static_cast<u16>(EphemeralPortFirst +
                                            next_ephemeral_port.fetch_add(1) % EphemeralPortCount);
        Bind(ephemeral);
    }

    if (!is_bound) {
        LOG_ERROR(Network, "ProxySocket is not bound!");
        return {static_cast<s32>(message.size()), Errno::SUCCESS};
    }

    if (auto room_member = Network::GetRoomMember().lock()) {
        if (!room_member->IsConnected()) {
            return {static_cast<s32>(message.size()), Errno::SUCCESS};
        }
    }

    if (socket_type == Type::DGRAM && addr && addr->ip[0] != 127 && addr->ip[3] != 255 &&
        addr->ip != GetRoomIPv4Address() && SwitchSportsLanFixesEnabled()) {
        Service::Friend::NoteResortUnicastTarget(addr->ip);
    }

    ProxyPacket packet;
    packet.local_endpoint = local_endpoint;
    packet.remote_endpoint = *addr;
    packet.protocol = protocol;
    packet.broadcast = broadcast && packet.remote_endpoint.ip[3] == 255;

    auto& ip = local_endpoint.ip;
    auto ipv4 = Network::GetHostIPv4Address();
    // If the ip is all zeroes (INADDR_ANY) or if it matches the hosts ip address,
    // replace it with a "fake" routing address
    if (std::all_of(ip.begin(), ip.end(), [](u8 i) { return i == 0; }) || (ipv4 && ipv4 == ip)) {
        if (auto room_member = Network::GetRoomMember().lock()) {
            packet.local_endpoint.ip = room_member->GetFakeIpAddress();
        }
    }

    packet.data.clear();
    std::copy(message.begin(), message.end(), std::back_inserter(packet.data));

    SendPacket(packet);

    return {static_cast<s32>(message.size()), Errno::SUCCESS};
}

Errno ProxySocket::Close() {
    {
        std::lock_guard guard(packets_mutex);
        fd = INVALID_SOCKET;
        closed = true;
        received_packets = {};
        front_read_offset = 0;
    }
    packets_cv.notify_all();

    return Errno::SUCCESS;
}

Errno ProxySocket::SetLinger(bool enable, u32 linger) {
    struct Linger {
        u16 linger_enable;
        u16 linger_time;
    } values;
    values.linger_enable = enable ? 1 : 0;
    values.linger_time = static_cast<u16>(linger);

    return SetSockOpt(fd, SO_LINGER, values);
}

Errno ProxySocket::SetReuseAddr(bool enable) {
    {
        std::lock_guard guard(packets_mutex);
        reuse_addr = enable;
    }
    return SetSockOpt<u32>(fd, SO_REUSEADDR, enable ? 1 : 0);
}

Errno ProxySocket::SetBroadcast(bool enable) {
    {
        std::lock_guard guard(packets_mutex);
        broadcast = enable;
    }
    return SetSockOpt<u32>(fd, SO_BROADCAST, enable ? 1 : 0);
}

Errno ProxySocket::SetSndBuf(u32 value) {
    return SetSockOpt(fd, SO_SNDBUF, value);
}

Errno ProxySocket::SetKeepAlive(bool enable) {
    return Errno::SUCCESS;
}

Errno ProxySocket::SetRcvBuf(u32 value) {
    return SetSockOpt(fd, SO_RCVBUF, value);
}

Errno ProxySocket::SetSndTimeo(u32 value) {
    send_timeout = value;
    return SetSockOpt(fd, SO_SNDTIMEO, static_cast<int>(value));
}

Errno ProxySocket::SetRcvTimeo(u32 value) {
    {
        std::lock_guard guard(packets_mutex);
        receive_timeout = value;
    }
    return SetSockOpt(fd, SO_RCVTIMEO, static_cast<int>(value));
}

Errno ProxySocket::SetNoDelay(bool enable) {
    // Room sockets have no TCP stack; the option is only kept for GetNoDelay.
    std::lock_guard guard(packets_mutex);
    no_delay = enable;
    return Errno::SUCCESS;
}

Errno ProxySocket::SetNonBlock(bool enable) {
    std::lock_guard guard(packets_mutex);
    blocking = !enable;
    return Errno::SUCCESS;
}

std::pair<Errno, Errno> ProxySocket::GetPendingError() {
    LOG_DEBUG(Network, "(STUBBED) called");
    return {Errno::SUCCESS, Errno::SUCCESS};
}

std::pair<bool, Errno> ProxySocket::GetReuseAddr() {
    std::lock_guard guard(packets_mutex);
    return {reuse_addr, Errno::SUCCESS};
}

std::pair<bool, Errno> ProxySocket::GetNoDelay() {
    std::lock_guard guard(packets_mutex);
    return {no_delay, Errno::SUCCESS};
}

bool ProxySocket::IsOpened() const {
    return fd != INVALID_SOCKET;
}

} // namespace Network
