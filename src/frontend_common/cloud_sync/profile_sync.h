// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "common/uuid.h"
#include "frontend_common/cloud_sync/sync_engine.h"
#include "frontend_common/cloud_sync/sync_state.h"

namespace Service::Account {
class ProfileManager;
}

// Profiles on Drive: Lichii/Profiles/<Nickname>/ (lichii=profile, userUuid) holding
// profile.json (lichii=profileJson, userUuid, contentHash of profile.json) and avatar.jpg
// (lichii=avatar, userUuid). profile.json names its avatar by hash, so a pull takes only the
// avatar it was published with. A profile pulled to a machine that lacks it is added with the same
// uuid, so its saves line up; one that replaces a local profile backs that up first
// (ConfigDir/cloud_sync/backups/profiles/<uuid>/). A local profile that is the same person as a
// Drive profile under another uuid can be linked to it instead: it then syncs, saves included, as
// that Drive profile, without changing its local uuid or moving its saves. SyncAll syncs the
// profiles before the saves. Local profiles and avatars are never removed: a Drive profile without
// an avatar leaves the local one, which then differs from Drive and goes up on the next sync.
namespace FrontendCommon::CloudSync {

struct RemoteProfile {
    Common::UUID uuid;
    std::string nickname;
};

// The profiles on Drive, to pick one to link to. nullopt when Drive fails or stop is requested.
std::optional<std::vector<RemoteProfile>> ListRemoteProfiles(std::stop_token stop = {});

// Makes the local profile sync as the remote one, or as itself again when remote is nullopt. The
// next sync of either Drive identity then starts without a base, so differing contents are a
// conflict. False when another local profile already syncs as remote, or the state can't be read
// or saved.
bool LinkProfile(const Service::Account::ProfileManager& profiles, const Common::UUID& local,
                 std::optional<Common::UUID> remote);

// The remote profile the local one is linked to, if any.
std::optional<Common::UUID> LinkedProfile(const Common::UUID& local);

std::string ProfileNickname(const Service::Account::ProfileManager& profiles,
                            const Common::UUID& user);

// For SyncAll, with SyncStateMutex held. False when Drive couldn't be reached.
bool SyncProfiles(Service::Account::ProfileManager& profiles, const SyncCallbacks& callbacks,
                  SyncState& state, SyncResult& result, std::stop_token stop);

} // namespace FrontendCommon::CloudSync
