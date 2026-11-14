// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <string>

#include "common/logging.h"
#include "common/zstd_compression.h"
#include "core/hle/service/friend/resort_doc_bus.h"

namespace Service::Friend::ResortBus {

namespace {

constexpr u8 MessageCompressed = 0x80;

std::string IpText(u32 ip) {
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 0xFF) + "." +
           std::to_string((ip >> 8) & 0xFF) + "." + std::to_string(ip & 0xFF);
}

const char* TypeName(MessageType type) {
    switch (type) {
    case MessageType::Hello:
        return "hello";
    case MessageType::Bye:
        return "bye";
    case MessageType::Update:
        return "update";
    case MessageType::Snapshot:
        return "snapshot";
    case MessageType::SnapshotEnd:
        return "snapshot-end";
    case MessageType::WriteRequest:
        return "write-request";
    }
    return "unknown";
}

u64 Mix(u64 value) {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

bool IsLive(u32 flags, bool erased) {
    return !erased && (flags & DocFlag::Deleted) == 0;
}

class Writer {
public:
    void U8(u8 value) {
        data.push_back(value);
    }
    void U16(u16 value) {
        Int(value);
    }
    void U32(u32 value) {
        Int(value);
    }
    void U64(u64 value) {
        Int(value);
    }
    void Bytes(std::span<const u8> bytes) {
        data.insert(data.end(), bytes.begin(), bytes.end());
    }
    void Text(std::string_view text) {
        data.insert(data.end(), text.begin(), text.end());
    }
    void PatchU32(std::size_t offset, u32 value) {
        for (std::size_t i = 0; i < 4; ++i) {
            data[offset + i] = static_cast<u8>(value >> (8 * i));
        }
    }

    std::vector<u8> data;

private:
    template <typename T>
    void Int(T value) {
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            data.push_back(static_cast<u8>(value >> (8 * i)));
        }
    }
};

class Reader {
public:
    explicit Reader(std::span<const u8> bytes_) : bytes{bytes_} {}

    bool U8(u8& out) {
        return Int(out);
    }
    bool U16(u16& out) {
        return Int(out);
    }
    bool U32(u32& out) {
        return Int(out);
    }
    bool U64(u64& out) {
        return Int(out);
    }
    bool Bytes(std::size_t size, std::span<const u8>& out) {
        if (bytes.size() - offset < size) {
            return false;
        }
        out = bytes.subspan(offset, size);
        offset += size;
        return true;
    }

private:
    template <typename T>
    bool Int(T& out) {
        if (bytes.size() - offset < sizeof(T)) {
            return false;
        }
        T value{};
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            value = static_cast<T>(value |
                                   static_cast<T>(static_cast<T>(bytes[offset + i]) << (8 * i)));
        }
        out = value;
        offset += sizeof(T);
        return true;
    }

    std::span<const u8> bytes;
    std::size_t offset{};
};

void WriteNickname(Writer& writer, std::string_view nickname) {
    const auto size = std::min<std::size_t>(nickname.size(), 0x20);
    writer.U8(static_cast<u8>(size));
    writer.U8(0);
    writer.U16(0);
    writer.Text(nickname.substr(0, size));
}

bool ReadNickname(Reader& reader, std::string& out) {
    u8 size{};
    u8 reserved8{};
    u16 reserved16{};
    std::span<const u8> bytes;
    if (!reader.U8(size) || !reader.U8(reserved8) || !reader.U16(reserved16) ||
        !reader.Bytes(size, bytes)) {
        return false;
    }
    out.assign(bytes.begin(), bytes.end());
    return true;
}

} // Anonymous namespace

DocBus::DocBus(Link& link_, u64 program_id_, u16 schema_)
    : link{link_}, program_id{program_id_}, schema{schema_} {}

void DocBus::Start(u64 node_id, const std::vector<RoomMemberView>& members) {
    {
        std::scoped_lock lock{mutex};
        ResetLocked();
        node = node_id;
        active.store(true, std::memory_order_release);
        LOG_INFO(Service_Friend,
                 "ResortBus start node={:016x} ip={} nickname={} online={} epoch={}", node,
                 IpText(link.OwnIp()), link.Nickname(), link.Connected(), epoch.load());
    }
    OnRoomMembers(members);
    OnConnection(link.Connected());
}

void DocBus::Stop() {
    std::scoped_lock lock{mutex};
    if (!active.load(std::memory_order_acquire)) {
        return;
    }
    SendBye(lobby, ByeReason::Stop);
    LOG_INFO(Service_Friend, "ResortBus stop node={:016x}", node);
    active.store(false, std::memory_order_release);
    ResetLocked();
}

bool DocBus::IsActive() const {
    return active.load(std::memory_order_acquire);
}

void DocBus::ResetLocked() {
    entries.clear();
    by_seq.clear();
    peers.clear();
    room_ips.clear();
    room_known = false;
    lobby = 0;
    session = 0;
    clock = 0;
    tx_seq = 0;
    node = 0;
    malformed_count = 0;
    oversize_count = 0;
    epoch.fetch_add(1, std::memory_order_acq_rel);
}

IdentityView DocBus::Identity() {
    std::scoped_lock lock{mutex};
    IdentityView view{};
    view.node = node;
    view.session = session;
    view.ip = link.OwnIp();
    view.lobby = lobby;
    return view;
}

