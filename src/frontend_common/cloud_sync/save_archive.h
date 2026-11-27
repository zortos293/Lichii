// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/uuid.h"
#include "frontend_common/cloud_sync/zip_archive.h"

namespace FrontendCommon::CloudSync {

// Backups RestoreSave keeps per save, the oldest removed first.
constexpr std::size_t BackupsKept = 5;

struct SaveId {
    u64 title_id{};
    // nullopt for the title's device save.
    std::optional<Common::UUID> user;
};

// The save's folder in SaveDir, as FileSys::SaveDataFactory::GetFullPath finds it: the future
// layout when that folder exists, else the old one.
std::filesystem::path LocalSavePath(const SaveId& save);

// ConfigDir/cloud_sync/backups/<title id>/<user uuid or "device">.
std::filesystem::path BackupFolder(const SaveId& save);

// SHA-256 over the entries sorted by name (each name, its size and its bytes), as 64 lowercase hex
// digits. Mtimes and the zip's order don't change it.
std::string ContentHash(std::span<const ZipEntry> entries);

struct PackedSave {
    std::vector<u8> zip;
    std::string content_hash;
};

// A deterministic zip of the folder's files and folders, sorted by name. nullopt when the folder
// doesn't exist or can't be read.
std::optional<PackedSave> PackSave(const std::filesystem::path& folder);

// Whether the folder holds exactly the entries (and their parent folders), with the same names and
// contents. A filesystem that takes two entry names for one path (case or Unicode folding on
// Windows and macOS) merges them, which this catches.
bool FolderMatches(const std::filesystem::path& folder, std::span<const ZipEntry> entries);

// Replaces the folder with the zip's contents. The zip is extracted beside the folder first, the
// current save zipped into backups (keeping the last BackupsKept), then the two swapped. A save an
// interrupted swap left beside the folder is moved back first. Returns false and leaves the save as
// it was when the zip is unreadable, an entry would land outside the folder or on another entry's
// path, the extracted folder doesn't match the zip (FolderMatches), or a step fails.
bool RestoreSave(const std::filesystem::path& folder, std::span<const u8> zip,
                 const std::filesystem::path& backups);

} // namespace FrontendCommon::CloudSync
