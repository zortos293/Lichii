// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

#include <fmt/format.h>

#include "common/assert.h"
#include "core/hle/service/acc/profile_manager.h"
#include "frontend_common/cloud_sync/drive_client.h"
#include "frontend_common/cloud_sync/profile_sync.h"
#include "frontend_common/cloud_sync/save_archive.h"
#include "frontend_common/cloud_sync/sync_engine.h"
#include "frontend_common/cloud_sync/sync_layout.h"
#include "frontend_common/cloud_sync/sync_state.h"
#include "frontend_common/cloud_sync/web_service_sync.h"

namespace FrontendCommon::CloudSync {

namespace {

constexpr std::string_view ZipMimeType = "application/zip";

enum class Direction { Pull, Push, Both };

// One save of the title: a profile's or the device's.
struct SaveItem {
    SaveId id;
    std::string drive_user;
    // Empty for the device save.
    std::string nickname;
};

struct TitleSync {
    const Title& title;
    const SyncCallbacks& callbacks;
    Direction direction;
    SyncState& state;
    SyncResult& result;
    std::stop_token stop;
    // Looked up, or created, on the first upload.
    std::optional<std::string> game_folder{};

    std::string Describe(const SaveItem& item) const {
        return item.nickname.empty() ? fmt::format("the shared save of {}", title.name)
                                     : fmt::format("{}'s save of {}", item.nickname, title.name);
    }

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

    // Saves/<Game name>, with the title id appended when another title's folder has the name.
    std::optional<std::string> GameFolder() {
        if (game_folder) {
            return game_folder;
        }
        const std::string title_id = fmt::format("{:016X}", title.id);
        const auto found = Drive::FindFiles({{KindKey, "game"}, {TitleIdKey, title_id}}, {}, stop);
        if (!found) {
            return std::nullopt;
        }
        if (!found->empty()) {
            return game_folder = found->front().id;
        }
        const auto saves = LayoutFolder("saves", "Saves", stop);
        if (!saves) {
            return std::nullopt;
        }
        const auto games = Drive::FindFiles({{KindKey, "game"}}, *saves, stop);
        if (!games) {
            return std::nullopt;
        }
        std::string name = title.name.empty() ? title_id : title.name;
        if (std::ranges::any_of(*games,
                                [&](const Drive::File& game) { return game.name == name; })) {
            name += fmt::format(" [{}]", title_id);
        }
        const auto folder =
            Drive::CreateFolder(name, *saves, {{KindKey, "game"}, {TitleIdKey, title_id}}, stop);
        if (!folder) {
            return std::nullopt;
        }
        return game_folder = folder->id;
    }

    void Record(const std::string& key, SyncedVersion version) {
        state.saves[key] = std::move(version);
        if (!SaveSyncState(state)) {
            Error("Could not save the cloud sync state");
        }
    }

    // Uploads the local save, in place when Drive has it.
    void Push(const SaveItem& item, const PackedSave& local, const Drive::File* remote,
              const std::string& key) {
        const Drive::AppProperties properties{{KindKey, "save"},
                                              {TitleIdKey, fmt::format("{:016X}", title.id)},
                                              {UserKey, item.drive_user},
                                              {HashKey, local.content_hash}};
        std::optional<Drive::File> file;
        if (remote) {
            file = Drive::UpdateFile(remote->id, properties, ZipMimeType, local.zip, stop);
        } else if (const auto folder = GameFolder()) {
            const std::string name = item.nickname.empty() ? "Shared.zip" : item.nickname + ".zip";
            file = Drive::UploadFile(name, *folder, properties, ZipMimeType, local.zip, stop);
        }
        if (!file) {
            DriveError(fmt::format("Could not upload {}", Describe(item)));
            return;
        }
        Record(key, {file->id, local.content_hash, file->modified_time});
        ++result.pushed;
    }

    // Restores Drive's save. False when what was downloaded isn't the version the decision was
    // made on: Drive's save changed meanwhile.
    bool Pull(const SaveItem& item, const Drive::File& remote, const std::string& key) {
        const auto zip = Drive::DownloadFile(remote.id, stop);
        if (!zip) {
            DriveError(fmt::format("Could not download {}", Describe(item)));
            return true;
        }
        if (stop.stop_requested()) {
            result.cancelled = true;
            return true;
        }
        const auto entries = ReadZip(*zip);
        if (!entries || ContentHash(*entries) != FileHash(remote)) {
            return false;
        }
        if (!RestoreSave(LocalSavePath(item.id), *zip, BackupFolder(item.id))) {
            Error(fmt::format("Could not restore {}", Describe(item)));
            return true;
        }
        Record(key, {remote.id, FileHash(remote), remote.modified_time});
        ++result.pulled;
        return true;
    }