Status DocBus::EnterLobby(u64 scope) {
    if (scope == RoomScope || scope == PendingScope) {
        return Status::InvalidArgument;
    }
    std::scoped_lock lock{mutex};
    if (!active.load(std::memory_order_acquire)) {
        return Status::Inactive;
    }
    if (lobby == scope) {
        return Status::Ok;
    }
    if (lobby != 0) {
        LeaveLobbyLocked();
    }
    lobby = scope;
    do {
        session = Mix(node ^ Mix(++session_counter)) | 1;
    } while (session == node);

    std::vector<DocKey> pending;
    for (const auto& [doc_key, entry] : entries) {
        if (doc_key.scope == PendingScope && IsLive(entry.flags, entry.erased)) {
            pending.push_back(doc_key);
        }
    }
    for (const auto& doc_key : pending) {
        auto it = entries.find(doc_key);
        Entry moved = std::move(it->second);
        by_seq.erase(moved.seq);
        entries.erase(it);

        DocKey lobby_key{scope, doc_key.collection, doc_key.key};
        Entry& slot = entries[lobby_key];
        if (slot.seq != 0) {
            by_seq.erase(slot.seq);
        }
        moved.seq = 0;
        moved.owner = session;
        moved.cause = node;
        moved.stamp = Stamp{++clock, node};
        slot = std::move(moved);
        Touch(lobby_key, slot);
    }

    LOG_INFO(Service_Friend,
             "ResortBus entered lobby {:016x} session={:016x}, {} own docs carried over", scope,
             session, pending.size());
    SendHello(HelloFlag::WantLobbyDocs);
    SendOwned(lobby, MessageType::Snapshot, std::nullopt);
    return Status::Ok;
}

Status DocBus::LeaveLobby() {
    std::scoped_lock lock{mutex};
    if (!active.load(std::memory_order_acquire)) {
        return Status::Inactive;
    }
    LeaveLobbyLocked();
    return Status::Ok;
}

void DocBus::LeaveLobbyLocked() {
    const u64 old_lobby = lobby;
    if (old_lobby == 0) {
        return;
    }
    SendBye(old_lobby, ByeReason::LeaveLobby);

    std::vector<DocKey> keys;
    for (const auto& [doc_key, entry] : entries) {
        if (doc_key.scope == old_lobby) {
            keys.push_back(doc_key);
        }
    }
    u32 kept = 0;
    u32 dropped = 0;
    u32 removed = 0;
    for (const auto& doc_key : keys) {
        auto it = entries.find(doc_key);
        Entry& entry = it->second;
        if (entry.erased) {
            continue;
        }
        if (!IsLive(entry.flags, entry.erased)) {
            entry.erased = true;
            continue;
        }
        const bool own = entry.owner == session;
        if (own && (entry.flags & DocFlag::Sticky) != 0) {
            Entry moved = std::move(entry);
            entries.erase(it);
            DocKey pending_key{PendingScope, doc_key.collection, doc_key.key};
            Entry& slot = entries[pending_key];
            if (slot.seq != 0) {
                by_seq.erase(slot.seq);
            }
            moved.owner = node;
            slot = std::move(moved);
            by_seq[slot.seq] = pending_key;
            ++kept;
            continue;
        }
        entry.erased = true;
        entry.body.clear();
        entry.cause = own ? node : entry.owner;
        Touch(doc_key, entry);
        if (own) {
            ++dropped;
        } else {
            ++removed;
        }
    }
    LOG_INFO(Service_Friend,
             "ResortBus left lobby {:016x} session={:016x}: {} own docs kept, {} own docs "
             "dropped, {} remote docs removed",
             old_lobby, session, kept, dropped, removed);
    lobby = 0;
    session = 0;
}

Status DocBus::Publish(u32 collection, std::string_view key, std::span<const u8> body, u32 flags,
                       u64 expected_seq, Stamp* out_stamp, u64* out_seq) {
    if (key.empty() || key.size() > MaxKeySize) {
        return Status::InvalidArgument;
    }
    if (body.size() > MaxBodySize) {
        return Status::TooLarge;
    }
    std::scoped_lock lock{mutex};
    if (!active.load(std::memory_order_acquire)) {
        return Status::Inactive;
    }
    const u64 scope = ScopeFor(flags);
    DocKey doc_key{scope, collection, std::string{key}};
    auto it = entries.find(doc_key);
    const bool exists = it != entries.end();
    const bool live = exists && IsLive(it->second.flags, it->second.erased);
    if (expected_seq == ExpectAbsent && live) {
        LOG_WARNING(Service_Friend, "ResortBus create of {:08x}/{} refused: owned by {:016x}",
                    collection, key, it->second.owner);
        return Status::Conflict;
    }
    if (expected_seq != ExpectAny && expected_seq != ExpectAbsent &&
        (!exists || it->second.seq != expected_seq)) {
        return Status::Conflict;
    }
    if (!exists && entries.size() >= MaxEntries) {
        CompactLocked();
        if (entries.size() >= MaxEntries) {
            return Status::Full;
        }
    }
    Entry& entry = exists ? it->second : entries[doc_key];
    entry.owner = live ? entry.owner : OwnerFor(scope);
    entry.stamp = Stamp{++clock, node};
    entry.flags = flags & (DocFlag::Sticky | DocFlag::RoomWide);
    entry.erased = false;
    entry.cause = node;
    entry.body.assign(body.begin(), body.end());
    Touch(doc_key, entry);
    LOG_DEBUG(
        Service_Friend, "ResortBus publish {:08x}/{} scope={:016x} size={} stamp={}:{:016x} seq={}",
        collection, key, scope, body.size(), entry.stamp.lamport, entry.stamp.node, entry.seq);
    if (scope != PendingScope) {
        if (IsOwn(entry.owner)) {
            SendRecords(MessageType::Update, scope, {MakeRecord(doc_key, entry)}, std::nullopt);
        } else {
            Forward(scope, doc_key, entry);
        }
    }
    if (out_stamp) {
        *out_stamp = entry.stamp;
    }
    if (out_seq) {
        *out_seq = entry.seq;
    }
    return Status::Ok;
}

