// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>

#include "common/game_presets.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/resort_bus.h"
#include "core/hle/service/friend/resort_doc_adapter.h"
#include "core/memory.h"

namespace Core::HLE::ResortBus {

namespace {

constexpr u64 MaxInput = 0x200;
constexpr u64 MaxOutput = 0x10000;
constexpr auto StationWaitRestart = std::chrono::seconds{1};
constexpr auto StationWaitTimeout = std::chrono::seconds{20};

bool ReadInput(Core::Memory::Memory& memory, u64 address, u64 size, std::vector<u8>& out) {
    if (size == 0 || size > MaxInput || address + size < address ||
        !memory.IsValidVirtualAddressRange(address, size)) {
        return false;
    }
    out.resize(size);
    return memory.ReadBlock(address, out.data(), out.size());
}

s32 StationWait(u32 stations, u32 members) {
    static std::mutex mutex;
    static std::chrono::steady_clock::time_point start{};
    static std::chrono::steady_clock::time_point last{};
    static bool timed_out = false;
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock{mutex};
    if (now - last > StationWaitRestart) {
        start = now;
        timed_out = false;
    }
    last = now;
    if (now - start < StationWaitTimeout) {
        return 0;
    }
    if (!timed_out) {
        timed_out = true;
        LOG_WARNING(Service_Friend,
                    "ResortBus next match station wait timed out with {} of {} members, starting",
                    stations, members);
    }
    return 1;
}

s32 FilterGameError(s32 kind, u32 raiser) {
    if (raiser == 0 && kind == 9 && Service::Friend::ResortBus::JoinedHuddleClosed()) {
        return 0;
    }
    return kind == 2 || (raiser == 0 && (kind == 9 || kind == 14)) ? 1 : 0;
}

} // Anonymous namespace

s32 HandleSvc(Core::System& system, Kernel::KProcess& process, std::span<const u64, 8> args) {
    if (Settings::GetCurrentProgramID() != Settings::SwitchSportsResortProgramId) {
        return ResultInactive;
    }
    auto& memory = process.GetMemory();
    const auto op = static_cast<Op>(static_cast<u32>(args[0]));
    switch (op) {
    case Op::Query: {
        std::vector<u8> in;
        if (!ReadInput(memory, args[1], args[2], in)) {
            return ResultBadBuffer;
        }
        return Service::Friend::ResortBus::GameQuery(in);
    }
    case Op::Publish: {
        std::vector<u8> in;
        if (!ReadInput(memory, args[1], args[2], in)) {
            return ResultBadBuffer;
        }
        return Service::Friend::ResortBus::GamePublish(memory, in);
    }
    case Op::Poll: {
        const u64 out_address = args[3];
        const u64 capacity = std::min<u64>(static_cast<u32>(args[4]), MaxOutput);
        if (capacity == 0 || out_address + capacity < out_address ||
            !memory.IsValidVirtualAddressRange(out_address, capacity)) {
            return ResultBadBuffer;
        }
        std::vector<u8> out;
        const s32 result =
            Service::Friend::ResortBus::GamePoll(out_address, static_cast<u32>(capacity), out);
        if (result <= 0) {
            return result;
        }
        if (out.size() != static_cast<std::size_t>(result) || out.size() > capacity ||
            !memory.WriteBlock(out_address, out.data(), out.size())) {
            LOG_WARNING(Service_Friend, "ResortBus poll result {} does not fit {} bytes", result,
                        capacity);
            return ResultTooSmall;
        }
        return result;
    }
    case Op::RaiseFilter:
        return FilterGameError(static_cast<s32>(static_cast<u32>(args[1])),
                               static_cast<u32>(args[2]));
    case Op::StationWait:
        return StationWait(static_cast<u32>(args[1]), static_cast<u32>(args[2]));
    }
    return ResultBadOp;
}

} // namespace Core::HLE::ResortBus
