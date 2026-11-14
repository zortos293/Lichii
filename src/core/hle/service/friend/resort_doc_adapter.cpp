// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/swap.h"
#include "core/hle/service/friend/resort_doc_adapter.h"
#include "core/hle/service/friend/resort_doc_bus.h"
#include "core/hle/service/friend/resort_lan_friends.h"
#include "core/memory.h"
#include "network/network.h"
#include "network/room_member.h"

namespace Service::Friend::ResortBus {

namespace {

constexpr u32 RecordMagic = 0x57434F44;
constexpr u32 QueryMagic = 0x51434F44;
constexpr u32 EventMagic = 0x53434F44;
constexpr u32 RoomHash = 0x57D9634C;
constexpr u32 RoomrHash = 0xD7CB3179;
constexpr u32 UserdocHash = 0x4C687DDD;
constexpr u32 MemberCollection = 0xEDB50001;
constexpr u32 LanCollection = 0xEDB50002;
constexpr std::string_view LanKey = "lan";
constexpr std::size_t LanBodySize = 12;
constexpr std::chrono::seconds LanBeat{1};
constexpr std::chrono::seconds LanStale{3};
constexpr std::chrono::seconds LanTargetLife{30};
constexpr std::chrono::seconds LanAdoptWindow{5};
constexpr s32 LobbyNone = 0;
constexpr s32 LobbyHost = 1;
constexpr s32 LobbyRejoin = 2;
constexpr s32 LobbyWait = 3;
constexpr std::size_t RecordSize = 0x40;
constexpr std::size_t EventHeaderSize = 0x20;
constexpr std::size_t SnapshotEntrySize = 0x18;
constexpr std::size_t MemberListHead = 0x20;
constexpr std::size_t MemberNodesStart = 0x30;
constexpr std::size_t MemberNodeSize = 0xD0;
constexpr std::size_t MaxMembers = 16;
constexpr std::size_t MaxShortString = 22;
constexpr std::size_t MaxIdLength = 0x80;
constexpr std::size_t MaxPathLength = 0x40;
constexpr u32 LocalMemberKind = 3;
constexpr std::chrono::milliseconds LocalMemberRetry{3000};

using Entries = std::vector<std::pair<std::string, std::vector<u8>>>;

struct Event {
    std::string name;
    std::vector<u8> bytes;
    std::vector<u32> pointers;
};

struct LanTrack {
    u64 lobby{};
    bool up{};
    u32 beat{};
    std::chrono::steady_clock::time_point next_beat{};
    std::optional<std::chrono::steady_clock::time_point> seen;
    u32 last_gen{};
    std::chrono::steady_clock::time_point gen_since{};
    bool had{};
    std::chrono::steady_clock::time_point had_since{};
    u32 target_gen{};
    std::chrono::steady_clock::time_point target_time{};
    u32 joined_gen{};
    u32 rejoin_gen{};
};

struct State {
    std::mutex mutex;
    bool announced{};
    u32 epoch{};
    u64 cursor{};
    u64 lobby{};
    bool dirty_members{true};
    bool dirty_userdocs{true};
    bool dirty_rooms{true};
    bool dirty_roomrs{true};
    u64 last_members{};
    u64 last_userdocs{};
    u64 last_rooms{};
    u64 last_roomrs{};
    std::deque<Event> outbox;
    u32 gsf{};
    std::array<std::string, 2> own_uid{};
    u64 max_seen_ms{};
    u64 last_version{};
    bool want_lobby{};
    std::string joined_huddle;
    std::chrono::steady_clock::time_point next_enter_try{};
    bool member_seen{};
    u32 local_tries{};
    std::chrono::steady_clock::time_point next_local_try{};
    u32 lan_gen{};
    LanTrack lan;
};

State& GetState() {
    static State state;
    return state;
}

const char* CollectionName(u32 collection) {
    switch (collection) {
    case RoomHash:
        return "room";
    case RoomrHash:
        return "roomr";
    case UserdocHash:
        return "userdoc";
    case MemberCollection:
        return "member";
    default:
        return "other";
    }
}

u32 HashFromPath(std::string_view path) {
    if (path == "room/doc/src") {
        return RoomHash;
    }
    if (path == "roomr/doc/src") {
        return RoomrHash;
    }
    if (path == "col/userdoc/src") {
        return UserdocHash;
    }
    return 0;
}

void PutVarint(std::vector<u8>& out, u64 value) {
    while (value >= 0x80) {
        out.push_back(static_cast<u8>(value | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<u8>(value));
}

bool GetVarint(std::span<const u8> in, std::size_t& pos, u64& value) {
    value = 0;
    for (u32 shift = 0; shift < 64; shift += 7) {
        if (pos >= in.size()) {
            return false;
        }
        const u8 byte = in[pos++];
        value |= static_cast<u64>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return true;
        }
    }
    return false;
}

bool NextField(std::span<const u8> in, std::size_t& pos, u32& field, u32& wire,
               std::span<const u8>& bytes, u64& number) {
    u64 tag{};
    if (!GetVarint(in, pos, tag)) {
        return false;
    }
    field = static_cast<u32>(tag >> 3);
    wire = static_cast<u32>(tag & 7);
    switch (wire) {
    case 0:
        return GetVarint(in, pos, number);
    case 1:
        if (in.size() - pos < 8) {
            return false;
        }
        std::memcpy(&number, in.data() + pos, 8);
        pos += 8;
        return true;
    case 2: {
        u64 length{};
        if (!GetVarint(in, pos, length) || length > in.size() - pos) {
            return false;
        }
        bytes = in.subspan(pos, static_cast<std::size_t>(length));
        pos += static_cast<std::size_t>(length);
        return true;
    }
    case 5: {
        if (in.size() - pos < 4) {
            return false;
        }
        u32 value{};
        std::memcpy(&value, in.data() + pos, 4);
        number = value;
        pos += 4;
        return true;
    }
    default:
        return false;
    }
}

void SetEntry(Entries& entries, std::string_view key, std::vector<u8> value) {
    auto it = std::ranges::find(entries, key, &Entries::value_type::first);
    if (it != entries.end()) {
        it->second = std::move(value);
    } else {
        entries.emplace_back(std::string{key}, std::move(value));
    }
}

std::optional<Entries> ParseMap(std::span<const u8> body) {
    Entries entries;
    std::size_t pos = 0;
    while (pos < body.size()) {
        u32 field{};
        u32 wire{};
        std::span<const u8> bytes;
        u64 number{};
        if (!NextField(body, pos, field, wire, bytes, number)) {
            return std::nullopt;
        }
        if (field != 1 || wire != 2) {
            continue;
        }
        std::string key;
        std::vector<u8> value;
        std::size_t inner = 0;
        while (inner < bytes.size()) {
            u32 f{};
            u32 w{};
            std::span<const u8> b;
            u64 n{};
            if (!NextField(bytes, inner, f, w, b, n)) {
                return std::nullopt;
            }
            if (f == 1 && w == 2) {
                key.assign(b.begin(), b.end());
            } else if (f == 2 && w == 2) {
                value.assign(b.begin(), b.end());
            }
        }
        SetEntry(entries, key, std::move(value));
    }
    return entries;
}

void PutBytesField(std::vector<u8>& out, u32 field, std::span<const u8> bytes) {
    PutVarint(out, (static_cast<u64>(field) << 3) | 2);
    PutVarint(out, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}

std::vector<u8> EncodeMap(const Entries& entries) {
    std::vector<u8> out;
    for (const auto& [key, value] : entries) {
        std::vector<u8> entry;
        PutBytesField(entry, 1,
                      std::span<const u8>{reinterpret_cast<const u8*>(key.data()), key.size()});
        PutBytesField(entry, 2, value);
        PutBytesField(out, 1, entry);
    }
    return out;
}

std::vector<u8> TimestampValue(u64 ms) {
    std::vector<u8> timestamp;
    PutVarint(timestamp, 1 << 3);
    PutVarint(timestamp, ms / 1000);
    PutVarint(timestamp, 2 << 3);
    PutVarint(timestamp, (ms % 1000) * 1000000);
    std::vector<u8> value;
    PutBytesField(value, 6, timestamp);
    return value;
}

std::vector<u8> StringValue(std::string_view text) {
    std::vector<u8> value;
    PutBytesField(value, 7,
                  std::span<const u8>{reinterpret_cast<const u8*>(text.data()), text.size()});
    return value;
}

std::optional<u64> ValueMilliseconds(std::span<const u8> value) {
    std::size_t pos = 0;
    while (pos < value.size()) {
        u32 field{};
        u32 wire{};
        std::span<const u8> bytes;
        u64 number{};
        if (!NextField(value, pos, field, wire, bytes, number)) {
            return std::nullopt;
        }
        if ((field == 1 || field == 3) && wire == 0) {
            return number;
        }
        if (field == 6 && wire == 2) {
            u64 seconds{};
            u64 nanos{};
            std::size_t inner = 0;
            while (inner < bytes.size()) {
                u32 f{};
                u32 w{};
                std::span<const u8> b;
                u64 n{};
                if (!NextField(bytes, inner, f, w, b, n)) {
                    return std::nullopt;
                }
                if (f == 1 && w == 0) {
                    seconds = n;
                } else if (f == 2 && w == 0) {
                    nanos = n;
                }
            }
            return seconds * 1000 + nanos / 1000000;
        }
    }
    return std::nullopt;
}

void ObserveTimes(State& state, const Entries& entries) {
    for (const auto& [key, value] : entries) {
        if (key != "uts" && key != "utime") {
            continue;
        }
        if (const auto ms = ValueMilliseconds(value);
            ms && *ms > state.max_seen_ms && *ms < 0x7FFFFFFFFFFFULL) {
            state.max_seen_ms = *ms;
        }
    }
}

u64 NextStamp(State& state) {
    const auto now = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
    const u64 stamp = std::max(now, state.max_seen_ms + 1);
    state.max_seen_ms = stamp;
    return stamp;
}

u64 NextVersion(State& state) {
    const auto now = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
    state.last_version = std::max(now, state.last_version + 1);
    return state.last_version;
}

bool IsJoined(const Network::RoomMember& member) {
    const auto room_state = member.GetState();
    return room_state == Network::RoomMember::State::Joined ||
           room_state == Network::RoomMember::State::Moderator;
}

bool EnsureActive(State& state) {
    if (!ResortLanFriendsActive()) {
        return false;
    }
    const auto member = Network::GetRoomMember().lock();
    if (!member || !IsJoined(*member)) {
        return false;
    }
    if (!StartResortDocBus()) {
        return false;
    }
    if (!state.announced) {
        state.announced = true;
        const auto identity = GetResortDocBus().Identity();
        LOG_INFO(Service_Friend, "ResortDocs active: node={:016x} lobby={:016x}", identity.node,
                 identity.lobby);
    }
    return true;
}

void MarkDirty(State& state, u32 collection) {
    switch (collection) {
    case MemberCollection:
        state.dirty_members = true;
        break;
    case UserdocHash:
        state.dirty_userdocs = true;
        state.dirty_rooms = true;
        break;
    case RoomHash:
        state.dirty_rooms = true;
        break;
    case RoomrHash:
        state.dirty_roomrs = true;
        break;
    default:
        break;
    }
}

void MarkAll(State& state) {
    state.dirty_members = state.dirty_userdocs = state.dirty_rooms = state.dirty_roomrs = true;
    state.last_members = state.last_userdocs = state.last_rooms = state.last_roomrs = 0;
}

void MaybeEnterLobby() {
    auto& bus = GetResortDocBus();
    if (bus.Identity().lobby != 0) {
        return;
    }
    if (IsResortHosting() || ResolveResortLobbyScope()) {
        const auto status = EnterResortLobby(0);
        LOG_INFO(Service_Friend, "ResortDocs auto lobby entry: status {}",
                 static_cast<u32>(status));
    }
}

template <typename T>
void Put(std::vector<u8>& out, std::size_t offset, T value) {
    std::memcpy(out.data() + offset, &value, sizeof(T));
}

std::size_t Align8(std::size_t value) {
    return (value + 7) & ~std::size_t{7};
}

void WriteHeader(Event& event, u32 kind, u32 collection, u32 gsf, u32 count) {
    Put<u32>(event.bytes, 0x00, EventMagic);
    Put<u32>(event.bytes, 0x04, kind);
    Put<u32>(event.bytes, 0x08, collection);
    Put<u32>(event.bytes, 0x0C, gsf);
    Put<u32>(event.bytes, 0x10, count);
    Put<u32>(event.bytes, 0x14, 0);
}

u64 Fingerprint(const Event& event) {
    u64 hash = 0xCBF29CE484222325ULL;
    for (std::size_t i = 0; i < event.bytes.size(); ++i) {
        if (i >= 0x18 && i < 0x20) {
            continue;
        }
        hash = (hash ^ event.bytes[i]) * 0x100000001B3ULL;
    }
    return hash | 1;
}

bool IsOwnUserdoc(const State& state, std::string_view key) {
    for (const auto& uid : state.own_uid) {
        if (!uid.empty() && key.size() > uid.size() && key.substr(0, uid.size()) == uid &&
            key[uid.size()] == '_') {
            return true;
        }
    }
    return false;
}

Event BuildSnapshot(State& state, DocBus& bus, u32 collection) {
    auto docs = bus.List(collection);
    if (collection == UserdocHash) {
        const auto identity = bus.Identity();
        std::erase_if(docs, [&](const DocView& view) {
            return view.owner == identity.node || view.owner == identity.session ||
                   IsOwnUserdoc(state, view.key);
        });
    }
    std::ranges::sort(docs, {}, &DocView::key);
    Event event;
    event.name = CollectionName(collection);
    std::size_t size = EventHeaderSize + docs.size() * SnapshotEntrySize;
    for (const auto& doc : docs) {
        size += Align8(doc.key.size() + 1) + Align8(doc.body.size());
    }
    event.bytes.assign(size, 0);
    WriteHeader(event, 1, collection, state.gsf, static_cast<u32>(docs.size()));
    std::size_t data = EventHeaderSize + docs.size() * SnapshotEntrySize;
    for (std::size_t i = 0; i < docs.size(); ++i) {
        const std::size_t slot = EventHeaderSize + i * SnapshotEntrySize;
        std::memcpy(event.bytes.data() + data, docs[i].key.data(), docs[i].key.size());
        Put<u64>(event.bytes, slot, data);
        event.pointers.push_back(static_cast<u32>(slot));
        data += Align8(docs[i].key.size() + 1);
        if (!docs[i].body.empty()) {
            std::memcpy(event.bytes.data() + data, docs[i].body.data(), docs[i].body.size());
            Put<u64>(event.bytes, slot + 8, data);
            event.pointers.push_back(static_cast<u32>(slot + 8));
        }
        Put<u64>(event.bytes, slot + 16, docs[i].body.size());
        data += Align8(docs[i].body.size());
    }
    return event;
}

void PutShortString(std::vector<u8>& out, std::size_t offset, std::string_view text) {
    out[offset] = static_cast<u8>(text.size() << 1);
    std::memcpy(out.data() + offset + 1, text.data(), text.size());
}

std::optional<Event> BuildMembers(State& state, DocBus& bus) {
    auto docs = bus.List(MemberCollection);
    std::erase_if(docs, [](const DocView& view) {
        return view.key.empty() || view.key.size() > MaxShortString || view.body.size() < 8;
    });
    const auto identity = bus.Identity();
    const auto is_own = [&](const DocView& view) {
        return view.key == state.own_uid[0] || view.key == state.own_uid[1] ||
               view.owner == identity.node || view.owner == identity.session;
    };
    if (std::ranges::none_of(docs, is_own)) {
        return std::nullopt;
    }
    std::ranges::stable_sort(docs, [&](const DocView& a, const DocView& b) {
        if (is_own(a) != is_own(b)) {
            return is_own(a);
        }
        return a.key < b.key;
    });
    if (docs.size() > MaxMembers) {
        docs.resize(MaxMembers);
    }
    const std::size_t count = docs.size();
    std::size_t size = MemberNodesStart + count * MemberNodeSize;
    for (const auto& doc : docs) {
        size += Align8(doc.body.size() - 8);
    }
    Event event;
    event.name = "members";
    event.bytes.assign(size, 0);
    WriteHeader(event, 2, 0, state.gsf, static_cast<u32>(count));
    const auto link = [](std::size_t index) {
        return MemberNodesStart + index * MemberNodeSize + 0xA8;
    };
    const u64 head = MemberListHead;
    Put<u64>(event.bytes, MemberListHead, count ? link(count - 1) : head);
    Put<u64>(event.bytes, MemberListHead + 8, count ? link(0) : head);
    event.pointers.push_back(static_cast<u32>(MemberListHead));
    event.pointers.push_back(static_cast<u32>(MemberListHead + 8));
    std::size_t data = MemberNodesStart + count * MemberNodeSize;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t node = MemberNodesStart + i * MemberNodeSize;
        const auto& doc = docs[i];
        PutShortString(event.bytes, node + 0x08, doc.key);
        PutShortString(event.bytes, node + 0x30, doc.key);
        std::memcpy(event.bytes.data() + node + 0xA0, doc.body.data(), 8);
        Put<u64>(event.bytes, node + 0xA8, i == 0 ? head : link(i - 1));
        Put<u64>(event.bytes, node + 0xB0, i + 1 == count ? head : link(i + 1));
        event.pointers.push_back(static_cast<u32>(node + 0xA8));
        event.pointers.push_back(static_cast<u32>(node + 0xB0));
        const std::size_t length = doc.body.size() - 8;
        if (length != 0) {
            std::memcpy(event.bytes.data() + data, doc.body.data() + 8, length);
            Put<u64>(event.bytes, node + 0xB8, data);
            event.pointers.push_back(static_cast<u32>(node + 0xB8));
        }
        Put<u64>(event.bytes, node + 0xC0, length);
        data += Align8(length);
    }
    return event;
}

void Queue(State& state, Event&& event, u64& last) {
    const u64 fingerprint = Fingerprint(event);
    if (fingerprint == last) {
        return;
    }
    last = fingerprint;
    Put<u64>(event.bytes, 0x18, NextVersion(state));
    LOG_DEBUG(Service_Friend, "ResortDocs event {} count={} bytes={}", event.name,
              event.bytes.size() >= 0x14 ? event.bytes[0x10] : 0, event.bytes.size());
    state.outbox.push_back(std::move(event));
}

void SyncLan(State& state, u64 lobby) {
    if (state.lan.lobby != lobby) {
        state.lan = {};
        state.lan.lobby = lobby;
    }
}

void NoteLanDoc(State& state, const DocView& doc) {
    const auto now = std::chrono::steady_clock::now();
    state.lan.seen = now;
    if (doc.body.size() < LanBodySize) {
        return;
    }
    u32 gen{};
    std::memcpy(&gen, doc.body.data(), 4);
    if (gen != state.lan.last_gen) {
        state.lan.last_gen = gen;
        state.lan.gen_since = now;
    }
}

void Drain(State& state, DocBus& bus) {
    const auto identity = bus.Identity();
    SyncLan(state, identity.lobby);
    if (identity.lobby != state.lobby) {
        LOG_INFO(Service_Friend, "ResortDocs lobby {:016x} -> {:016x}", state.lobby,
                 identity.lobby);
        if (state.lobby != 0 && identity.lobby == 0) {
            state.want_lobby = false;
        }
        state.lobby = identity.lobby;
        MarkAll(state);
    }
    for (u32 round = 0; round < 8; ++round) {
        const auto result = bus.Poll(0, state.epoch, state.cursor, 256, 0x100000, true);
        if (result.reset) {
            MarkAll(state);
        }
        state.epoch = result.epoch;
        state.cursor = result.next_cursor;
        for (const auto& change : result.changes) {
            if (change.collection == LanCollection) {
                NoteLanDoc(state, change);
            }
            MarkDirty(state, change.collection);
            if (change.collection == RoomHash || change.collection == RoomrHash ||
                change.collection == UserdocHash) {
                if (const auto entries = ParseMap(change.body)) {
                    ObserveTimes(state, *entries);
                }
            }
        }
        if (!result.more) {
            break;
        }
    }
}

void BuildEvents(State& state, DocBus& bus) {
    if (state.dirty_members) {
        if (auto members = BuildMembers(state, bus)) {
            state.dirty_members = false;
            const u64 before = state.last_members;
            Queue(state, std::move(*members), state.last_members);
            if (state.last_members != before) {
                state.dirty_userdocs = state.dirty_rooms = true;
                state.last_userdocs = state.last_rooms = 0;
            }
        }
    }
    if (state.dirty_userdocs) {
        state.dirty_userdocs = false;
        Queue(state, BuildSnapshot(state, bus, UserdocHash), state.last_userdocs);
    }
    if (state.dirty_rooms) {
        state.dirty_rooms = false;
        Queue(state, BuildSnapshot(state, bus, RoomHash), state.last_rooms);
    }
    if (state.dirty_roomrs) {
        state.dirty_roomrs = false;
        Queue(state, BuildSnapshot(state, bus, RoomrHash), state.last_roomrs);
    }
}

s32 PublishLeave(State& state, const std::string& room_id);

bool IsOwnRoom(const State& state, std::string_view id) {
    if (id.size() < 2 || id[0] != 'r') {
        return false;
    }
    const auto owner = id.substr(1);
    return owner == state.own_uid[0] || (!state.own_uid[1].empty() && owner == state.own_uid[1]);
}

bool RoomListsMember(std::span<const u8> body, const std::array<std::string, 2>& uids) {
    const auto entries = ParseMap(body);
    if (!entries) {
        return false;
    }
    const auto it =
        std::ranges::find(*entries, std::string_view{"uids"}, &Entries::value_type::first);
    if (it == entries->end()) {
        return false;
    }
    const std::string_view list{reinterpret_cast<const char*>(it->second.data()),
                                it->second.size()};
    return std::ranges::any_of(uids, [&](const std::string& uid) {
        return !uid.empty() && list.find(uid) != std::string_view::npos;
    });
}

void CloseStaleHuddles(State& state, DocBus& bus, const std::string& keep) {
    if (state.own_uid[0].empty()) {
        return;
    }
    for (const auto& doc : bus.List(RoomHash)) {
        if (doc.key == keep) {
            continue;
        }
        if (!IsOwnRoom(state, doc.key) && !RoomListsMember(doc.body, state.own_uid)) {
            continue;
        }
        LOG_INFO(Service_Friend, "ResortDocs stale huddle {} left before {}", doc.key, keep);
        PublishLeave(state, doc.key);
    }
}

s32 PublishDocument(State& state, u32 collection, const std::string& id, std::span<const u8> body,
                    bool replace, u32 kind) {
    if (collection != RoomHash && collection != RoomrHash && collection != UserdocHash) {
        LOG_DEBUG(Service_Friend, "ResortDocs ignored kind {} write to {:08x}/{}", kind, collection,
                  id);
        return 0;
    }
    if (id.empty()) {
        LOG_WARNING(Service_Friend, "ResortDocs {} write without a document id",
                    CollectionName(collection));
        return 5;
    }
    auto incoming = ParseMap(body);
    if (!incoming) {
        LOG_WARNING(Service_Friend, "ResortDocs {}/{}: body is not a MapValue",
                    CollectionName(collection), id);
        return 6;
    }
    auto& bus = GetResortDocBus();
    const bool creates_huddle = collection == RoomHash && replace && IsOwnRoom(state, id);
    const bool joins_huddle =
        kind == 2 && collection == RoomrHash && !id.ends_with("leave") && !IsOwnRoom(state, id);
    if (creates_huddle || joins_huddle) {
        CloseStaleHuddles(state, bus, id);
        state.joined_huddle = joins_huddle ? id : std::string{};
    }
    Entries merged;
    DocView current{};
    if (!replace && bus.Get(collection, id, 0, current) == Status::Ok) {
        if (auto existing = ParseMap(current.body)) {
            merged = std::move(*existing);
        }
    }
    for (auto& [key, value] : *incoming) {
        SetEntry(merged, key, std::move(value));
    }
    ObserveTimes(state, merged);
    const u64 stamp = NextStamp(state);
    SetEntry(merged, "uts", TimestampValue(stamp));
    const auto encoded = EncodeMap(merged);
    const u32 flags = collection == UserdocHash ? DocFlag::Sticky : 0;
    const auto status = bus.Publish(collection, id, encoded, flags, ExpectAny, nullptr, nullptr);
    if (status != Status::Ok) {
        LOG_WARNING(Service_Friend, "ResortDocs {}/{} publish failed: status {}",
                    CollectionName(collection), id, static_cast<u32>(status));
        return 0x10 + static_cast<s32>(status);
    }
    MarkDirty(state, collection);
    LOG_DEBUG(Service_Friend, "ResortDocs publish kind={} {}/{} {} bytes uts={} {}", kind,
              CollectionName(collection), id, encoded.size(), stamp, replace ? "replace" : "merge");
    return 0;
}

s32 PublishMember(State& state, u8 lid, const std::string& uid, u64 aux, std::span<const u8> body) {
    if (uid.empty() || uid.size() > MaxShortString || lid > 1) {
        return 7;
    }
    state.own_uid[lid] = uid;
    state.member_seen = true;
    std::vector<u8> value(8 + body.size());
    std::memcpy(value.data(), &aux, 8);
    if (!body.empty()) {
        std::memcpy(value.data() + 8, body.data(), body.size());
    }
    auto& bus = GetResortDocBus();
    DocView current{};
    if (bus.Get(MemberCollection, uid, 0, current) != Status::Ok || current.body != value) {
        const auto status =
            bus.Publish(MemberCollection, uid, value, DocFlag::Sticky, ExpectAny, nullptr, nullptr);
        if (status != Status::Ok) {
            LOG_WARNING(Service_Friend, "ResortDocs member {} publish failed: status {}", uid,
                        static_cast<u32>(status));
            return 0x10 + static_cast<s32>(status);
        }
        MarkDirty(state, MemberCollection);
        LOG_DEBUG(Service_Friend, "ResortDocs member {} lid={} key={:016x} {} bytes", uid, lid, aux,
                  body.size());
    }
    state.want_lobby = true;
    MaybeEnterLobby();
    return 0;
}

s32 PublishLeave(State& state, const std::string& room_id) {
    if (room_id.size() < 2 || room_id[0] != 'r') {
        return 8;
    }
    auto& bus = GetResortDocBus();
    if (room_id == state.joined_huddle) {
        state.joined_huddle.clear();
    }
    const std::string owner = room_id.substr(1);
    if (owner == state.own_uid[0] || owner == state.own_uid[1]) {
        bus.Delete(RoomHash, room_id, 0, ExpectAny, nullptr, nullptr);
        bus.Delete(RoomrHash, room_id, 0, ExpectAny, nullptr, nullptr);
        bus.Delete(RoomrHash, room_id + "leave", 0, ExpectAny, nullptr, nullptr);
        MarkDirty(state, RoomHash);
        MarkDirty(state, RoomrHash);
        LOG_INFO(Service_Friend, "ResortDocs huddle {} closed by its host", room_id);
        return 0;
    }
    if (state.own_uid[0].empty()) {
        return 9;
    }
    DocView room{};
    if (bus.Get(RoomHash, room_id, 0, room) != Status::Ok) {
        LOG_INFO(Service_Friend, "ResortDocs huddle {} is already closed", room_id);
        return 0;
    }
    const std::string key = room_id + "leave";
    Entries merged;
    DocView current{};
    if (bus.Get(RoomrHash, key, 0, current) == Status::Ok) {
        if (auto existing = ParseMap(current.body)) {
            merged = std::move(*existing);
        }
    }
    SetEntry(merged, "rid", StringValue(state.own_uid[0]));
    const u64 stamp = NextStamp(state);
    SetEntry(merged, "uts", TimestampValue(stamp));
    const auto status =
        bus.Publish(RoomrHash, key, EncodeMap(merged), 0, ExpectAny, nullptr, nullptr);
    MarkDirty(state, RoomrHash);
    LOG_INFO(Service_Friend, "ResortDocs {} left huddle {} (uts={}, status {})", state.own_uid[0],
             room_id, stamp, static_cast<u32>(status));
    return status == Status::Ok ? 0 : 0x10 + static_cast<s32>(status);
}

u32 IpValue(const Network::IPv4Address& ip) {
    return static_cast<u32>(ip[0]) << 24 | static_cast<u32>(ip[1]) << 16 |
           static_cast<u32>(ip[2]) << 8 | static_cast<u32>(ip[3]);
}

bool LobbyHostOnline(u32 host_ip) {
    const auto hosts = CollectResortRoomHosts();
    return std::ranges::any_of(
        hosts, [host_ip](const ResortRoomFriend& host) { return IpValue(host.ip) == host_ip; });
}

bool ValueIsTrue(std::span<const u8> value) {
    std::size_t pos = 0;
    while (pos < value.size()) {
        u32 field{};
        u32 wire{};
        std::span<const u8> bytes;
        u64 number{};
        if (!NextField(value, pos, field, wire, bytes, number)) {
            return false;
        }
        if (wire == 0 && number != 0) {
            return true;
        }
    }
    return false;
}

bool HostInPlay(DocBus& bus, u32 host_ip) {
    const std::array<std::string, 2> uids{fmt::format("{:08x}", host_ip), std::string{}};
    const std::string own_room = "r" + uids[0];
    for (const auto& doc : bus.List(RoomHash)) {
        if (doc.key != own_room && !RoomListsMember(doc.body, uids)) {
            continue;
        }
        const auto entries = ParseMap(doc.body);
        if (!entries) {
            continue;
        }
        const auto it =
            std::ranges::find(*entries, std::string_view{"iplay"}, &Entries::value_type::first);
        if (it != entries->end() && ValueIsTrue(it->second)) {
            return true;
        }
    }
    return false;
}

void PublishLan(State& state, DocBus& bus) {
    std::array<u8, LanBodySize> body{};
    const u32 up = state.lan.up ? 1 : 0;
    std::memcpy(body.data(), &state.lan_gen, 4);
    std::memcpy(body.data() + 4, &state.lan.beat, 4);
    std::memcpy(body.data() + 8, &up, 4);
    const auto status = bus.Publish(LanCollection, LanKey, body, 0, ExpectAny, nullptr, nullptr);
    if (status != Status::Ok) {
        LOG_WARNING(Service_Friend, "ResortDocs LAN session state publish failed: status {}",
                    static_cast<u32>(status));
    }
}

s32 LobbyQuery(State& state, bool has_session, bool at_start) {
    if (!EnsureActive(state)) {
        return LobbyNone;
    }
    auto& bus = GetResortDocBus();
    const auto identity = bus.Identity();
    SyncLan(state, identity.lobby);
    auto& lan = state.lan;
    if (identity.lobby == 0) {
        return LobbyNone;
    }
    const u32 host_ip = static_cast<u32>(identity.lobby);
    const auto now = std::chrono::steady_clock::now();
    if (host_ip == identity.ip) {
        const bool hosting = IsResortHosting();
        const bool up = has_session && hosting;
        const bool changed = up != lan.up;
        if (changed) {
            lan.up = up;
            if (up && ++state.lan_gen == 0) {
                ++state.lan_gen;
            }
            LOG_INFO(Service_Friend, "ResortDocs Friend Lobby LAN session {} (generation {})",
                     up ? "up" : "down", state.lan_gen);
        }
        if (changed || (up && now >= lan.next_beat)) {
            ++lan.beat;
            lan.next_beat = now + LanBeat;
            PublishLan(state, bus);
        }
        return hosting ? LobbyHost : LobbyNone;
    }
    if (!LobbyHostOnline(host_ip)) {
        return LobbyNone;
    }
    const s32 target = static_cast<s32>(Common::swap32(host_ip));
    if (target >= -16 && target <= LobbyWait) {
        return LobbyNone;
    }
    bool doc_up = false;
    u32 gen{};
    DocView doc{};
    if (bus.Get(LanCollection, LanKey, 0, doc) == Status::Ok && doc.body.size() >= LanBodySize) {
        u32 up{};
        std::memcpy(&gen, doc.body.data(), 4);
        std::memcpy(&up, doc.body.data() + 8, 4);
        doc_up = up != 0 && gen != 0;
    }
    const bool fresh = doc_up && lan.seen && now - *lan.seen <= LanStale;
    if (!has_session) {
        lan.had = false;
        lan.joined_gen = 0;
        lan.target_gen = doc_up ? gen : 0;
        lan.target_time = now;
        if (fresh || (at_start && doc_up && !HostInPlay(bus, host_ip))) {
            return target;
        }
        return LobbyWait;
    }
    if (!lan.had) {
        lan.had = true;
        lan.had_since = now;
        if (lan.target_gen != 0 && now - lan.target_time <= LanTargetLife) {
            lan.joined_gen = lan.target_gen;
        } else {
            lan.joined_gen = doc_up ? gen : 0;
        }
        lan.target_gen = 0;
    }
    if (!doc_up) {
        return LobbyWait;
    }
    if (lan.joined_gen == 0 && lan.gen_since <= lan.had_since + LanAdoptWindow) {
        lan.joined_gen = gen;
    }
    if (gen == lan.joined_gen) {
        return fresh ? target : LobbyWait;
    }
    if (lan.rejoin_gen != gen) {
        lan.rejoin_gen = gen;
        LOG_INFO(
            Service_Friend,
            "ResortDocs LAN session is from lobby host generation {}, host is on {}: rejoining",
            lan.joined_gen, gen);
    }
    return LobbyRejoin;
}

} // Anonymous namespace

s32 GameQuery(std::span<const u8> in) {
    if (in.size() < 8) {
        return -2;
    }
    u32 magic{};
    u32 kind{};
    std::memcpy(&magic, in.data(), 4);
    std::memcpy(&kind, in.data() + 4, 4);
    if (magic != QueryMagic || (kind != 1 && kind != 2)) {
        return -2;
    }
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (kind == 2) {
        u32 flags{};
        if (in.size() >= 12) {
            std::memcpy(&flags, in.data() + 8, 4);
        }
        return LobbyQuery(state, (flags & 1) != 0, (flags & 2) != 0);
    }
    return EnsureActive(state) ? 1 : 0;
}

s32 GamePublish(Core::Memory::Memory& memory, std::span<const u8> record) {
    if (record.size() < RecordSize) {
        return 1;
    }
    u32 magic{};
    u32 kind{};
    u32 hash{};
    u32 word{};
    u64 id_ptr{};
    u64 path_ptr{};
    u64 body_ptr{};
    u64 body_len{};
    u64 aux{};
    std::memcpy(&magic, record.data() + 0x00, 4);
    std::memcpy(&kind, record.data() + 0x04, 4);
    std::memcpy(&hash, record.data() + 0x08, 4);
    std::memcpy(&word, record.data() + 0x0C, 4);
    std::memcpy(&id_ptr, record.data() + 0x10, 8);
    std::memcpy(&path_ptr, record.data() + 0x18, 8);
    std::memcpy(&body_ptr, record.data() + 0x20, 8);
    std::memcpy(&body_len, record.data() + 0x28, 8);
    std::memcpy(&aux, record.data() + 0x30, 8);
    if (magic != RecordMagic) {
        return 2;
    }
    if (body_len > MaxBodySize) {
        LOG_WARNING(Service_Friend, "ResortDocs kind {} body of {} bytes is too large", kind,
                    body_len);
        return 3;
    }
    std::vector<u8> body(static_cast<std::size_t>(body_len));
    if (!body.empty() && (body_ptr == 0 || !memory.IsValidVirtualAddressRange(body_ptr, body_len) ||
                          !memory.ReadBlock(body_ptr, body.data(), body.size()))) {
        return 3;
    }
    const std::string id = id_ptr != 0 ? memory.ReadCString(id_ptr, MaxIdLength) : std::string{};
    const std::string path =
        path_ptr != 0 ? memory.ReadCString(path_ptr, MaxPathLength) : std::string{};
    const u8 user = static_cast<u8>(word);
    const u8 mode_d = static_cast<u8>(word >> 8);
    const u8 gsf = static_cast<u8>(word >> 24);

    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (!EnsureActive(state)) {
        return 1;
    }
    if (gsf != 0xFF && kind == 3) {
        state.gsf = gsf;
    }
    switch (kind) {
    case 1:
        return PublishDocument(state, hash, id, body, mode_d == 1, kind);
    case 2:
        return PublishDocument(state, HashFromPath(path), id, body, false, kind);
    case 3:
        return PublishMember(state, user, id, aux, body);
    case 4:
        return PublishLeave(state, id);
    default:
        return 4;
    }
}

s32 GamePoll(u64 out_address, u32 capacity, std::vector<u8>& out) {
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (!EnsureActive(state)) {
        return 0;
    }
    auto& bus = GetResortDocBus();
    Drain(state, bus);
    if (TakeDueResortLobbyLeave()) {
        const auto identity = bus.Identity();
        const u32 lobby_host = static_cast<u32>(identity.lobby);
        if (identity.lobby != 0 && lobby_host != identity.ip &&
            RecentResortUnicastTo(lobby_host, 1000)) {
            LOG_INFO(Service_Friend,
                     "ResortDocs staying in lobby {:016x}: still connected to its host",
                     identity.lobby);
        } else if (identity.lobby != 0) {
            bus.LeaveLobby();
        }
    }
    if (const auto target = TakeResortUnicastTarget(); target && !IsResortHosting()) {
        const u64 scope = LobbyScopeFromHostIp(IpValue(*target));
        if (state.lobby != scope) {
            for (const auto& host : CollectResortRoomHosts()) {
                if (host.ip != *target) {
                    continue;
                }
                if (state.lobby != 0) {
                    bus.LeaveLobby();
                }
                const auto status = EnterResortLobby(scope);
                LOG_INFO(Service_Friend,
                         "ResortDocs joined {}'s lobby from game traffic: status {}", host.nickname,
                         static_cast<u32>(status));
                break;
            }
        }
    }
    if (state.want_lobby && state.lobby == 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= state.next_enter_try) {
            state.next_enter_try = now + std::chrono::seconds{2};
            MaybeEnterLobby();
        }
    }
    if (!state.member_seen) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= state.next_local_try) {
            state.next_local_try = now + LocalMemberRetry;
            if (++state.local_tries == 1) {
                LOG_INFO(Service_Friend, "ResortDocs asking the game to register its local player");
            }
            Event event;
            event.name = "local";
            event.bytes.assign(EventHeaderSize, 0);
            WriteHeader(event, LocalMemberKind, 0, state.gsf, 0);
            Put<u64>(event.bytes, 0x18, NextVersion(state));
            state.outbox.push_front(std::move(event));
        }
    }
    if (state.outbox.empty()) {
        BuildEvents(state, bus);
    }
    while (!state.outbox.empty()) {
        Event event = std::move(state.outbox.front());
        state.outbox.pop_front();
        if (event.bytes.size() > capacity) {
            LOG_ERROR(Service_Friend, "ResortDocs {} event of {} bytes dropped: buffer is {} bytes",
                      event.name, event.bytes.size(), capacity);
            continue;
        }
        for (const u32 offset : event.pointers) {
            u64 value{};
            std::memcpy(&value, event.bytes.data() + offset, 8);
            value += out_address;
            std::memcpy(event.bytes.data() + offset, &value, 8);
        }
        out = std::move(event.bytes);
        return static_cast<s32>(out.size());
    }
    return 0;
}

bool JoinedHuddleClosed() {
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    auto& bus = GetResortDocBus();
    if (state.joined_huddle.empty() || !bus.IsActive() || state.lobby == 0) {
        return false;
    }
    DocView room{};
    return bus.Get(RoomHash, state.joined_huddle, 0, room) != Status::Ok;
}

void ResetGameAdapter() {
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.joined_huddle.clear();
    state.announced = false;
    state.epoch = 0;
    state.cursor = 0;
    state.lobby = 0;
    MarkAll(state);
    state.outbox.clear();
    state.gsf = 0;
    state.own_uid = {};
    state.max_seen_ms = 0;
    state.last_version = 0;
    state.want_lobby = false;
    state.next_enter_try = {};
    state.member_seen = false;
    state.local_tries = 0;
    state.next_local_try = {};
    state.lan = {};
}

} // namespace Service::Friend::ResortBus