Status DocBus::Delete(u32 collection, std::string_view key, u32 flags, u64 expected_seq,
                      Stamp* out_stamp, u64* out_seq) {
    if (key.empty() || key.size() > MaxKeySize || expected_seq == ExpectAbsent) {
        return Status::InvalidArgument;
    }
    std::scoped_lock lock{mutex};
    if (!active.load(std::memory_order_acquire)) {
        return Status::Inactive;
    }
    const u64 scope = ScopeFor(flags);
    DocKey doc_key{scope, collection, std::string{key}};
    auto it = entries.find(doc_key);
    if (it == entries.end() || !IsLive(it->second.flags, it->second.erased)) {
        return Status::NotFound;
    }
    Entry& entry = it->second;
    if (expected_seq != ExpectAny && entry.seq != expected_seq) {
        return Status::Conflict;
    }
    entry.stamp = Stamp{++clock, node};
    entry.flags = (entry.flags & (DocFlag::Sticky | DocFlag::RoomWide)) | DocFlag::Deleted;
    entry.body.clear();
    entry.cause = node;
    Touch(doc_key, entry);
    LOG_DEBUG(Service_Friend, "ResortBus delete {:08x}/{} scope={:016x} stamp={}:{:016x} seq={}",
              collection, key, scope, entry.stamp.lamport, entry.stamp.node, entry.seq);
    if (scope != PendingScope) {
        if (IsOwn(entry.owner)) {
            SendRecords(MessageType::Update, scope, {MakeRecord(doc_key, entry)}, std::nullopt);
        } else {
            Forward(scope, doc_key, entry);
        }
    }
    if (out_stamp) {
        *out_stamp = entry.stamp;
    }
    if (out_seq) {
        *out_seq = entry.seq;
    }
    return Status::Ok;
}

Status DocBus::Get(u32 collection, std::string_view key, u32 flags, DocView& out) {
    std::scoped_lock lock{mutex};
    if (!active.load(std::memory_order_acquire)) {
        return Status::Inactive;
    }
    const auto it = entries.find(DocKey{ScopeFor(flags), collection, std::string{key}});
    if (it == entries.end() || !IsLive(it->second.flags, it->second.erased)) {
        return Status::NotFound;
    }
    out = MakeView(it->first, it->second);
    return Status::Ok;
}

std::vector<DocView> DocBus::List(u32 collection) {
    std::scoped_lock lock{mutex};
    std::vector<DocView> views;
    for (const auto& [doc_key, entry] : entries) {
        if (IsVisibleScope(doc_key.scope) && IsLive(entry.flags, entry.erased) &&
            (collection == 0 || doc_key.collection == collection)) {
            views.push_back(MakeView(doc_key, entry));
        }
    }
    return views;
}

PollResult DocBus::Poll(u32 collection, u32 consumer_epoch, u64 cursor, u32 max_items,
                        u32 max_bytes, bool include_local) {
    PollResult result{};
    result.epoch = epoch.load(std::memory_order_acquire);
    result.head = head.load(std::memory_order_acquire);
    result.next_cursor = cursor;
    if (!active.load(std::memory_order_acquire)) {
        result.reset = consumer_epoch != result.epoch;
        result.next_cursor = 0;
        return result;
    }
    if (consumer_epoch == result.epoch && cursor >= result.head) {
        return result;
    }

    std::scoped_lock lock{mutex};
    result.epoch = epoch.load(std::memory_order_acquire);
    result.head = head.load(std::memory_order_acquire);
    u64 position = cursor;
    if (consumer_epoch != result.epoch) {
        result.reset = true;
        position = 0;
    }
    std::size_t bytes = 0;
    for (auto it = by_seq.upper_bound(position); it != by_seq.end(); ++it) {
        const auto& [seq, doc_key] = *it;
        const Entry& entry = entries.at(doc_key);
        if ((collection != 0 && doc_key.collection != collection) ||
            (!include_local && entry.cause == node)) {
            position = seq;
            continue;
        }
        const std::size_t size = ViewSize(doc_key.key.size(), entry.body.size());
        if (size > max_bytes) {
            ++oversize_count;
            if ((oversize_count & (oversize_count - 1)) == 0) {
                LOG_WARNING(Service_Friend,
                            "ResortBus poll skipped {:08x}/{}: {} bytes do not fit a {} byte "
                            "buffer ({} so far)",
                            doc_key.collection, doc_key.key, size, max_bytes, oversize_count);
            }
            position = seq;
            continue;
        }
        if (result.changes.size() >= max_items || bytes + size > max_bytes) {
            result.more = true;
            break;
        }
        result.changes.push_back(MakeView(doc_key, entry));
        bytes += size;
        position = seq;
    }
    result.next_cursor = position;
    return result;
}

