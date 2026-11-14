// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <compare>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"

namespace Service::Friend::ResortBus {

constexpr u32 WireMagic = 0x53554245;
constexpr u8 WireVersion = 1;
constexpr u8 WireProtocol = 0xED;
constexpr u16 WirePort = 0xEDB5;
constexpr u16 WireSchema = 1;
constexpr std::size_t HeaderSize = 0x38;
constexpr std::size_t RecordHeaderSize = 0x28;
constexpr std::size_t MaxKeySize = 0x80;
constexpr std::size_t MaxBodySize = 0xF000;
constexpr std::size_t MaxMessageSize = 0x10000;
constexpr std::size_t MaxRecordsPerMessage = 0x400;
constexpr std::size_t MaxEntries = 0x1000;
constexpr std::size_t CompressThreshold = 0x400;
constexpr std::size_t ViewHeaderSize = 0x40;
constexpr u64 RoomScope = 0;
constexpr u64 PendingScope = ~u64{0};
constexpr u64 LobbyScopeTag = 0xEDB5000000000000ULL;
constexpr u64 ExpectAny = 0;
constexpr u64 ExpectAbsent = ~u64{0};

enum class MessageType : u8 {
    Hello = 1,
    Bye = 2,
    Update = 3,
    Snapshot = 4,
    SnapshotEnd = 5,
    WriteRequest = 6,
};

enum class Status : u32 {
    Ok = 0,
    Inactive = 1,
    InvalidArgument = 2,
    TooLarge = 3,
    NotFound = 4,
    Conflict = 5,
    Full = 6,
    NoLobby = 7,
};

namespace DocFlag {
constexpr u32 Deleted = 1U << 0;
constexpr u32 Sticky = 1U << 1;
constexpr u32 RoomWide = 1U << 2;
constexpr u32 Retract = 1U << 3;
constexpr u32 WireMask = Deleted | Sticky | RoomWide;
} // namespace DocFlag

namespace HelloFlag {
constexpr u8 WantLobbyDocs = 1U << 0;
constexpr u8 WantRoomDocs = 1U << 1;
} // namespace HelloFlag

enum class ByeReason : u8 {
    LeaveLobby = 0,
    Stop = 1,
};

struct Stamp {
    u64 lamport{};
    u64 node{};
    friend auto operator<=>(const Stamp&, const Stamp&) = default;
};

struct DocKey {
    u64 scope{};
    u32 collection{};
    std::string key;
    friend auto operator<=>(const DocKey&, const DocKey&) = default;
};

struct DocView {
    u32 collection{};
    std::string key;
    u64 owner{};
    std::vector<u8> body;
};

struct PollResult {
    u32 epoch{};
    bool reset{};
    bool more{};
    u64 next_cursor{};
    u64 head{};
    std::vector<DocView> changes;
};

struct IdentityView {
    u64 node{};
    u64 session{};
    u32 ip{};
    u64 lobby{};
};

struct RoomMemberView {
    u32 ip{};
    u64 program_id{};
    std::string nickname;
};

class Link {
public:
    virtual ~Link() = default;
    virtual bool Connected() const = 0;
    virtual u32 OwnIp() const = 0;
    virtual std::string Nickname() const = 0;
    virtual void Send(std::vector<u8>&& message, std::optional<u32> target_ip) = 0;
};

constexpr u64 LobbyScopeFromHostIp(u32 host_ip) {
    return LobbyScopeTag | host_ip;
}

constexpr std::size_t ViewSize(std::size_t key_size, std::size_t body_size) {
    return ViewHeaderSize + ((key_size + 1 + 7) & ~std::size_t{7}) +
           ((body_size + 7) & ~std::size_t{7});
}

class DocBus {
public:
    DocBus(Link& link, u64 program_id, u16 schema = WireSchema);

    void Start(u64 node_id, const std::vector<RoomMemberView>& members);
    void Stop();
    bool IsActive() const;

