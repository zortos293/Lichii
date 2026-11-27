// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>

#include "common/common_types.h"
#include "common/uuid.h"

namespace FrontendCommon::CloudSync {

// What a save or profile looked like when it was last the same here and on Drive: the base the
// three-way decision compares both sides with.
struct SyncedVersion {
    std::string file_id;
    std::string content_hash;
    // Drive's modifiedTime of that version.
    std::string modified_time;
};

// This machine's sync state, ConfigDir/cloud_sync/state.json.
struct SyncState {
    // By SaveKey.
    std::map<std::string, SyncedVersion> saves;
    // By Drive user id.
    std::map<std::string, SyncedVersion> profiles;
    // Local profile uuid to the Drive user id it was linked to (both FormattedString).
    std::map<std::string, std::string> links;
    // The web service login's.
    std::optional<SyncedVersion> web_service_login;
    // Unix time the last sync finished without errors, 0 before the first.
    s64 last_sync{};
};

// The userUuid a local profile has on Drive: the remote profile it was linked to, else its own.
std::string DriveUser(const SyncState& state, const Common::UUID& user);

// "<title id>/<Drive user id>", the Drive user id being the userUuid appProperty or "device".
std::string SaveKey(u64 title_id, const std::string& drive_user);

enum class SyncAction { None, Push, Pull, Conflict };

// Compares the content hashes of the two sides with the last synced one. A side that is missing
// takes the other's: nothing is deleted because one side lacks it.
SyncAction DecideSync(const std::optional<std::string>& local,
                      const std::optional<std::string>& remote,
                      const std::optional<std::string>& base);

// Held while reading and writing the state, so that two syncs don't overwrite each other's.
std::timed_mutex& SyncStateMutex();

// Waits for SyncStateMutex until it is free or stop is requested; the lock owns nothing then.
std::unique_lock<std::timed_mutex> LockSyncState(std::stop_token stop);

// An empty state when the file is missing. nullopt, logged, when it exists but can't be read:
// syncing without its bases and links could overwrite changes.
std::optional<SyncState> LoadSyncState();

// Replaces the file in one rename, keeping the old one when the new one can't be fully written.
// False then.
bool SaveSyncState(const SyncState& state);

} // namespace FrontendCommon::CloudSync