u64 DocBus::ScopeFor(u32 flags) const {
    if ((flags & DocFlag::RoomWide) != 0) {
        return RoomScope;
    }
    return lobby != 0 ? lobby : PendingScope;
}

u64 DocBus::OwnerFor(u64 scope) const {
    return scope == RoomScope || scope == PendingScope ? node : session;
}

bool DocBus::IsOwn(u64 owner) const {
    return owner == node || (session != 0 && owner == session);
}

bool DocBus::IsVisibleScope(u64 scope) const {
    return scope == RoomScope || scope == PendingScope || (lobby != 0 && scope == lobby);
}

void DocBus::Touch(const DocKey& doc_key, Entry& entry) {
    if (entry.seq != 0) {
        by_seq.erase(entry.seq);
    }
    entry.seq = head.load(std::memory_order_relaxed) + 1;
    head.store(entry.seq, std::memory_order_release);
    by_seq[entry.seq] = doc_key;
}

DocView DocBus::MakeView(const DocKey& doc_key, const Entry& entry) const {
    DocView view{};
    view.collection = doc_key.collection;
    view.key = doc_key.key;
    view.owner = entry.owner;
    view.body = entry.body;
    return view;
}

DocBus::Record DocBus::MakeRecord(const DocKey& doc_key, const Entry& entry) const {
    return Record{
        doc_key.collection, doc_key.key, entry.owner, entry.stamp, entry.flags & DocFlag::WireMask,
        entry.body};
}

std::optional<u32> DocBus::OwnerAddress(u64 scope, u64 owner) const {
    for (const auto& [id, peer] : peers) {
        if (scope == RoomScope ? id == owner : (peer.session != 0 && peer.session == owner)) {
            return peer.ip;
        }
    }
    return std::nullopt;
}

void DocBus::Forward(u64 scope, const DocKey& doc_key, const Entry& entry) {
    const auto target = OwnerAddress(scope, entry.owner);
    if (!target) {
        LOG_WARNING(Service_Friend,
                    "ResortBus write to {:08x}/{} stays local: owner {:016x} unknown",
                    doc_key.collection, doc_key.key, entry.owner);
        return;
    }
    LOG_DEBUG(Service_Friend, "ResortBus forward {:08x}/{} to owner {:016x} at {}",
              doc_key.collection, doc_key.key, entry.owner, IpText(*target));
    SendRecords(MessageType::WriteRequest, scope, {MakeRecord(doc_key, entry)}, target);
}

DocBus::Peer& DocBus::Register(u64 sender, u32 source_ip) {
    std::vector<u64> stale;
    for (const auto& [id, peer] : peers) {
        if (id != sender && peer.ip == source_ip) {
            stale.push_back(id);
        }
    }
    for (const u64 id : stale) {
        Depart(id, "address reused");
    }
    auto [it, inserted] = peers.try_emplace(sender);
    if (inserted) {
        LOG_INFO(Service_Friend, "ResortBus peer {:016x} at {}", sender, IpText(source_ip));
    }
    it->second.ip = source_ip;
    return it->second;
}

void DocBus::Depart(u64 node_id, const char* reason) {
    const auto it = peers.find(node_id);
    u32 ip = 0;
    std::string nickname;
    u32 removed = EraseOwned(node_id, std::nullopt);
    if (it != peers.end()) {
        ip = it->second.ip;
        nickname = it->second.nickname;
        if (it->second.session != 0) {
            removed += EraseOwned(it->second.session, std::nullopt);
        }
        peers.erase(it);
    }
    LOG_INFO(Service_Friend, "ResortBus peer {:016x} {} at {} gone ({}), {} docs removed", node_id,
             nickname, IpText(ip), reason, removed);
}

void DocBus::EndSession(Peer& peer) {
    if (peer.session == 0) {
        return;
    }
    const u32 removed = EraseOwned(peer.session, std::nullopt);
    LOG_INFO(Service_Friend,
             "ResortBus {} ({}) left lobby {:016x} (session {:016x}), {} docs "
             "removed",
             peer.nickname, IpText(peer.ip), peer.lobby, peer.session, removed);
    peer.session = 0;
}

u32 DocBus::EraseOwned(u64 owner, std::optional<u64> scope) {
    u32 removed = 0;
    for (auto& [doc_key, entry] : entries) {
        if (entry.owner != owner || entry.erased || (scope && doc_key.scope != *scope)) {
            continue;
        }
        const bool live = IsLive(entry.flags, entry.erased);
        entry.erased = true;
        entry.body.clear();
        if (live) {
            entry.cause = owner;
            Touch(doc_key, entry);
            ++removed;
        }
    }
    return removed;
}

