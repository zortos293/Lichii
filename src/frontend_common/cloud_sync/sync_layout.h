// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "frontend_common/cloud_sync/drive_client.h"
#include "frontend_common/cloud_sync/sync_state.h"

// Lichii's folders on Drive. Names are only for the user: everything is found by appProperties,
// so renamed or moved files and folders are still found.
//   Lichii/                     lichii=root
//     Profiles/                 lichii=profiles
//     Saves/                    lichii=saves
//       <Game name>/            lichii=game, titleId
//         <Nickname>.zip        lichii=save, titleId, userUuid, contentHash
//         Shared.zip            lichii=save, titleId, userUuid=device, contentHash
namespace FrontendCommon::CloudSync {

constexpr const char* KindKey = "lichii";
constexpr const char* TitleIdKey = "titleId";
constexpr const char* UserKey = "userUuid";
constexpr const char* HashKey = "contentHash";
// The userUuid of a device save.
constexpr const char* DeviceUser = "device";

// The first folder with the properties, else a new one named name in parent. nullopt when Drive
// fails.
std::optional<std::string> FindOrCreateFolder(const Drive::AppProperties& properties,
                                              std::string_view name, std::string_view parent,
                                              std::stop_token stop);

// The id of the folder of that kind ("profiles", "saves") in Lichii/, created with name when
// missing, along with Lichii/.
std::optional<std::string> LayoutFolder(std::string_view kind, std::string_view name,
                                        std::stop_token stop);

// Of several files with one identity (two machines that made it at once), the one every machine
// picks: the first created, then the lowest id. files must not be empty.
const Drive::File& CanonicalFile(std::span<const Drive::File* const> files);

// The contentHash appProperty, empty when there is none.
std::string FileHash(const Drive::File& file);

// The identity and base rules saves and profiles share.

// Drive's file for one identity (a save, a profile), as every machine picks it.
struct RemoteFile {
    std::optional<Drive::File> file;
    // The files with its contents, it included: a base recorded for any of them applies to it.
    std::vector<std::string> same_contents;
    // A duplicate with other contents, kept as it is and to be reported. Empty when there is none.
    std::string differing_id;
    // Looking up the synced file failed; Drive::LastError says why.
    bool failed{};
};

// Picks among the files the search found for the identity (CanonicalFile). When it found none but
// one was synced, that one is looked up by id, as a search can miss a file changed a moment ago;
// it counts if it isn't in the trash and still has the identity's properties.
RemoteFile FindRemoteFile(std::span<const Drive::File* const> found, const SyncedVersion* synced,
                          const Drive::AppProperties& identity, std::stop_token stop);

// The base for the decision: the synced hash, when it was recorded for Drive's file or one with
// its contents. A base of a duplicate with other contents says nothing about the file synced now.
std::optional<std::string> ApplicableBase(const SyncedVersion* synced, const RemoteFile& remote);

// Whether sides found equal must be recorded as the base: there is none, or it names another file
// or other contents (both sides changed the same way, or an earlier record failed).
bool BaseOutdated(const SyncedVersion* synced, const Drive::File& remote, std::string_view hash);

// A Drive modifiedTime (RFC 3339, UTC). The epoch when it can't be read.
std::chrono::system_clock::time_point DriveTime(std::string_view rfc3339);

// The newest modification time of the path or anything in it.
std::chrono::system_clock::time_point LocalTime(const std::filesystem::path& path);

} // namespace FrontendCommon::CloudSync
