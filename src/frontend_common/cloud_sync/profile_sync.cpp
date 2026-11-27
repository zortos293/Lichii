// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <tuple>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/fs/path_util.h"
#include "common/string_util.h"
#include "core/hle/service/acc/profile_manager.h"
#include "frontend_common/cloud_sync/drive_client.h"
#include "frontend_common/cloud_sync/profile_sync.h"
#include "frontend_common/cloud_sync/save_archive.h"
#include "frontend_common/cloud_sync/sync_layout.h"

namespace FrontendCommon::CloudSync {

namespace {

constexpr std::string_view JsonMimeType = "application/json";
constexpr std::string_view JpegMimeType = "image/jpeg";
constexpr std::string_view AvatarFolder = "system/save/8000000000000010/su/avators";

std::filesystem::path AvatarPath(const Common::UUID& user) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) / AvatarFolder /
           fmt::format("{}.jpg", user.FormattedString());
}

std::filesystem::path ProfilesFilePath() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) / AvatarFolder / "profiles.dat";
}

std::optional<Common::UUID> ParseUuid(std::string_view text) {
    // FormattedString's shape; the UUID constructor asserts on other lengths and on non-hex digits.
    if (text.size() != 36) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? text[i] != '-' : !std::isxdigit(static_cast<unsigned char>(text[i]))) {
            return std::nullopt;
        }
    }
    const Common::UUID uuid{text};
    return uuid.IsValid() ? std::optional{uuid} : std::nullopt;
}

// A file that may be missing: no data when it is, nullopt when it is there but can't be read.
std::optional<std::optional<std::vector<u8>>> ReadOptionalFile(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) && !ec) {
        return std::optional<std::vector<u8>>{};
    }
    std::ifstream file{path, std::ios::binary};
    std::vector<u8> data{std::istreambuf_iterator<char>{file}, {}};
    if (!file.is_open() || file.bad()) {
        return std::nullopt;
    }
    return std::optional{std::move(data)};
}

bool WriteFile(const std::filesystem::path& path, std::span<const u8> data) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    file.close();
    return !file.fail();
}

std::span<const u8> Bytes(std::string_view text) {
    return {reinterpret_cast<const u8*>(text.data()), text.size()};
}