    // Decides and acts on the save once. False when Drive's save turned out to have changed since
    // it was read; found.file is then the current one (nullopt when it was trashed).
    bool SyncOnce(const SaveItem& item, RemoteFile& found) {
        const std::string key = SaveKey(title.id, item.drive_user);
        const auto folder = LocalSavePath(item.id);
        std::error_code ec;
        std::optional<PackedSave> local;
        if (std::filesystem::exists(folder, ec) || ec) {
            local = PackSave(folder);
            if (!local) {
                Error(fmt::format("Could not read {}", Describe(item)));
                return true;
            }
            // An empty folder is no save.
            if (local->content_hash == ContentHash({})) {
                local.reset();
            }
        }
        std::optional<Drive::File>& remote = found.file;
        const std::optional<std::string> remote_hash =
            remote ? std::optional{FileHash(*remote)} : std::nullopt;
        const auto synced_entry = state.saves.find(key);
        const SyncedVersion* synced =
            synced_entry != state.saves.end() ? &synced_entry->second : nullptr;
        const std::optional<std::string> base = ApplicableBase(synced, found);
        const std::optional<std::string> local_hash =
            local ? std::optional{local->content_hash} : std::nullopt;

        SyncAction action = DecideSync(local_hash, remote_hash, base);
        if (action == SyncAction::None) {
            // Both sides match: make that the base, so that the next change isn't a conflict.
            if (local && BaseOutdated(synced, *remote, *local_hash)) {
                Record(key, {remote->id, *local_hash, remote->modified_time});
            }
            return true;
        }
        if (action == SyncAction::Conflict) {
            const Conflict conflict{
                .title_id = title.id,
                .title_name = title.name,
                .nickname = item.nickname,
                .local_time = LocalTime(folder),
                .remote_time = DriveTime(remote->modified_time),
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
        } else if ((action == SyncAction::Push && direction == Direction::Pull) ||
                   (action == SyncAction::Pull && direction == Direction::Push)) {
            return true;
        }
        if (action == SyncAction::Push) {
            // Drive v3 has no conditional update (files.update takes no revision or ETag), so
            // Drive's save is read again just before it is replaced: a change since the decision
            // (another machine's upload) is decided again, which makes it a conflict. A change in
            // the moment between this read and the upload is still overwritten, and only kept as a
            // revision.
            if (remote) {
                const auto current = Drive::GetFile(remote->id, stop);
                if (!current) {
                    DriveError(fmt::format("Could not upload {}", Describe(item)));
                    return true;
                }
                if (current->trashed || FileHash(*current) != *remote_hash ||
                    current->head_revision_id != remote->head_revision_id) {
                    remote = current->trashed ? std::nullopt : current;
                    found.same_contents.clear();
                    return false;
                }
            }
            Push(item, *local, remote ? &*remote : nullptr, key);
            return true;
        }
        if (Pull(item, *remote, key)) {
            return true;
        }
        const auto current = Drive::GetFile(remote->id, stop);
        if (!current) {
            DriveError(fmt::format("Could not download {}", Describe(item)));
            return true;
        }
        remote = current->trashed ? std::nullopt : current;
        found.same_contents.clear();
        return false;
    }

    void Sync(const SaveItem& item, RemoteFile found) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (SyncOnce(item, found)) {
                return;
            }
        }
        Error(fmt::format("{} kept changing on Google Drive; try again later", Describe(item)));
    }

