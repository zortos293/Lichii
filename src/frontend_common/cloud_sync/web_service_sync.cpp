// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <span>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/settings.h"
#include "frontend_common/cloud_sync/drive_client.h"
#include "frontend_common/cloud_sync/save_archive.h"
#include "frontend_common/cloud_sync/sync_layout.h"
#include "frontend_common/cloud_sync/web_service_sync.h"

namespace FrontendCommon::CloudSync {

namespace {

constexpr std::string_view FileName = "Web Service Login.json";
constexpr std::string_view JsonMimeType = "application/json";
constexpr const char* Kind = "webServiceLogin";

std::string LoginJson(const WebServiceLogin& login) {
    return nlohmann::json{{"username", login.username}, {"token", login.token}}.dump();
}

std::string JsonHash(std::string_view json) {
    const ZipEntry entry{std::string{FileName}, {json.begin(), json.end()}};
    return ContentHash({&entry, 1});
}

std::span<const u8> Bytes(std::string_view text) {
    return {reinterpret_cast<const u8*>(text.data()), text.size()};
}

struct LoginSync {
    const std::optional<WebServiceLogin>& local;
    const SyncCallbacks& callbacks;
    SyncState& state;
    SyncResult& result;
    std::stop_token stop;

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

    void Record(SyncedVersion version) {
        state.web_service_login = std::move(version);
        if (!SaveSyncState(state)) {
            Error("Could not save the cloud sync state");
        }
    }

    // Drive's login, checked against the hash the decision was made on. nullopt with changed set
    // when it changed meanwhile, else with an error.
    std::optional<WebServiceLogin> Download(const Drive::File& remote, bool& changed) {
        const auto data = Drive::DownloadFile(remote.id, stop);
        if (!data) {
            DriveError("Could not download the web service login");
            return std::nullopt;
        }
        const std::string_view text{reinterpret_cast<const char*>(data->data()), data->size()};
        if (JsonHash(text) != FileHash(remote)) {
            changed = true;
            return std::nullopt;
        }
        const auto json = nlohmann::json::parse(text, nullptr, false);
        const auto field = [&json](const char* key) {
            const auto it = json.find(key);
            return it != json.end() && it->is_string() ? it->get<std::string>() : std::string{};
        };
        WebServiceLogin login;
        if (json.is_object()) {
            login = {field("username"), field("token")};
        }
        if (login.username.empty() || login.token.empty()) {
            Error("The web service login on Google Drive is unreadable");
            return std::nullopt;
        }
        return login;
    }

    void Push(const std::string& json, const std::string& hash, const Drive::File* remote) {
        const Drive::AppProperties properties{{KindKey, Kind}, {HashKey, hash}};
        std::optional<Drive::File> file;
        if (remote) {
            file = Drive::UpdateFile(remote->id, properties, JsonMimeType, Bytes(json), stop);
        } else if (const auto root = FindOrCreateFolder({{KindKey, "root"}}, "Lichii", {}, stop)) {
            file = Drive::UploadFile(FileName, *root, properties, JsonMimeType, Bytes(json), stop);
        }
        if (!file) {
            DriveError("Could not upload the web service login");
            return;
        }
        Record({file->id, hash, file->modified_time});
        ++result.pushed;
    }

    // Decides and acts once. False when Drive's login turned out to have changed since it was
    // read; found.file is then the current one (nullopt when it was trashed).
    bool SyncOnce(RemoteFile& found) {
        const std::string local_json = local ? LoginJson(*local) : std::string{};
        const std::optional<std::string> local_hash =
            local ? std::optional{JsonHash(local_json)} : std::nullopt;
        std::optional<Drive::File>& remote = found.file;
        const std::optional<std::string> remote_hash =
            remote ? std::optional{FileHash(*remote)} : std::nullopt;
        const SyncedVersion* synced = state.web_service_login ? &*state.web_service_login : nullptr;

        SyncAction action = DecideSync(local_hash, remote_hash, ApplicableBase(synced, found));
        if (action == SyncAction::None) {
            if (local && BaseOutdated(synced, *remote, *local_hash)) {
                Record({remote->id, *local_hash, remote->modified_time});
            }
            return true;
        }
        // Drive's login is read before a conflict is asked, to show its username.
        std::optional<WebServiceLogin> remote_login;
        if (action != SyncAction::Push) {
            bool changed = false;
            remote_login = Download(*remote, changed);
            if (!remote_login) {
                return changed ? Refresh(found) : true;
            }
        }
        if (action == SyncAction::Conflict) {
            const Conflict conflict{
                .nickname = local->username,
                .remote_time = DriveTime(remote->modified_time),
                .web_service = true,
                .remote_nickname = remote_login->username,
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
            // As for saves (sync_engine.cpp): read again just before replacing, as Drive v3 has
            // no conditional update.
            if (remote) {
                const auto current = Drive::GetFile(remote->id, stop);
                if (!current) {
                    DriveError("Could not upload the web service login");
                    return true;
                }
                if (current->trashed || FileHash(*current) != *remote_hash ||
                    current->head_revision_id != remote->head_revision_id) {
                    remote = current->trashed ? std::nullopt : current;
                    found.same_contents.clear();
                    return false;
                }
            }
            Push(local_json, *local_hash, remote ? &*remote : nullptr);
            return true;
        }
        // Not recorded as the base until this device has it (SyncResult::web_service_login).
        result.web_service_login = std::move(remote_login);
        ++result.pulled;
        return true;
    }

    // Reads Drive's file again after it changed under a download. False, to decide again, unless
    // that fails.
    bool Refresh(RemoteFile& found) {
        const auto current = Drive::GetFile(found.file->id, stop);
        if (!current) {
            DriveError("Could not download the web service login");
            return true;
        }
        found.file = current->trashed ? std::nullopt : current;
        found.same_contents.clear();
        return false;
    }
};

} // Anonymous namespace

std::optional<WebServiceLogin> LocalWebServiceLogin() {
    const auto& username = Settings::values.eden_username;
    if (username.GetValue() == username.GetDefault() || username.GetValue().empty() ||
        Settings::values.eden_token.GetValue().empty()) {
        return std::nullopt;
    }
    return WebServiceLogin{username.GetValue(), Settings::values.eden_token.GetValue()};
}

bool SyncWebServiceLogin(const std::optional<WebServiceLogin>& local,
                         const SyncCallbacks& callbacks, SyncState& state, SyncResult& result,
                         std::stop_token stop) {
    LoginSync sync{local, callbacks, state, result, stop};
    const auto files = Drive::FindFiles({{KindKey, Kind}}, {}, stop);
    if (!files) {
        sync.DriveError("Could not reach Google Drive for the web service login");
        return false;
    }
    std::vector<const Drive::File*> found;
    for (const Drive::File& file : *files) {
        found.push_back(&file);
    }
    auto remote =
        FindRemoteFile(found, state.web_service_login ? &*state.web_service_login : nullptr,
                       {{KindKey, Kind}}, stop);
    if (remote.failed) {
        sync.DriveError("Could not look up the web service login");
        return true;
    }
    if (!remote.differing_id.empty()) {
        sync.Error(fmt::format("Google Drive has two different web service logins ({} and {}): the "
                               "first is synced, the other kept as it is. Remove the one you "
                               "don't want.",
                               remote.file->id, remote.differing_id));
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (sync.SyncOnce(remote)) {
            return true;
        }
    }
    sync.Error("The web service login kept changing on Google Drive; try again later");
    return true;
}

} // namespace FrontendCommon::CloudSync