std::string StringField(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

template <typename T>
T NumberField(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_number_unsigned() ? it->get<T>() : T{};
}

std::string AvatarHash(std::span<const u8> avatar) {
    const ZipEntry entry{"avatar.jpg", {avatar.begin(), avatar.end()}};
    return ContentHash({&entry, 1});
}

std::string JsonHash(std::string_view json) {
    const ZipEntry entry{"profile.json", {json.begin(), json.end()}};
    return ContentHash({&entry, 1});
}

// A profile as it is synced. profile.json names the avatar by its hash (empty without one), so
// its own hash, the contentHash on Drive, covers both, and a pull can tell whether the avatar it
// downloaded is the one that profile.json was published with.
struct ProfileFiles {
    std::string json;
    std::optional<std::vector<u8>> avatar;
    std::string content_hash;
};

// nullopt when the avatar can't be read.
std::optional<ProfileFiles> LocalProfileFiles(const Service::Account::ProfileManager& profiles,
                                              const Common::UUID& user,
                                              const std::string& drive_user) {
    auto avatar = ReadOptionalFile(AvatarPath(user));
    if (!avatar) {
        return std::nullopt;
    }
    Service::Account::ProfileBase base{};
    Service::Account::UserData data{};
    profiles.GetProfileBaseAndData(user, base, data);
    ProfileFiles files{
        .json = nlohmann::json{{"uuid", drive_user},
                               {"nickname", ProfileNickname(profiles, user)},
                               {"creationTime", u64{base.timestamp}},
                               {"iconId", data.icon_id},
                               {"bgColorId", data.bg_color_id},
                               {"avatarHash", *avatar ? AvatarHash(**avatar) : std::string{}}}
                    .dump(),
        .avatar = std::move(*avatar),
    };
    files.content_hash = JsonHash(files.json);
    return files;
}

// Copies the local profile's profile.json and avatar to
// ConfigDir/cloud_sync/backups/profiles/<uuid>/<milliseconds>/, keeping the last BackupsKept.
bool BackUpProfile(const ProfileFiles& files, const Common::UUID& user) {
    const auto backups = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "cloud_sync" /
                         "backups" / "profiles" / user.FormattedString();
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    // A folder of its own, even when the clock gives the same time twice.
    std::error_code ec;
    std::filesystem::create_directories(backups, ec);
    auto folder = backups / fmt::format("{:015}", now);
    for (int suffix = 1; !std::filesystem::create_directory(folder, ec); ++suffix) {
        if (ec || suffix > 100) {
            return false;
        }
        folder = backups / fmt::format("{:015}-{}", now, suffix);
    }
    if (!WriteFile(folder / "profile.json", Bytes(files.json)) ||
        (files.avatar && !WriteFile(folder / "avatar.jpg", *files.avatar))) {
        return false;
    }
    std::vector<std::filesystem::path> kept;
    for (const auto& entry : std::filesystem::directory_iterator{backups, ec}) {
        kept.push_back(entry.path());
    }
    std::ranges::sort(kept);
    for (std::size_t i = 0; i + BackupsKept < kept.size(); ++i) {
        if (kept[i] != folder) {
            std::filesystem::remove_all(kept[i], ec);
        }
    }
    return true;
}

// The avatars with the identity in the profile's folder: those whose contentHash is hash first,
// then the others, each oldest first. Two machines that published a profile at once can each
// leave an avatar there.
std::vector<const Drive::File*> AvatarsIn(const std::vector<const Drive::File*>& avatars,
                                          std::string_view folder, std::string_view hash) {
    std::vector<const Drive::File*> inside;
    for (const Drive::File* avatar : avatars) {
        if (std::ranges::find(avatar->parents, folder) != avatar->parents.end()) {
            inside.push_back(avatar);
        }
    }
    std::ranges::sort(inside, {}, [hash](const Drive::File* avatar) {
        return std::tuple{FileHash(*avatar) != hash, avatar->created_time, avatar->id};
    });
    return inside;
}

struct ProfilesSync {
    Service::Account::ProfileManager& profiles;
    const SyncCallbacks& callbacks;
    SyncState& state;
    SyncResult& result;
    std::stop_token stop;
    // Of each local profile, captured before this sync changes any.
    std::map<std::string, std::chrono::system_clock::time_point> local_times{};
    // The avatar Push uploaded for the profile being synced, taken again by a retry.
    std::optional<Drive::File> uploaded_avatar{};

    void Error(std::string message) {
        result.errors.push_back(std::move(message));
    }

    // For a failed Drive call: cancelled when stop made it fail.
    void DriveError(std::string_view message) {
        if (stop.stop_requested()) {
            result.cancelled = true;
        } else {
            Error(fmt::format("{}: {}", message, Drive::LastError()));
        }
    }

    void Record(const std::string& drive_user, SyncedVersion version) {
        state.profiles[drive_user] = std::move(version);
        if (!SaveSyncState(state)) {
            Error("Could not save the cloud sync state");
        }
    }

    // False, as SyncOnce, when Drive's profile.json changed since it was read: json is then the
    // current one.
    bool Push(const std::string& drive_user, const Common::UUID& user, const ProfileFiles& files,
              std::optional<Drive::File>& json, const std::vector<const Drive::File*>& avatars) {
        const std::string nickname = ProfileNickname(profiles, user);
        const auto fail = [&] {
            DriveError(fmt::format("Could not upload the profile {}", nickname));
            return true;
        };
        std::optional<std::string> folder;
        if (json && !json->parents.empty()) {
            folder = json->parents.front();
        } else if (const auto parent = LayoutFolder("profiles", "Profiles", stop)) {
            folder = FindOrCreateFolder({{KindKey, "profile"}, {UserKey, drive_user}}, nickname,
                                        *parent, stop);
        }
        if (!folder) {
            return fail();
        }
        // The avatar first: profile.json, written last, publishes the profile. A pull checks the
        // avatar against the hash in it, so an upload cut short between the two is never taken.
        // A changed avatar is a new file, so the one the published profile.json names stays
        // readable until it is replaced. Without a local avatar, Drive's is left as it is (sync
        // deletes no avatar), and profile.json says there is none.
        if (files.avatar) {
            const std::string avatar_hash = AvatarHash(*files.avatar);
            const auto inside = AvatarsIn(avatars, *folder, avatar_hash);
            const bool already_uploaded = uploaded_avatar &&
                                          FileHash(*uploaded_avatar) == avatar_hash &&
                                          std::ranges::find(uploaded_avatar->parents, *folder) !=
                                              uploaded_avatar->parents.end();
            if (!already_uploaded && (inside.empty() || FileHash(*inside.front()) != avatar_hash)) {
                uploaded_avatar = Drive::UploadFile(
                    "avatar.jpg", *folder,
                    {{KindKey, "avatar"}, {UserKey, drive_user}, {HashKey, avatar_hash}},
                    JpegMimeType, *files.avatar, stop);
                if (!uploaded_avatar) {
                    return fail();
                }
            }
        }
        // As for saves (sync_engine.cpp): read again just before replacing, as Drive v3 has no
        // conditional update. After the avatar, whose upload another device can outlast.
        if (json) {
            const auto current = Drive::GetFile(json->id, stop);
            if (!current) {
                return fail();
            }
            if (current->trashed || FileHash(*current) != FileHash(*json) ||
                current->head_revision_id != json->head_revision_id) {
                json = current->trashed ? std::nullopt : current;
                return false;
            }
        }
        const Drive::AppProperties properties{
            {KindKey, "profileJson"}, {UserKey, drive_user}, {HashKey, files.content_hash}};
        const auto uploaded =
            json ? Drive::UpdateFile(json->id, properties, JsonMimeType, Bytes(files.json), stop)
                 : Drive::UploadFile("profile.json", *folder, properties, JsonMimeType,
                                     Bytes(files.json), stop);
        if (!uploaded) {
            return fail();
        }
        Record(drive_user, {uploaded->id, files.content_hash, uploaded->modified_time});
        ++result.pushed;
        return true;
    }

    // Sets the local profile (made with the Drive uuid when user is nullopt) to Drive's. The
    // profile it replaces is backed up first, the avatar is written beside the old one and only
    // swapped in once profiles.dat took the rest, and a step that fails puts back what it changed.
    // False when what was downloaded isn't the version the decision was made on.
    bool Pull(const std::string& drive_user, std::optional<Common::UUID> user,
              const std::optional<ProfileFiles>& local, const Drive::File& json,
              const std::vector<const Drive::File*>& avatars) {
        const bool create = !user;
        if (create) {
            user = ParseUuid(drive_user);
            if (!user || profiles.UserExists(*user)) {
                Error(fmt::format("Could not add the Drive profile {}: its id is taken here",
                                  drive_user));
                return true;
            }
        }
        const auto text = Drive::DownloadFile(json.id, stop);
        if (!text) {
            DriveError(fmt::format("Could not download the Drive profile {}", drive_user));
            return true;
        }
        const std::string_view json_text{reinterpret_cast<const char*>(text->data()), text->size()};
        if (JsonHash(json_text) != FileHash(json)) {
            return false;
        }
        const auto profile = nlohmann::json::parse(json_text, nullptr, false);
        const std::string nickname =
            profile.is_object() ? StringField(profile, "nickname") : std::string{};
        if (nickname.empty()) {
            Error(fmt::format("The Drive profile {} is unreadable", drive_user));
            return true;
        }
        const std::string avatar_hash = StringField(profile, "avatarHash");
        std::optional<std::vector<u8>> avatar_data;
        if (!avatar_hash.empty()) {
            // The one profile.json was published with, checked by its contents.
            const auto inside = json.parents.empty()
                                    ? std::vector<const Drive::File*>{}
                                    : AvatarsIn(avatars, json.parents.front(), avatar_hash);
            // One that can't be downloaded (deleted since it was listed) leaves the others.
            std::string download_error;
            for (const Drive::File* avatar : inside) {
                auto data = Drive::DownloadFile(avatar->id, stop);
                if (!data) {
                    if (stop.stop_requested()) {
                        result.cancelled = true;
                        return true;
                    }
                    download_error = Drive::LastError();
                    continue;
                }
                if (AvatarHash(*data) == avatar_hash) {
                    avatar_data = std::move(data);
                    break;
                }
            }
            if (!avatar_data && !download_error.empty()) {
                Error(fmt::format("Could not download the avatar of {}: {}", nickname,
                                  download_error));
                return true;
            }
            if (!avatar_data) {
                Error(fmt::format("The avatar of {} on Google Drive isn't the one its profile was "
                                  "uploaded with yet; try again later",
                                  nickname));
                return true;
            }
        }
        if (stop.stop_requested()) {
            result.cancelled = true;
            return true;
        }

        if (local && !BackUpProfile(*local, *user)) {
            Error(fmt::format("Could not back up the profile {} before replacing it",
                              ProfileNickname(profiles, *user)));
            return true;
        }
        const auto avatar_path = AvatarPath(*user);
        auto staged_avatar = avatar_path;
        staged_avatar += ".sync";
        std::error_code ec;
        if (avatar_data && !WriteFile(staged_avatar, *avatar_data)) {
            std::filesystem::remove(staged_avatar, ec);
            Error(fmt::format("Could not write the avatar of {}", nickname));
            return true;
        }

        Service::Account::ProfileBase old_base{};
        Service::Account::UserData old_data{};
        if (create) {
            if (profiles.CreateNewUser(*user, nickname).IsError()) {
                Error(fmt::format("Could not add the Drive profile {}", nickname));
                std::filesystem::remove(staged_avatar, ec);
                return true;
            }
        }
        profiles.GetProfileBaseAndData(*user, old_base, old_data);
        // Puts back the profile as it was in memory and in profiles.dat, as far as it can.
        bool saved = false;
        const auto undo = [&] {
            if (create && saved) {
                profiles.RemoveUser(*user);
            } else if (create) {
                profiles.RemoveUnsavedUser(*user);
            } else {
                profiles.SetProfileBaseAndData(*user, old_base, old_data);
            }
            std::filesystem::remove(staged_avatar, ec);
        };
        Service::Account::ProfileBase base = old_base;
        Service::Account::UserData data = old_data;
        base.timestamp = NumberField<u64>(profile, "creationTime");
        base.username.fill(0);
        std::copy_n(nickname.begin(), std::min(nickname.size(), base.username.size()),
                    base.username.begin());
        data.icon_id = NumberField<u32>(profile, "iconId");
        data.bg_color_id = NumberField<u8>(profile, "bgColorId");
        // SetProfileBaseAndData writes profiles.dat but doesn't say whether it could: the second
        // write does, as a failed one leaves the changes to write.
        if (!profiles.SetProfileBaseAndData(*user, base, data) || !profiles.WriteUserSaveFile()) {
            undo();
            Error(fmt::format("Could not save the profile {}", nickname));
            return true;
        }
        saved = true;
        if (avatar_data) {
            std::filesystem::rename(staged_avatar, avatar_path, ec);
            if (ec) {
                undo();
                Error(fmt::format("Could not replace the avatar of {}", nickname));
                return true;
            }
        }
        Record(drive_user, {json.id, FileHash(json), json.modified_time});
        ++result.pulled;
        return true;
    }

    // Decides and acts on the profile once. False when Drive's profile.json turned out to have
    // changed since it was read; found.file is then the current one (nullopt when it was trashed).
    bool SyncOnce(const std::string& drive_user, std::optional<Common::UUID> user,
                  RemoteFile& found, const std::vector<const Drive::File*>& avatars) {
        std::optional<ProfileFiles> local;
        if (user) {
            local = LocalProfileFiles(profiles, *user, drive_user);
            if (!local) {
                Error(fmt::format("Could not read the avatar of {}",
                                  ProfileNickname(profiles, *user)));
                return true;
            }
        }
        std::optional<Drive::File>& json = found.file;
        const std::optional<std::string> remote_hash =
            json ? std::optional{FileHash(*json)} : std::nullopt;
        const auto synced_entry = state.profiles.find(drive_user);
        const SyncedVersion* synced =
            synced_entry != state.profiles.end() ? &synced_entry->second : nullptr;
        const std::optional<std::string> base = ApplicableBase(synced, found);
        const std::optional<std::string> local_hash =
            local ? std::optional{local->content_hash} : std::nullopt;

        SyncAction action = DecideSync(local_hash, remote_hash, base);
        if (action == SyncAction::None) {
            if (local && BaseOutdated(synced, *json, *local_hash)) {
                Record(drive_user, {json->id, *local_hash, json->modified_time});
            }
            return true;
        }
        if (action == SyncAction::Conflict) {
            const Conflict conflict{
                .nickname = ProfileNickname(profiles, *user),
                .local_time = local_times[user->FormattedString()],
                .remote_time = DriveTime(json->modified_time),
            };
            switch (callbacks.resolve_conflict(conflict, stop)) {
            case ConflictChoice::KeepLocal:
                action = SyncAction::Push;
                break;
            case ConflictChoice::KeepRemote:
                action = SyncAction::Pull;
                break;
            case ConflictChoice::Skip:
                ++result.skipped;
                return true;
            }
        }
        if (action == SyncAction::Push) {
            if (Push(drive_user, *user, *local, json, avatars)) {
                return true;
            }
            found.same_contents.clear();
            return false;
        }
        if (Pull(drive_user, user, local, *json, avatars)) {
            return true;
        }
        const auto current = Drive::GetFile(json->id, stop);
        if (!current) {
            DriveError(fmt::format("Could not download the Drive profile {}", drive_user));
            return true;
        }
        json = current->trashed ? std::nullopt : current;
        found.same_contents.clear();
        return false;
    }

    void Sync(const std::string& drive_user, std::optional<Common::UUID> user, RemoteFile found,
              const std::vector<const Drive::File*>& avatars) {
        uploaded_avatar.reset();
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (SyncOnce(drive_user, user, found, avatars)) {
                return;
            }
        }
        Error(fmt::format("The Drive profile {} kept changing; try again later", drive_user));
    }
};

} // Anonymous namespace

