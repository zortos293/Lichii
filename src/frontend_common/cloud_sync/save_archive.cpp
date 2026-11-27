// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <ranges>
#include <set>
#include <string_view>
#include <system_error>

#include <fmt/format.h>
#include <openssl/evp.h>

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/file_sys/savedata_factory.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "frontend_common/cloud_sync/save_archive.h"

namespace FrontendCommon::CloudSync {

namespace {

std::filesystem::path EntryPath(std::string_view name) {
    return std::u8string{name.begin(), name.end()};
}

// Relative, '/'-separated, without "." or ".." parts, a drive or a stream name, so it stays in the
// folder it is extracted to on every host. A NUL would cut the name short in the OS.
bool SafeEntryName(std::string_view name) {
    if (name.empty() || name.front() == '/' ||
        name.find_first_of(std::string_view{"\\:\0", 3}) != std::string_view::npos) {
        return false;
    }
    if (name.back() == '/') {
        name.remove_suffix(1);
    }
    return std::ranges::none_of(std::views::split(name, '/'), [](const auto& part) {
        const std::string_view text{part.begin(), part.end()};
        return text.empty() || text == "." || text == "..";
    });
}

// No two entries land on the same path, even on a case-insensitive filesystem (Windows, macOS),
// where one would overwrite the other. Only ASCII letters are folded.
bool DistinctDestinations(std::span<const ZipEntry> entries) {
    std::vector<std::string> paths;
    for (const ZipEntry& entry : entries) {
        std::string path = entry.name.ends_with('/') ? entry.name.substr(0, entry.name.size() - 1)
                                                     : entry.name;
        std::ranges::transform(path, path.begin(), [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        });
        paths.push_back(std::move(path));
    }
    std::ranges::sort(paths);
    return std::ranges::adjacent_find(paths) == paths.end();
}

// Creates the file, failing when anything is already at path. A file it can't fully write is
// removed.
bool WriteNewFile(const std::filesystem::path& path, std::span<const u8> data) {
#ifdef _WIN32
    std::FILE* const file = _wfopen(path.c_str(), L"wbx");
#else
    std::FILE* const file = std::fopen(path.c_str(), "wbx");
#endif
    if (file == nullptr) {
        return false;
    }
    const bool written = std::fwrite(data.data(), 1, data.size(), file) == data.size();
    if (std::fclose(file) != 0 || !written) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return false;
    }
    return true;
}

std::optional<std::vector<ZipEntry>> ReadFolder(const std::filesystem::path& folder) {
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) {
        return std::nullopt;
    }
    std::vector<ZipEntry> entries;
    for (auto it = std::filesystem::recursive_directory_iterator{folder, ec};
         !ec && it != std::filesystem::recursive_directory_iterator{}; it.increment(ec)) {
        const auto relative = it->path().lexically_relative(folder).generic_u8string();
        ZipEntry entry{.name{relative.begin(), relative.end()}};
        // The iterator doesn't enter a linked folder, which would be packed empty.
        const auto status = it->symlink_status(ec);
        if (std::filesystem::is_symlink(status)) {
            LOG_ERROR(Frontend, "Not packing the save with the symlink {}",
                      Common::FS::PathToUTF8String(it->path()));
            return std::nullopt;
        }
        if (std::filesystem::is_directory(status)) {
            entry.name += '/';
        } else if (std::filesystem::is_regular_file(status)) {
            std::ifstream file{it->path(), std::ios::binary};
            entry.data.assign(std::istreambuf_iterator<char>{file}, {});
            if (!file.is_open() || file.bad()) {
                return std::nullopt;
            }
        } else {
            continue;
        }
        entries.push_back(std::move(entry));
    }
    if (ec) {
        LOG_ERROR(Frontend, "Could not read the save folder: {}", ec.message());
        return std::nullopt;
    }
    std::ranges::sort(entries, {}, &ZipEntry::name);
    return entries;
}

// Into a folder that must not exist yet, so nothing but the entries ends up in it.
bool Extract(std::span<const ZipEntry> entries, const std::filesystem::path& folder) {
    std::error_code ec;
    if (!std::filesystem::create_directory(folder, ec)) {
        return false;
    }
    for (const ZipEntry& entry : entries) {
        const auto path = folder / EntryPath(entry.name);
        if (entry.name.ends_with('/')) {
            std::filesystem::create_directories(path, ec);
        } else {
            std::filesystem::create_directories(path.parent_path(), ec);
            if (!ec && !WriteNewFile(path, entry.data)) {
                return false;
            }
        }
        if (ec) {
            return false;
        }
    }
    return true;
}

