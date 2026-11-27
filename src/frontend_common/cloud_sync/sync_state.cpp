// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "frontend_common/cloud_sync/sync_state.h"

namespace FrontendCommon::CloudSync {

namespace {

std::filesystem::path StatePath() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "cloud_sync" / "state.json";
}

std::string StringField(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

SyncedVersion ReadVersion(const nlohmann::json& value) {
    return {StringField(value, "fileId"), StringField(value, "contentHash"),
            StringField(value, "modifiedTime")};
}

std::map<std::string, SyncedVersion> ReadVersions(const nlohmann::json& root, const char* key) {
    std::map<std::string, SyncedVersion> versions;
    const auto object = root.find(key);
    if (object == root.end() || !object->is_object()) {
        return versions;
    }
    for (const auto& [name, value] : object->items()) {
        if (value.is_object()) {
            versions.emplace(name, ReadVersion(value));
        }
    }
    return versions;
}

nlohmann::json VersionJson(const SyncedVersion& version) {
    return {{"fileId", version.file_id},
            {"contentHash", version.content_hash},
            {"modifiedTime", version.modified_time}};
}

nlohmann::json VersionsJson(const std::map<std::string, SyncedVersion>& versions) {
    nlohmann::json json = nlohmann::json::object();
    for (const auto& [name, version] : versions) {
        json[name] = VersionJson(version);
    }
    return json;
}

} // Anonymous namespace

std::string DriveUser(const SyncState& state, const Common::UUID& user) {
    const std::string local = user.FormattedString();
    const auto link = state.links.find(local);
    return link != state.links.end() ? link->second : local;
}

SyncAction DecideSync(const std::optional<std::string>& local,
                      const std::optional<std::string>& remote,
                      const std::optional<std::string>& base) {
    if (!local && !remote) {
        return SyncAction::None;
    }
    if (!remote) {
        return SyncAction::Push;
    }
    if (!local) {
        return SyncAction::Pull;
    }
    if (*local == *remote) {
        return SyncAction::None;
    }
    if (base == local) {
        return SyncAction::Pull;
    }
    if (base == remote) {
        return SyncAction::Push;
    }
    return SyncAction::Conflict;
}

std::string SaveKey(u64 title_id, const std::string& drive_user) {
    return fmt::format("{:016X}/{}", title_id, drive_user);
}

std::timed_mutex& SyncStateMutex() {
    static std::timed_mutex mutex;
    return mutex;
}

std::unique_lock<std::timed_mutex> LockSyncState(std::stop_token stop) {
    std::unique_lock lock{SyncStateMutex(), std::defer_lock};
    while (!stop.stop_requested() && !lock.try_lock_for(std::chrono::milliseconds{50})) {
    }
    return lock;
}

std::optional<SyncState> LoadSyncState() {
    const auto path = StatePath();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) && !ec) {
        return SyncState{};
    }
    std::ifstream file{path, std::ios::binary};
    const auto root = file.is_open() ? nlohmann::json::parse(std::istreambuf_iterator<char>{file},
                                                             {}, nullptr, false)
                                     : nlohmann::json{};
    if (!root.is_object()) {
        LOG_ERROR(Frontend, "The cloud sync state {} is unreadable",
                  Common::FS::PathToUTF8String(path));
        return std::nullopt;
    }
    SyncState state{.saves = ReadVersions(root, "saves"),
                    .profiles = ReadVersions(root, "profiles")};
    if (const auto last_sync = root.find("last_sync");
        last_sync != root.end() && last_sync->is_number_integer()) {
        state.last_sync = last_sync->get<s64>();
    }
    if (const auto login = root.find("webServiceLogin");
        login != root.end() && login->is_object()) {
        state.web_service_login = ReadVersion(*login);
    }
    if (const auto links = root.find("links"); links != root.end() && links->is_object()) {
        for (const auto& [local, remote] : links->items()) {
            if (remote.is_string()) {
                state.links.emplace(local, remote.get<std::string>());
            }
        }
    }
    return state;
}

bool SaveSyncState(const SyncState& state) {
    const auto path = StatePath();
    auto temporary = path;
    temporary += ".tmp";
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    {
        std::ofstream file{temporary, std::ios::binary | std::ios::trunc};
        nlohmann::json root{{"saves", VersionsJson(state.saves)},
                            {"profiles", VersionsJson(state.profiles)},
                            {"links", state.links},
                            {"last_sync", state.last_sync}};
        if (state.web_service_login) {
            root["webServiceLogin"] = VersionJson(*state.web_service_login);
        }
        file << root.dump(1, '\t');
        // Closed here, as a failed flush shows only then.
        file.close();
        if (file.fail()) {
            LOG_ERROR(Frontend, "Could not write the cloud sync state");
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        LOG_ERROR(Frontend, "Could not replace the cloud sync state: {}", ec.message());
        return false;
    }
    return true;
}

} // namespace FrontendCommon::CloudSync