std::optional<std::vector<RemoteProfile>> ListRemoteProfiles(std::stop_token stop) {
    const auto found = Drive::FindFiles({{KindKey, "profileJson"}}, {}, stop);
    if (!found) {
        return std::nullopt;
    }
    // Each one's profile.json as sync picks it.
    std::map<std::string, std::vector<const Drive::File*>> by_user;
    for (const Drive::File& file : *found) {
        if (const auto user = file.app_properties.find(UserKey);
            user != file.app_properties.end()) {
            by_user[user->second].push_back(&file);
        }
    }
    std::vector<RemoteProfile> remote;
    for (const auto& [user, files] : by_user) {
        const auto uuid = ParseUuid(user);
        if (!uuid) {
            continue;
        }
        const auto text = Drive::DownloadFile(CanonicalFile(files).id, stop);
        if (!text) {
            return std::nullopt;
        }
        const auto json = nlohmann::json::parse(text->begin(), text->end(), nullptr, false);
        remote.push_back({*uuid, json.is_object() ? StringField(json, "nickname") : ""});
    }
    return remote;
}

bool LinkProfile(const Service::Account::ProfileManager& profiles, const Common::UUID& local,
                 std::optional<Common::UUID> remote) {
    std::scoped_lock lock{SyncStateMutex()};
    auto state = LoadSyncState();
    if (!state) {
        return false;
    }
    const std::string key = local.FormattedString();
    const std::string previous = DriveUser(*state, local);
    const std::string target = remote ? remote->FormattedString() : key;
    for (const Common::UUID& user : profiles.GetAllUsers()) {
        if (user.IsValid() && user != local && DriveUser(*state, user) == target) {
            return false;
        }
    }
    if (target == previous) {
        return true;
    }
    if (target == key) {
        state->links.erase(key);
    } else {
        state->links[key] = target;
    }
    // The bases of both Drive identities were recorded with another pairing: the first sync of
    // the new one compares from scratch, so differing contents are a conflict.
    for (const std::string& drive_user : {previous, target}) {
        state->profiles.erase(drive_user);
        std::erase_if(state->saves,
                      [&](const auto& save) { return save.first.ends_with("/" + drive_user); });
    }
    return SaveSyncState(*state);
}

