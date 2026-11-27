// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdio>
#include <string>
#include <system_error>
#include <tuple>

#include "frontend_common/cloud_sync/sync_layout.h"

namespace FrontendCommon::CloudSync {

std::optional<std::string> FindOrCreateFolder(const Drive::AppProperties& properties,
                                              std::string_view name, std::string_view parent,
                                              std::stop_token stop) {
    const auto found = Drive::FindFiles(properties, {}, stop);
    if (!found) {
        return std::nullopt;
    }
    if (!found->empty()) {
        return found->front().id;
    }
    const auto created = Drive::CreateFolder(name, parent, properties, stop);
    return created ? std::optional{created->id} : std::nullopt;
}

std::optional<std::string> LayoutFolder(std::string_view kind, std::string_view name,
                                        std::stop_token stop) {
    const auto root = FindOrCreateFolder({{KindKey, "root"}}, "Lichii", {}, stop);
    if (!root) {
        return std::nullopt;
    }
    return FindOrCreateFolder({{KindKey, std::string{kind}}}, name, *root, stop);
}

const Drive::File& CanonicalFile(std::span<const Drive::File* const> files) {
    return **std::ranges::min_element(
        files, {}, [](const Drive::File* file) { return std::tie(file->created_time, file->id); });
}

std::string FileHash(const Drive::File& file) {
    const auto hash = file.app_properties.find(HashKey);
    return hash != file.app_properties.end() ? hash->second : std::string{};
}

RemoteFile FindRemoteFile(std::span<const Drive::File* const> found, const SyncedVersion* synced,
                          const Drive::AppProperties& identity, std::stop_token stop) {
    RemoteFile remote;
    if (!found.empty()) {
        const Drive::File& chosen = CanonicalFile(found);
        for (const Drive::File* file : found) {
            if (FileHash(*file) == FileHash(chosen)) {
                remote.same_contents.push_back(file->id);
            } else if (remote.differing_id.empty()) {
                remote.differing_id = file->id;
            }
        }
        remote.file = chosen;
        return remote;
    }
    if (!synced) {
        return remote;
    }
    auto file = Drive::GetFile(synced->file_id, stop);
    if (!file) {
        remote.failed = !Drive::LastError().starts_with("HTTP 404");
        return remote;
    }
    const bool same_identity = std::ranges::all_of(identity, [&file](const auto& property) {
        const auto it = file->app_properties.find(property.first);
        return it != file->app_properties.end() && it->second == property.second;
    });
    if (!file->trashed && same_identity) {
        remote.same_contents.push_back(file->id);
        remote.file = std::move(file);
    }
    return remote;
}

std::optional<std::string> ApplicableBase(const SyncedVersion* synced, const RemoteFile& remote) {
    if (!synced ||
        (remote.file && synced->file_id != remote.file->id &&
         std::ranges::find(remote.same_contents, synced->file_id) == remote.same_contents.end())) {
        return std::nullopt;
    }
    return synced->content_hash;
}

bool BaseOutdated(const SyncedVersion* synced, const Drive::File& remote, std::string_view hash) {
    return !synced || synced->file_id != remote.id || synced->content_hash != hash;
}

std::chrono::system_clock::time_point DriveTime(std::string_view rfc3339) {
    int year{}, month{}, day{}, hour{}, minute{}, second{};
    if (std::sscanf(std::string{rfc3339}.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour,
                    &minute, &second) != 6) {
        return {};
    }
    using namespace std::chrono;
    const year_month_day date{std::chrono::year{year}, std::chrono::month{unsigned(month)},
                              std::chrono::day{unsigned(day)}};
    if (!date.ok()) {
        return {};
    }
    return sys_days{date} + hours{hour} + minutes{minute} + seconds{second};
}

std::chrono::system_clock::time_point LocalTime(const std::filesystem::path& path) {
    std::error_code ec;
    auto newest = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return {};
    }
    if (std::filesystem::is_directory(path, ec)) {
        for (auto it = std::filesystem::recursive_directory_iterator{path, ec};
             !ec && it != std::filesystem::recursive_directory_iterator{}; it.increment(ec)) {
            newest = std::max(newest, it->last_write_time(ec));
        }
    }
    // file_clock has no portable conversion to system_clock (clock_cast is missing on some
    // standard libraries), so shift by the offset between the two clocks' current times.
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        newest - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
}

} // namespace FrontendCommon::CloudSync