    IdentityView Identity();
    Status EnterLobby(u64 scope);
    Status LeaveLobby();
    Status Publish(u32 collection, std::string_view key, std::span<const u8> body, u32 flags,
                   u64 expected_seq, Stamp* out_stamp, u64* out_seq);
    Status Delete(u32 collection, std::string_view key, u32 flags, u64 expected_seq,
                  Stamp* out_stamp, u64* out_seq);
    Status Get(u32 collection, std::string_view key, u32 flags, DocView& out);
    std::vector<DocView> List(u32 collection);
    PollResult Poll(u32 collection, u32 epoch, u64 cursor, u32 max_items, u32 max_bytes,
                    bool include_local);

    void OnDatagram(u32 source_ip, std::span<const u8> message);
    void OnRoomMembers(const std::vector<RoomMemberView>& members);
    void OnConnection(bool connected);

private:
    struct Entry {
        u64 owner{};
        Stamp stamp{};
        u32 flags{};
        u64 seq{};
        u64 cause{};
        bool erased{};
        std::vector<u8> body;
    };

    struct Peer {
        u32 ip{};
        u64 lobby{};
        u64 session{};
        std::string nickname;
    };

    struct Header {
        MessageType type{};
        u8 flags{};
        u64 node{};
        u64 session{};
        u64 scope{};
        u64 lamport{};
        u32 seq{};
        u16 schema{};
    };

    struct Record {
        u32 collection{};
        std::string key;
        u64 owner{};
        Stamp stamp{};
        u32 flags{};
        std::vector<u8> body;
    };

    void ResetLocked();
    void Touch(const DocKey& key, Entry& entry);
    u64 ScopeFor(u32 flags) const;
    u64 OwnerFor(u64 scope) const;
    bool IsOwn(u64 owner) const;
    bool IsVisibleScope(u64 scope) const;
    DocView MakeView(const DocKey& key, const Entry& entry) const;
    Peer& Register(u64 sender, u32 source_ip);
    void Depart(u64 node_id, const char* reason);
    void EndSession(Peer& peer);
    u32 EraseOwned(u64 owner, std::optional<u64> scope);
    void LeaveLobbyLocked();
    bool ApplyRemote(u64 scope, Record&& record, std::vector<Record>& retracts);
    std::optional<u32> OwnerAddress(u64 scope, u64 owner) const;
    void Forward(u64 scope, const DocKey& key, const Entry& entry);
    void HandleHello(u64 sender, Peer& peer, const Header& header, std::span<const u8> payload);
    void HandleBye(u64 sender, Peer& peer, const Header& header, std::span<const u8> payload);
    void HandleRecords(u64 sender, Peer& peer, const Header& header, std::span<const u8> payload);
    void HandleWriteRequest(Peer& peer, const Header& header, std::span<const u8> payload);
    static bool ReadRecords(std::span<const u8> payload, std::vector<Record>& out);
    void SendHello(u8 flags);
    void SendBye(u64 scope, ByeReason reason);
    u32 SendOwned(u64 scope, MessageType type, std::optional<u32> target);
    void SendRecords(MessageType type, u64 scope, const std::vector<Record>& records,
                     std::optional<u32> target);
    void SendMessage(MessageType type, u64 scope, std::vector<u8>&& payload,
                     std::optional<u32> target, u8 flags);
    Record MakeRecord(const DocKey& key, const Entry& entry) const;
    void CompactLocked();

    Link& link;
    const u64 program_id;
    const u16 schema;

    mutable std::mutex mutex;
    std::atomic<bool> active{false};
    std::atomic<u64> head{0};
    std::atomic<u32> epoch{0};
    u64 node{};
    u64 session{};
    u64 session_counter{};
    u64 clock{};
    u32 tx_seq{};
    u64 lobby{};
    bool room_known{};
    std::map<DocKey, Entry> entries;
    std::map<u64, DocKey> by_seq;
    std::unordered_map<u64, Peer> peers;
    std::unordered_set<u32> room_ips;
    u64 malformed_count{};
    u64 oversize_count{};
};

DocBus& GetResortDocBus();
bool StartResortDocBus();
void StopResortDocBus();
Status EnterResortLobby(u64 scope);
std::optional<u64> ResolveResortLobbyScope();

} // namespace Service::Friend::ResortBus