void DocBus::OnDatagram(u32 source_ip, std::span<const u8> message) {
    if (!active.load(std::memory_order_acquire)) {
        return;
    }
    Header header{};
    std::vector<u8> payload;
    bool valid = message.size() >= HeaderSize;
    if (valid) {
        Reader reader{message};
        u32 magic{};
        u8 version{};
        u8 type{};
        u8 flags{};
        u8 reserved8{};
        u16 reserved16{};
        u32 raw_size{};
        u32 wire_size{};
        reader.U32(magic);
        reader.U8(version);
        reader.U8(type);
        reader.U8(flags);
        reader.U8(reserved8);
        reader.U64(header.node);
        reader.U64(header.session);
        reader.U64(header.scope);
        reader.U64(header.lamport);
        reader.U32(header.seq);
        reader.U16(header.schema);
        reader.U16(reserved16);
        reader.U32(raw_size);
        reader.U32(wire_size);
        header.type = static_cast<MessageType>(type);
        header.flags = static_cast<u8>(flags & ~MessageCompressed);
        const auto wire = message.subspan(HeaderSize);
        valid = magic == WireMagic && version == WireVersion && raw_size <= MaxMessageSize &&
                wire_size == wire.size() && header.node != 0;
        if (valid && (flags & MessageCompressed) != 0) {
            auto inflated = Common::Compression::TryDecompressDataZSTD(wire, MaxMessageSize);
            valid = inflated.has_value() && inflated->size() == raw_size;
            if (valid) {
                payload = std::move(*inflated);
            }
        } else if (valid) {
            valid = wire_size == raw_size;
            payload.assign(wire.begin(), wire.end());
        }
    }

    std::scoped_lock lock{mutex};
    if (!active.load(std::memory_order_acquire)) {
        return;
    }
    if (!valid) {
        ++malformed_count;
        if ((malformed_count & (malformed_count - 1)) == 0) {
            LOG_WARNING(Service_Friend,
                        "ResortBus dropped a malformed message from {} ({} bytes, {} so far)",
                        IpText(source_ip), message.size(), malformed_count);
        }
        return;
    }
    if (header.node == node) {
        return;
    }
    if (header.schema != schema) {
        LOG_WARNING(Service_Friend, "ResortBus dropped {} from {}: schema {} != {}",
                    TypeName(header.type), IpText(source_ip), header.schema, schema);
        return;
    }
    if (room_known && !room_ips.contains(source_ip)) {
        LOG_DEBUG(Service_Friend, "ResortBus dropped {} from {}: not a member of the room",
                  TypeName(header.type), IpText(source_ip));
        return;
    }
    clock = std::max(clock, header.lamport);
    Peer& peer = Register(header.node, source_ip);
    if (peer.session != header.session) {
        EndSession(peer);
        peer.session = header.session;
    }
    LOG_DEBUG(Service_Friend,
              "ResortBus rx {} from {:016x} ({}) session={:016x} scope={:016x} seq={} "
              "bytes={}",
              TypeName(header.type), header.node, IpText(source_ip), header.session, header.scope,
              header.seq, message.size());

    switch (header.type) {
    case MessageType::Hello:
        HandleHello(header.node, peer, header, payload);
        break;
    case MessageType::Bye:
        HandleBye(header.node, peer, header, payload);
        break;
    case MessageType::Update:
    case MessageType::Snapshot:
        HandleRecords(header.node, peer, header, payload);
        break;
    case MessageType::WriteRequest:
        HandleWriteRequest(peer, header, payload);
        break;
    case MessageType::SnapshotEnd: {
        Reader reader{payload};
        u64 their_lobby{};
        u32 count{};
        std::string nickname;
        if (reader.U64(their_lobby) && reader.U32(count) && ReadNickname(reader, nickname)) {
            peer.lobby = their_lobby;
            peer.nickname = std::move(nickname);
            LOG_DEBUG(Service_Friend,
                      "ResortBus snapshot from {} ({}) done: {} docs, lobby={:016x}", peer.nickname,
                      IpText(peer.ip), count, their_lobby);
        }
        break;
    }
    default:
        break;
    }
}

void DocBus::HandleHello(u64 sender, Peer& peer, const Header& header,
                         std::span<const u8> payload) {
    Reader reader{payload};
    u64 their_program{};
    u32 their_ip{};
    std::string nickname;
    if (!reader.U64(their_program) || !reader.U32(their_ip) || !ReadNickname(reader, nickname)) {
        return;
    }
    if (their_program != program_id) {
        LOG_WARNING(Service_Friend, "ResortBus hello from {} ignored: program {:016x}",
                    IpText(peer.ip), their_program);
        return;
    }
    peer.nickname = std::move(nickname);
    peer.lobby = header.scope;
    if (their_ip != peer.ip) {
        LOG_WARNING(Service_Friend, "ResortBus hello from {} claims address {}", IpText(peer.ip),
                    IpText(their_ip));
    }
    LOG_DEBUG(Service_Friend,
              "ResortBus hello from {} ({}) node={:016x} lobby={:016x} session={:016x} flags={:x}",
              peer.nickname, IpText(peer.ip), sender, header.scope, header.session, header.flags);

    const u8 wants = header.flags & (HelloFlag::WantRoomDocs | HelloFlag::WantLobbyDocs);
    if (wants == 0) {
        return;
    }
    const u32 target = peer.ip;
    u32 sent = 0;
    if ((wants & HelloFlag::WantRoomDocs) != 0) {
        sent += SendOwned(RoomScope, MessageType::Snapshot, target);
    }
    if ((wants & HelloFlag::WantLobbyDocs) != 0 && lobby != 0 && header.scope == lobby) {
        sent += SendOwned(lobby, MessageType::Snapshot, target);
    }
    Writer writer;
    writer.U64(lobby);
    writer.U32(sent);
    WriteNickname(writer, link.Nickname());
    SendMessage(MessageType::SnapshotEnd, lobby, std::move(writer.data), target, 0);
}