    // False when Drive couldn't be reached for the title's saves.
    bool Run(Service::Account::ProfileManager& profiles, bool save_steps) {
        const auto files = Drive::FindFiles(
            {{KindKey, "save"}, {TitleIdKey, fmt::format("{:016X}", title.id)}}, {}, stop);
        if (!files) {
            DriveError(fmt::format("Could not reach Google Drive for the saves of {}", title.name));
            return false;
        }
        std::map<std::string, std::vector<const Drive::File*>> remote;
        for (const Drive::File& file : *files) {
            if (const auto user = file.app_properties.find(UserKey);
                user != file.app_properties.end()) {
                remote[user->second].push_back(&file);
            }
        }

        std::vector<SaveItem> items;
        for (const Common::UUID& user : profiles.GetAllUsers()) {
            if (user.IsValid()) {
                items.push_back(
                    {{title.id, user}, DriveUser(state, user), ProfileNickname(profiles, user)});
            }
        }
        items.push_back({{title.id, std::nullopt}, DeviceUser, {}});

        for (std::size_t done = 0; done < items.size(); ++done) {
            if (stop.stop_requested()) {
                result.cancelled = true;
                break;
            }
            if (save_steps && callbacks.progress) {
                callbacks.progress(done, items.size());
            }
            const SaveItem& item = items[done];
            const auto synced = state.saves.find(SaveKey(title.id, item.drive_user));
            auto found = FindRemoteFile(remote[item.drive_user],
                                        synced != state.saves.end() ? &synced->second : nullptr,
                                        {{KindKey, "save"},
                                         {TitleIdKey, fmt::format("{:016X}", title.id)},
                                         {UserKey, item.drive_user}},
                                        stop);
            if (found.failed) {
                DriveError(fmt::format("Could not look up {}", Describe(item)));
                continue;
            }
            if (!found.differing_id.empty()) {
                Error(fmt::format("Google Drive has two different copies of {} ({} and {}): the "
                                  "first is synced, the other kept as it is. Remove the one you "
                                  "don't want.",
                                  Describe(item), found.file->id, found.differing_id));
            }
            Sync(item, std::move(found));
        }
        if (save_steps && callbacks.progress && !result.cancelled) {
            callbacks.progress(items.size(), items.size());
        }
        return true;
    }
};

// The syncs running, so that BeginGuest can stop those touching the guest's title.
struct Job {
    std::span<const Title> titles;
    // Syncs the profiles, which every guest uses.
    bool profiles{};
    std::stop_source stop;
    // Counts up from 1 in the order the jobs start.
    u64 id{};
};

std::mutex jobs_mutex;
std::condition_variable jobs_changed;
std::vector<Job*> jobs;
u64 last_job_id = 0;
bool guest_running = false;
u64 guest_title = 0;
// The last BeginGuest's, counting up from 1.
u64 guest_registration = 0;

bool Touches(const Job& job, u64 title_id) {
    return job.profiles || std::ranges::any_of(job.titles, [title_id](const Title& title) {
               return title.id == title_id;
           });
}

bool TitleInUse(u64 title_id) {
    std::scoped_lock lock{jobs_mutex};
    return guest_running && guest_title == title_id;
}

// Registers a sync for its run. Its stop token is stopped by the caller's or by BeginGuest.
class JobScope {
public:
    // With profiles only while no guest runs: one could be using them.
    JobScope(std::span<const Title> titles, bool profiles, std::stop_token caller)
        : job{titles, false, {}}, forward{caller, [this] { job.stop.request_stop(); }} {
        std::scoped_lock lock{jobs_mutex};
        job.profiles = profiles && !guest_running;
        job.id = ++last_job_id;
        jobs.push_back(&job);
    }
    ~JobScope() {
        std::scoped_lock lock{jobs_mutex};
        std::erase(jobs, &job);
        jobs_changed.notify_all();
    }
    JobScope(const JobScope&) = delete;
    JobScope& operator=(const JobScope&) = delete;

    std::stop_token Token() const {
        return job.stop.get_token();
    }
    bool Profiles() const {
        return job.profiles;
    }

private:
    Job job;
    std::stop_callback<std::function<void()>> forward;
};

SyncResult SyncTitles(Service::Account::ProfileManager& profiles, std::span<const Title> titles,
                      const SyncCallbacks& callbacks, Direction direction, bool with_profiles,
                      std::stop_token caller_stop,
                      const std::optional<std::optional<WebServiceLogin>>& web_service = {}) {
    SyncResult result;
    const JobScope job{titles, with_profiles, caller_stop};
    const std::stop_token stop = job.Token();
    const auto lock = LockSyncState(stop);
    if (!lock) {
        result.cancelled = true;
        return result;
    }
    auto loaded = LoadSyncState();
    if (!loaded) {
        result.errors.emplace_back("The cloud sync state (cloud_sync/state.json in the config "
                                   "folder) is unreadable; repair or remove it to sync again");
        return result;
    }
    SyncState& state = *loaded;
    // PullTitle and PushTitle report saves; SyncAll and SyncSaves the profiles (if synced), then
    // each title.
    const bool sync_all = direction == Direction::Both;
    const std::size_t steps = titles.size() + 1;
    if (sync_all && callbacks.progress) {
        callbacks.progress(0, steps);
    }
    // First, so that the saves of profiles new here are pulled too.
    if (job.Profiles() &&
        (!SyncProfiles(profiles, callbacks, state, result, stop) || result.cancelled)) {
        return result;
    }
    if (job.Profiles() && web_service &&
        (!SyncWebServiceLogin(*web_service, callbacks, state, result, stop) || result.cancelled)) {
        return result;
    }
    for (std::size_t done = 0; done < titles.size(); ++done) {
        if (stop.stop_requested()) {
            result.cancelled = true;
            return result;
        }
        if (sync_all && callbacks.progress) {
            callbacks.progress(done + 1, steps);
        }
        // PullTitle runs before its title boots, after BeginGuest.
        if (direction != Direction::Pull && TitleInUse(titles[done].id)) {
            result.errors.push_back(
                fmt::format("{} is running; its saves sync after it closes", titles[done].name));
            continue;
        }
        TitleSync sync{titles[done], callbacks, direction, state, result, stop};
        if (!sync.Run(profiles, !sync_all) || result.cancelled) {
            return result;
        }
    }
    if (!stop.stop_requested() && sync_all && callbacks.progress) {
        callbacks.progress(steps, steps);
    }
    if (stop.stop_requested()) {
        result.cancelled = true;
        return result;
    }
    if (result.errors.empty()) {
        state.last_sync = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
        if (!SaveSyncState(state)) {
            result.errors.emplace_back("Could not save the cloud sync state");
        }
    }
    return result;
}

} // Anonymous namespace

