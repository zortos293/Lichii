// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

// The Drive v3 calls sync needs, made with the signed-in account's access token (google_auth.h).
// Each one retries 429 and 5xx replies with backoff and refreshes the token once on a 401. They
// return nullopt when Drive can't be reached or refuses the call, or stop is requested: that
// aborts the request in flight (but not a connection still being made) and the wait for a retry.
namespace FrontendCommon::CloudSync::Drive {

constexpr std::string_view DriveBase = "https://www.googleapis.com";
constexpr std::string_view FolderMimeType = "application/vnd.google-apps.folder";
// Attempts after the first for a 429 or 5xx reply; the first waits a second, each next twice
// as long.
constexpr int Retries = 4;

// Sends the Drive requests to base instead of www.googleapis.com. For tests.
void SetDriveBaseForTests(std::string base);

// Why this thread's last call failed, for the user: Drive's message, or the connection error.
std::string LastError();

using AppProperties = std::map<std::string, std::string>;

struct File {
    std::string id;
    std::string name;
    std::vector<std::string> parents;
    AppProperties app_properties;
    // RFC 3339, UTC.
    std::string created_time;
    std::string modified_time;
    // Empty for folders.
    std::string md5_checksum;
    // Changes with every new version of the contents. Empty for folders.
    std::string head_revision_id;
    // Only GetFile can return a file in the trash.
    bool trashed{};
};

// The files and folders not in the trash whose appProperties hold all of properties, and that
// are in parent when it isn't empty.
std::optional<std::vector<File>> FindFiles(const AppProperties& properties,
                                           std::string_view parent = {}, std::stop_token stop = {});

// In the root of My Drive when parent is empty.
std::optional<File> CreateFolder(std::string_view name, std::string_view parent,
                                 const AppProperties& properties, std::stop_token stop = {});

std::optional<File> UploadFile(std::string_view name, std::string_view parent,
                               const AppProperties& properties, std::string_view mime_type,
                               std::span<const u8> data, std::stop_token stop = {});

// Replaces the file's contents, keeping the old ones as a revision, and sets properties.
std::optional<File> UpdateFile(std::string_view id, const AppProperties& properties,
                               std::string_view mime_type, std::span<const u8> data,
                               std::stop_token stop = {});

// The file's metadata, in the trash or not.
std::optional<File> GetFile(std::string_view id, std::stop_token stop = {});

std::optional<std::vector<u8>> DownloadFile(std::string_view id, std::stop_token stop = {});

std::optional<File> RenameFile(std::string_view id, std::string_view name,
                               std::stop_token stop = {});

} // namespace FrontendCommon::CloudSync::Drive