std::optional<Common::UUID> LinkedProfile(const Common::UUID& local) {
    std::scoped_lock lock{SyncStateMutex()};
    const auto state = LoadSyncState();
    if (!state) {
        return std::nullopt;
    }
    const auto link = state->links.find(local.FormattedString());
    return link != state->links.end() ? ParseUuid(link->second) : std::nullopt;
}

std::string ProfileNickname(const Service::Account::ProfileManager& profiles,
                            const Common::UUID& user) {
    Service::Account::ProfileBase base{};
    profiles.GetProfileBase(user, base);
    return Common::StringFromFixedZeroTerminatedBuffer(
        reinterpret_cast<const char*>(base.username.data()), base.username.size());
}

bool SyncProfiles(Service::Account::ProfileManager& profiles, const SyncCallbacks& callbacks,
                  SyncState& state, SyncResult& result, std::stop_token stop) {
    ProfilesSync sync{profiles, callbacks, state, result, stop};
    // The links of profiles removed here (by anything, not only this frontend), which would pair
    // a profile imported later with that uuid to the old Drive profile.
    const auto stale_links = std::erase_if(state.links, [&](const auto& link) {
        const auto uuid = ParseUuid(link.first);
        return !uuid || !profiles.UserExists(*uuid);
    });
    if (stale_links != 0 && !SaveSyncState(state)) {
        sync.Error("Could not save the cloud sync state");
    }
    std::map<std::string, Common::UUID> local;
    for (const Common::UUID& user : profiles.GetAllUsers()) {
        if (user.IsValid()) {
            local.emplace(DriveUser(state, user), user);
            // profiles.dat is shared by all profiles, so this time is only approximate.
            sync.local_times[user.FormattedString()] =
                std::max(LocalTime(ProfilesFilePath()), LocalTime(AvatarPath(user)));
        }
    }
    const auto jsons = Drive::FindFiles({{KindKey, "profileJson"}}, {}, stop);
    const auto avatars = jsons ? Drive::FindFiles({{KindKey, "avatar"}}, {}, stop) : std::nullopt;
    if (!jsons || !avatars) {
        sync.DriveError("Could not reach Google Drive for the profiles");
        return false;
    }
    std::map<std::string, std::vector<const Drive::File*>> remote_json;
    for (const Drive::File& file : *jsons) {
        if (const auto user = file.app_properties.find(UserKey);
            user != file.app_properties.end()) {
            remote_json[user->second].push_back(&file);
        }
    }
    std::map<std::string, std::vector<const Drive::File*>> remote_avatars;
    for (const Drive::File& file : *avatars) {
        if (const auto user = file.app_properties.find(UserKey);
            user != file.app_properties.end()) {
            remote_avatars[user->second].push_back(&file);
        }
    }

    std::set<std::string> drive_users;
    for (const auto& [drive_user, files] : remote_json) {
        drive_users.insert(drive_user);
    }
    for (const auto& [drive_user, user] : local) {
        drive_users.insert(drive_user);
    }
    for (const std::string& drive_user : drive_users) {
        if (stop.stop_requested()) {
            result.cancelled = true;
            return true;
        }
        const auto user = local.find(drive_user);
        // Drive's profile with the id of a local one that syncs as another: left as it is.
        if (const auto uuid = ParseUuid(drive_user);
            user == local.end() && uuid && profiles.UserExists(*uuid)) {
            continue;
        }
        const auto synced = state.profiles.find(drive_user);
        auto found = FindRemoteFile(remote_json[drive_user],
                                    synced != state.profiles.end() ? &synced->second : nullptr,
                                    {{KindKey, "profileJson"}, {UserKey, drive_user}}, stop);
        if (found.failed) {
            sync.DriveError(fmt::format("Could not look up the Drive profile {}", drive_user));
            continue;
        }
        if (!found.differing_id.empty()) {
            sync.Error(fmt::format("Google Drive has two different copies of the profile {} ({} "
                                   "and {}): the first is synced, the other kept as it is. "
                                   "Remove the one you don't want.",
                                   drive_user, found.file->id, found.differing_id));
        }
        sync.Sync(drive_user, user != local.end() ? std::optional{user->second} : std::nullopt,
                  std::move(found), remote_avatars[drive_user]);
    }
    return true;
}

} // namespace FrontendCommon::CloudSync