// Names sort by time: UTC down to the clock's resolution.
std::string BackupName() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto midnight = floor<days>(now);
    const year_month_day date{midnight};
    const hh_mm_ss time{duration_cast<nanoseconds>(now - midnight)};
    return fmt::format("{:04}{:02}{:02}T{:02}{:02}{:02}.{:09}Z.zip", static_cast<int>(date.year()),
                       static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                       time.hours().count(), time.minutes().count(), time.seconds().count(),
                       time.subseconds().count());
}

bool BackUp(const std::filesystem::path& folder, const std::filesystem::path& backups) {
    const auto packed = PackSave(folder);
    std::error_code ec;
    std::filesystem::create_directories(backups, ec);
    if (!packed || ec) {
        return false;
    }
    const auto backup = backups / BackupName();
    if (!WriteNewFile(backup, packed->zip)) {
        return false;
    }

    // The new backup is always kept, even when the clock went back and older names sort after it.
    std::vector<std::filesystem::path> older;
    for (const auto& entry : std::filesystem::directory_iterator{backups, ec}) {
        if (entry.path().extension() == ".zip" && entry.path() != backup) {
            older.push_back(entry.path());
        }
    }
    std::ranges::sort(older);
    for (std::size_t i = 0; i + BackupsKept < older.size() + 1; ++i) {
        std::filesystem::remove(older[i], ec);
    }
    return true;
}

} // namespace

std::filesystem::path LocalSavePath(const SaveId& save) {
    const auto save_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::SaveDir);
    const auto type = save.user ? FileSys::SaveDataType::Account : FileSys::SaveDataType::Device;
    const u128 user_id = save.user ? save.user->AsU128() : u128{};
    // GetFullPath takes the future layout only when its folder is there. A restore into it cut
    // short mid-swap left the save at its .previous (as in RestoreSave): put it back first.
    const u64 future_title = save.title_id & ~0xFFULL;
    const auto future = Common::FS::ConcatPathSafe(
        save_dir,
        save.user ? fmt::format("{}/{:016X}/0",
                                FileSys::SaveDataFactory::GetUserGameSaveDataRoot(user_id, true),
                                future_title)
                  : fmt::format("/user/save/device/{:016X}/0", future_title));
    auto future_previous = future;
    future_previous += ".previous";
    std::error_code ec;
    if (!std::filesystem::exists(future, ec) && std::filesystem::exists(future_previous, ec)) {
        std::filesystem::rename(future_previous, future, ec);
        if (ec) {
            LOG_ERROR(Frontend,
                      "Could not move back the save an interrupted restore moved aside: {}",
                      ec.message());
        } else {
            LOG_WARNING(Frontend, "Moved back the save an interrupted restore had moved aside");
        }
    }
    FileSys::RealVfsFilesystem vfs;
    const auto root =
        vfs.OpenDirectory(Common::FS::PathToUTF8String(save_dir), FileSys::OpenMode::Read);
    const auto relative = FileSys::SaveDataFactory::GetFullPath(
        {}, root, FileSys::SaveDataSpaceId::User, type, save.title_id, user_id, 0);
    return Common::FS::ConcatPathSafe(save_dir, relative);
}

std::filesystem::path BackupFolder(const SaveId& save) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "cloud_sync" / "backups" /
           fmt::format("{:016X}", save.title_id) /
           (save.user ? save.user->FormattedString() : "device");
}

std::string ContentHash(std::span<const ZipEntry> entries) {
    std::vector<const ZipEntry*> sorted;
    for (const ZipEntry& entry : entries) {
        sorted.push_back(&entry);
    }
    std::ranges::sort(sorted, {}, &ZipEntry::name);

    EVP_MD_CTX* const context = EVP_MD_CTX_new();
    EVP_DigestInit_ex(context, EVP_sha256(), nullptr);
    for (const ZipEntry* entry : sorted) {
        // The name ends at its NUL, which no zip name has, and the size ends the data.
        std::array<u8, 8> size{};
        for (std::size_t i = 0; i < size.size(); ++i) {
            size[i] = static_cast<u8>(static_cast<u64>(entry->data.size()) >> (8 * i));
        }
        EVP_DigestUpdate(context, entry->name.c_str(), entry->name.size() + 1);
        EVP_DigestUpdate(context, size.data(), size.size());
        EVP_DigestUpdate(context, entry->data.data(), entry->data.size());
    }
    std::array<u8, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    EVP_DigestFinal_ex(context, digest.data(), &digest_size);
    EVP_MD_CTX_free(context);

    std::string hex;
    for (unsigned int i = 0; i < digest_size; ++i) {
        hex += fmt::format("{:02x}", digest[i]);
    }
    return hex;
}