void DocBus::HandleBye(u64 sender, Peer& peer, const Header& header, std::span<const u8> payload) {
    Reader reader{payload};
    u8 reason{};
    if (!reader.U8(reason)) {
        return;
    }
    if (static_cast<ByeReason>(reason) == ByeReason::Stop) {
        Depart(sender, "stopped");
        return;
    }
    if (header.scope != peer.lobby && peer.lobby != 0) {
        LOG_DEBUG(Service_Friend, "ResortBus bye for lobby {:016x} while peer was in {:016x}",
                  header.scope, peer.lobby);
    }
    EndSession(peer);
    peer.lobby = 0;
}

bool DocBus::ReadRecords(std::span<const u8> payload, std::vector<Record>& out) {
    Reader reader{payload};
    u32 count{};
    if (!reader.U32(count) || count > MaxRecordsPerMessage) {
        return false;
    }
    out.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        Record record{};
        u16 key_size{};
        u16 reserved16{};
        u32 body_size{};
        std::span<const u8> key;
        std::span<const u8> body;
        if (!reader.U32(record.collection) || !reader.U16(key_size) || !reader.U16(reserved16) ||
            !reader.U64(record.owner) || !reader.U64(record.stamp.lamport) ||
            !reader.U64(record.stamp.node) || !reader.U32(record.flags) || !reader.U32(body_size) ||
            key_size == 0 || key_size > MaxKeySize || body_size > MaxBodySize ||
            !reader.Bytes(key_size, key) || !reader.Bytes(body_size, body) || record.owner == 0 ||
            record.stamp.node == 0) {
            return false;
        }
        record.key.assign(key.begin(), key.end());
        record.body.assign(body.begin(), body.end());
        out.push_back(std::move(record));
    }
    return true;
}

void DocBus::HandleRecords(u64 sender, Peer& peer, const Header& header,
                           std::span<const u8> payload) {
    if (header.scope != RoomScope) {
        peer.lobby = header.scope;
    }
    if (header.scope != RoomScope && header.scope != lobby) {
        LOG_DEBUG(Service_Friend, "ResortBus ignored {} for lobby {:016x} (ours {:016x})",
                  TypeName(header.type), header.scope, lobby);
        return;
    }
    std::vector<Record> records;
    if (!ReadRecords(payload, records)) {
        LOG_WARNING(Service_Friend, "ResortBus bad records in {} from {:016x}",
                    TypeName(header.type), sender);
        return;
    }
    const u64 expected_owner = header.scope == RoomScope ? sender : header.session;
    std::vector<Record> retracts;
    for (auto& record : records) {
        if (expected_owner == 0 || record.owner != expected_owner) {
            LOG_WARNING(
                Service_Friend,
                "ResortBus rejected {:08x}/{} from {:016x}: owner {:016x} is not the sender",
                record.collection, record.key, sender, record.owner);
            continue;
        }
        if ((record.flags & DocFlag::Retract) != 0) {
            const auto it = entries.find(DocKey{header.scope, record.collection, record.key});
            if (it != entries.end() && IsLive(it->second.flags, it->second.erased) &&
                it->second.owner == record.owner && it->second.stamp == record.stamp) {
                it->second.erased = true;
                it->second.body.clear();
                it->second.cause = record.owner;
                Touch(it->first, it->second);
                LOG_DEBUG(Service_Friend, "ResortBus retracted {:08x}/{} stamp={}:{:016x}",
                          record.collection, record.key, record.stamp.lamport, record.stamp.node);
            }
            continue;
        }
        ApplyRemote(header.scope, std::move(record), retracts);
    }
    if (!retracts.empty()) {
        SendRecords(MessageType::Update, header.scope, retracts, std::nullopt);
    }
}