ConflictChoice WaitForAnswer(std::stop_token stop,
                             const std::function<void(std::function<void(ConflictChoice)>)>& post) {
    struct Answer {
        std::mutex mutex;
        std::condition_variable_any given;
        std::optional<ConflictChoice> choice;
    };
    if (stop.stop_requested()) {
        return ConflictChoice::Skip;
    }
    const auto answer = std::make_shared<Answer>();
    post([answer](ConflictChoice choice) {
        std::scoped_lock lock{answer->mutex};
        if (!answer->choice) {
            answer->choice = choice;
            answer->given.notify_all();
        }
    });
    std::unique_lock lock{answer->mutex};
    answer->given.wait(lock, stop, [&answer] { return answer->choice.has_value(); });
    if (stop.stop_requested()) {
        return ConflictChoice::Skip;
    }
    return answer->choice.value_or(ConflictChoice::Skip);
}

u64 BeginGuest(u64 title_id) {
    std::unique_lock lock{jobs_mutex};
    guest_running = true;
    guest_title = title_id;
    const u64 registration = ++guest_registration;
    // Only the jobs already running: those that start now skip the title (or, PullTitle, are
    // the guest's own).
    const u64 started = last_job_id;
    const auto touching = [title_id, started](const Job* job) {
        return job->id <= started && Touches(*job, title_id);
    };
    for (Job* job : jobs) {
        if (touching(job)) {
            job->stop.request_stop();
        }
    }
    jobs_changed.wait(lock, [&] { return std::ranges::none_of(jobs, touching); });
    return registration;
}

void EndGuest(u64 registration) {
    std::scoped_lock lock{jobs_mutex};
    if (registration != guest_registration) {
        return;
    }
    guest_running = false;
    guest_title = 0;
}

bool SyncRunning() {
    std::scoped_lock lock{jobs_mutex};
    return !jobs.empty();
}

std::optional<s64> LastSyncTime() {
    // The state is replaced in one rename, so reading it while a sync writes is safe.
    const auto state = LoadSyncState();
    return state && state->last_sync > 0 ? std::optional{state->last_sync} : std::nullopt;
}

SyncResult PullTitle(Service::Account::ProfileManager& profiles, const Title& title,
                     const SyncCallbacks& callbacks, std::stop_token stop) {
    return SyncTitles(profiles, {&title, 1}, callbacks, Direction::Pull, false, stop);
}

SyncResult PushTitle(Service::Account::ProfileManager& profiles, const Title& title,
                     const SyncCallbacks& callbacks, std::stop_token stop) {
    return SyncTitles(profiles, {&title, 1}, callbacks, Direction::Push, false, stop);
}

SyncResult SyncAll(Service::Account::ProfileManager& profiles, std::span<const Title> titles,
                   const SyncCallbacks& callbacks, std::stop_token stop,
                   std::optional<std::optional<WebServiceLogin>> web_service) {
    return SyncTitles(profiles, titles, callbacks, Direction::Both, true, stop, web_service);
}

SyncResult SyncSaves(Service::Account::ProfileManager& profiles, std::span<const Title> titles,
                     const SyncCallbacks& callbacks, std::stop_token stop) {
    return SyncTitles(profiles, titles, callbacks, Direction::Both, false, stop);
}

} // namespace FrontendCommon::CloudSync