std::optional<PackedSave> PackSave(const std::filesystem::path& folder) {
    const auto entries = ReadFolder(folder);
    if (!entries) {
        return std::nullopt;
    }
    // RestoreSave refuses such a zip, so it couldn't be restored.
    if (!DistinctDestinations(*entries)) {
        LOG_ERROR(Frontend, "Not packing the save with two entries for the same path");
        return std::nullopt;
    }
    auto zip = WriteZip(*entries);
    if (!zip) {
        return std::nullopt;
    }
    return PackedSave{.zip = std::move(*zip), .content_hash = ContentHash(*entries)};
}

bool FolderMatches(const std::filesystem::path& folder, std::span<const ZipEntry> entries) {
    std::vector<ZipEntry> expected;
    std::set<std::string> folders;
    for (const ZipEntry& entry : entries) {
        if (entry.name.ends_with('/')) {
            folders.insert(entry.name);
        } else {
            expected.push_back(entry);
        }
        // Parent folders a zip may leave out.
        for (auto end = entry.name.find('/'); end != std::string::npos;
             end = entry.name.find('/', end + 1)) {
            folders.insert(entry.name.substr(0, end + 1));
        }
    }
    for (const std::string& name : folders) {
        expected.push_back({.name = name});
    }
    std::ranges::sort(expected, {}, &ZipEntry::name);
    const auto actual = ReadFolder(folder);
    return actual && *actual == expected;
}

bool RestoreSave(const std::filesystem::path& folder, std::span<const u8> zip,
                 const std::filesystem::path& backups) {
    auto staging = folder;
    staging += ".restore";
    auto previous = folder;
    previous += ".previous";
    std::error_code ec;
    // A restore cut short mid-swap left the save at previous: put it back before anything else.
    if (!std::filesystem::exists(folder, ec) && std::filesystem::exists(previous, ec)) {
        std::filesystem::rename(previous, folder, ec);
        if (ec) {
            LOG_ERROR(Frontend, "Could not move back the save an interrupted restore moved aside: {}",
                      ec.message());
            return false;
        }
        LOG_WARNING(Frontend, "Moved back the save an interrupted restore had moved aside");
    }

    const auto entries = ReadZip(zip);
    if (!entries) {
        LOG_ERROR(Frontend, "Not restoring a save from an unreadable zip");
        return false;
    }
    if (!std::ranges::all_of(*entries, SafeEntryName, &ZipEntry::name)) {
        LOG_ERROR(Frontend, "Not restoring a save with an entry outside its folder");
        return false;
    }
    if (!DistinctDestinations(*entries)) {
        LOG_ERROR(Frontend, "Not restoring a save with two entries for the same path");
        return false;
    }

    // Leftovers of an earlier restore. The save is in place, so previous is an old copy that was
    // backed up before it was moved aside.
    for (const auto& leftover : {staging, previous}) {
        std::filesystem::remove_all(leftover, ec);
        if (ec || std::filesystem::exists(leftover, ec)) {
            LOG_ERROR(Frontend, "Could not remove {} left by an earlier restore",
                      Common::FS::PathToUTF8String(leftover));
            return false;
        }
    }
    const auto discard_staging = [&staging] {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
    };
    std::filesystem::create_directories(folder.parent_path(), ec);
    if (ec || !Extract(*entries, staging)) {
        LOG_ERROR(Frontend, "Could not extract the save beside {}",
                  Common::FS::PathToUTF8String(folder));
        discard_staging();
        return false;
    }
    if (!FolderMatches(staging, *entries)) {
        LOG_ERROR(Frontend, "Not restoring a save whose paths this filesystem merges");
        discard_staging();
        return false;
    }

    const bool had_save = std::filesystem::exists(folder, ec);
    if (had_save) {
        if (!BackUp(folder, backups)) {
            LOG_ERROR(Frontend, "Could not back up the save before replacing it");
            discard_staging();
            return false;
        }
        std::filesystem::rename(folder, previous, ec);
        if (ec) {
            LOG_ERROR(Frontend, "Could not move the current save aside: {}", ec.message());
            discard_staging();
            return false;
        }
    }
    std::filesystem::rename(staging, folder, ec);
    if (ec) {
        LOG_ERROR(Frontend, "Could not move the restored save in place: {}", ec.message());
        if (had_save) {
            std::filesystem::rename(previous, folder, ec);
            if (ec) {
                LOG_CRITICAL(Frontend, "The save stays at {} until the next restore: {}",
                             Common::FS::PathToUTF8String(previous), ec.message());
            }
        }
        discard_staging();
        return false;
    }
    std::filesystem::remove_all(previous, ec);
    if (ec) {
        LOG_WARNING(Frontend, "Could not remove the replaced save: {}", ec.message());
    }
    return true;
}

} // namespace FrontendCommon::CloudSync