void DocBus::HandleWriteRequest(Peer& peer, const Header& header, std::span<const u8> payload) {
    if (header.scope != RoomScope && header.scope != lobby) {
        LOG_DEBUG(Service_Friend,
                  "ResortBus write request from {} for lobby {:016x} (ours {:016x})",
                  IpText(peer.ip), header.scope, lobby);
        return;
    }
    std::vector<Record> records;
    if (!ReadRecords(payload, records)) {
        return;
    }
    const u64 own = OwnerFor(header.scope);
    std::vector<Record> accepted;
    std::vector<Record> current;
    for (auto& record : records) {
        clock = std::max(clock, record.stamp.lamport);
        if (own == 0 || record.owner != own) {
            LOG_DEBUG(Service_Friend,
                      "ResortBus write request for {:08x}/{} names owner {:016x}, not us",
                      record.collection, record.key, record.owner);
            continue;
        }
        DocKey doc_key{header.scope, record.collection, record.key};
        auto it = entries.find(doc_key);
        if (it != entries.end() && !it->second.erased && !(record.stamp > it->second.stamp)) {
            current.push_back(MakeRecord(it->first, it->second));
            continue;
        }
        if (it == entries.end()) {
            if (entries.size() >= MaxEntries) {
                CompactLocked();
                if (entries.size() >= MaxEntries) {
                    continue;
                }
            }
            it = entries.emplace(doc_key, Entry{}).first;
        }
        Entry& entry = it->second;
        entry.owner = own;
        entry.stamp = record.stamp;
        entry.flags = record.flags & DocFlag::WireMask;
        entry.erased = false;
        entry.cause = record.stamp.node;
        entry.body = std::move(record.body);
        if ((entry.flags & DocFlag::Deleted) != 0) {
            entry.body.clear();
        }
        Touch(it->first, entry);
        LOG_DEBUG(Service_Friend, "ResortBus accepted write to {:08x}/{} from {} stamp={}:{:016x}",
                  doc_key.collection, doc_key.key, peer.nickname, entry.stamp.lamport,
                  entry.stamp.node);
        accepted.push_back(MakeRecord(it->first, entry));
    }
    if (!accepted.empty()) {
        SendRecords(MessageType::Update, header.scope, accepted, std::nullopt);
    }
    if (!current.empty()) {
        SendRecords(MessageType::Update, header.scope, current, peer.ip);
    }
}

bool DocBus::ApplyRemote(u64 scope, Record&& record, std::vector<Record>& retracts) {
    clock = std::max(clock, record.stamp.lamport);
    DocKey doc_key{scope, record.collection, record.key};
    auto it = entries.find(doc_key);
    if (it == entries.end()) {
        if (entries.size() >= MaxEntries) {
            CompactLocked();
            if (entries.size() >= MaxEntries) {
                LOG_ERROR(Service_Friend, "ResortBus table full, dropped {:08x}/{}",
                          record.collection, record.key);
                return false;
            }
        }
        it = entries.emplace(doc_key, Entry{}).first;
    } else if (!it->second.erased && !(record.stamp > it->second.stamp)) {
        return false;
    }
    Entry& entry = it->second;
    if (entry.seq != 0 && IsLive(entry.flags, entry.erased) && IsOwn(entry.owner) &&
        !IsOwn(record.owner)) {
        LOG_WARNING(Service_Friend,
                    "ResortBus key collision on {:08x}/{}: own doc replaced by {:016x}",
                    record.collection, record.key, record.owner);
        retracts.push_back(
            Record{record.collection, record.key, entry.owner, entry.stamp, DocFlag::Retract, {}});
    }
    entry.owner = record.owner;
    entry.stamp = record.stamp;
    entry.flags = record.flags & DocFlag::WireMask;
    entry.erased = false;
    entry.cause = record.stamp.node;
    entry.body = std::move(record.body);
    if ((entry.flags & DocFlag::Deleted) != 0) {
        entry.body.clear();
    }
    Touch(it->first, entry);
    LOG_DEBUG(Service_Friend,
              "ResortBus applied {:08x}/{} scope={:016x} owner={:016x} stamp={}:{:016x} "
              "deleted={} size={} seq={}",
              record.collection, record.key, scope, entry.owner, entry.stamp.lamport,
              entry.stamp.node, (entry.flags & DocFlag::Deleted) != 0, entry.body.size(),
              entry.seq);
    return true;
}

void DocBus::OnRoomMembers(const std::vector<RoomMemberView>& members) {
    std::scoped_lock lock{mutex};
    if (!active.load(std::memory_order_acquire)) {
        return;
    }
    const u32 own_ip = link.OwnIp();
    room_ips.clear();
    std::unordered_set<u32> resort_ips;
    for (const auto& member : members) {
        if (member.ip == own_ip) {
            continue;
        }
        room_ips.insert(member.ip);
        if (member.program_id == program_id) {
            resort_ips.insert(member.ip);
        }
    }
    room_known = true;
    std::vector<u64> gone;
    for (const auto& [id, peer] : peers) {
        if (!resort_ips.contains(peer.ip)) {
            gone.push_back(id);
        }
    }
    for (const u64 id : gone) {
        Depart(id, "left the room or stopped the game");
    }
}

void DocBus::OnConnection(bool connected) {
    std::scoped_lock lock{mutex};
    if (!active.load(std::memory_order_acquire)) {
        return;
    }
    if (!connected) {
        u32 removed = 0;
        for (auto& [doc_key, entry] : entries) {
            if (IsOwn(entry.owner) || entry.erased) {
                continue;
            }
            const bool live = IsLive(entry.flags, entry.erased);
            entry.erased = true;
            entry.body.clear();
            if (live) {
                entry.cause = entry.owner;
                Touch(doc_key, entry);
                ++removed;
            }
        }
        if (!peers.empty() || removed != 0) {
            LOG_INFO(Service_Friend, "ResortBus offline: {} peers and {} docs removed",
                     peers.size(), removed);
        }
        peers.clear();
        room_ips.clear();
        room_known = false;
        return;
    }
    if (lobby != 0) {
        const u64 old_session = session;
        do {
            session = Mix(node ^ Mix(++session_counter)) | 1;
        } while (session == node);
        for (auto& [doc_key, entry] : entries) {
            if (doc_key.scope == lobby && entry.owner == old_session &&
                IsLive(entry.flags, entry.erased)) {
                entry.owner = session;
                entry.stamp = Stamp{++clock, node};
                entry.cause = node;
                Touch(doc_key, entry);
            }
        }
    }
    LOG_INFO(Service_Friend, "ResortBus online as {} node={:016x} lobby={:016x} session={:016x}",
             IpText(link.OwnIp()), node, lobby, session);
    SendHello(
        static_cast<u8>(HelloFlag::WantRoomDocs | (lobby != 0 ? HelloFlag::WantLobbyDocs : 0)));
    SendOwned(RoomScope, MessageType::Snapshot, std::nullopt);
    if (lobby != 0) {
        SendOwned(lobby, MessageType::Snapshot, std::nullopt);
    }
}

void DocBus::SendHello(u8 flags) {
    Writer writer;
    writer.U64(program_id);
    writer.U32(link.OwnIp());
    WriteNickname(writer, link.Nickname());
    SendMessage(MessageType::Hello, lobby, std::move(writer.data), std::nullopt, flags);
}

void DocBus::SendBye(u64 scope, ByeReason reason) {
    Writer writer;
    writer.U8(static_cast<u8>(reason));
    SendMessage(MessageType::Bye, scope, std::move(writer.data), std::nullopt, 0);
}

u32 DocBus::SendOwned(u64 scope, MessageType type, std::optional<u32> target) {
    const u64 owner = OwnerFor(scope);
    std::vector<Record> records;
    for (const auto& [doc_key, entry] : entries) {
        if (doc_key.scope == scope && entry.owner == owner && IsLive(entry.flags, entry.erased)) {
            records.push_back(MakeRecord(doc_key, entry));
        }
    }
    if (!records.empty()) {
        SendRecords(type, scope, records, target);
    }
    return static_cast<u32>(records.size());
}

void DocBus::SendRecords(MessageType type, u64 scope, const std::vector<Record>& records,
                         std::optional<u32> target) {
    Writer writer;
    writer.U32(0);
    u32 count = 0;
    const auto flush = [&] {
        writer.PatchU32(0, count);
        SendMessage(type, scope, std::move(writer.data), target, 0);
        writer.data.clear();
        writer.U32(0);
        count = 0;
    };
    for (const auto& record : records) {
        const std::size_t size = RecordHeaderSize + record.key.size() + record.body.size();
        if (count != 0 &&
            (writer.data.size() + size > MaxMessageSize || count >= MaxRecordsPerMessage)) {
            flush();
        }
        writer.U32(record.collection);
        writer.U16(static_cast<u16>(record.key.size()));
        writer.U16(0);
        writer.U64(record.owner);
        writer.U64(record.stamp.lamport);
        writer.U64(record.stamp.node);
        writer.U32(record.flags);
        writer.U32(static_cast<u32>(record.body.size()));
        writer.Text(record.key);
        writer.Bytes(record.body);
        ++count;
    }
    if (count != 0) {
        flush();
    }
}

void DocBus::SendMessage(MessageType type, u64 scope, std::vector<u8>&& payload,
                         std::optional<u32> target, u8 flags) {
    if (!link.Connected()) {
        return;
    }
    std::vector<u8> compressed;
    std::span<const u8> wire{payload};
    if (payload.size() >= CompressThreshold) {
        compressed = Common::Compression::CompressDataZSTDDefault(payload.data(), payload.size());
        if (!compressed.empty() && compressed.size() < payload.size()) {
            wire = compressed;
            flags |= MessageCompressed;
        }
    }
    const u32 seq = target ? 0 : ++tx_seq;
    Writer writer;
    writer.data.reserve(HeaderSize + wire.size());
    writer.U32(WireMagic);
    writer.U8(WireVersion);
    writer.U8(static_cast<u8>(type));
    writer.U8(flags);
    writer.U8(0);
    writer.U64(node);
    writer.U64(session);
    writer.U64(scope);
    writer.U64(clock);
    writer.U32(seq);
    writer.U16(schema);
    writer.U16(0);
    writer.U32(static_cast<u32>(payload.size()));
    writer.U32(static_cast<u32>(wire.size()));
    writer.Bytes(wire);
    LOG_DEBUG(Service_Friend, "ResortBus tx {} scope={:016x} seq={} bytes={} raw={} to {}",
              TypeName(type), scope, seq, writer.data.size(), payload.size(),
              target ? IpText(*target) : std::string{"all"});
    link.Send(std::move(writer.data), target);
}

void DocBus::CompactLocked() {
    std::size_t removed = 0;
    for (auto it = entries.begin(); it != entries.end();) {
        const bool tombstone = !IsLive(it->second.flags, it->second.erased);
        if (tombstone && !IsVisibleScope(it->first.scope)) {
            by_seq.erase(it->second.seq);
            it = entries.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (entries.size() >= MaxEntries) {
        for (auto it = entries.begin(); it != entries.end();) {
            if (it->second.erased) {
                by_seq.erase(it->second.seq);
                it = entries.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
    }
    if (removed != 0) {
        epoch.fetch_add(1, std::memory_order_acq_rel);
        LOG_ERROR(Service_Friend, "ResortBus table full: {} tombstones removed, epoch now {}",
                  removed, epoch.load());
    }
}

} // namespace Service::Friend::ResortBus
