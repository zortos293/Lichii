// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <fstream>
#include <vector>

#include <QMessageBox>
#include <QObject>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include "common/fs/fs.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/romm_api.h"
#include "common/romm_config.h"
#include "core/file_sys/fs_filesystem.h"
#include "core/file_sys/savedata_factory.h"
#include "qt_common/qt_common.h"
#include "qt_common/util/compress.h"
#include "romm_sync.h"
#include "romm_save_picker_dialog.h"
#include "romm_title_match_dialog.h"

namespace RommSync {

namespace {

// The whole save directory travels as one zip, built and unpacked with the QuaZip helpers the
// frontend already ships (QtCommon::Compress), so RomM sync needs no zip library of its own.
std::string PathToUtf8(const std::filesystem::path& path) {
    return Common::FS::PathToUTF8String(path);
}

// Zips every file under `source_dir` into `dest_zip_path` (overwritten if present). An empty
// directory yields a valid empty archive: "no save exists yet" is a normal state, not an error.
bool ZipDirectory(const std::filesystem::path& source_dir,
                  const std::filesystem::path& dest_zip_path) {
    std::error_code ec;
    std::filesystem::remove(dest_zip_path, ec);
    return QtCommon::Compress::compressDir(
        QString::fromStdString(PathToUtf8(dest_zip_path)),
        QString::fromStdString(PathToUtf8(source_dir)), QtCommon::Compress::Options(),
        [](std::size_t, std::size_t) { return true; });
}

// Extracts `zip_path` over `dest_dir` (created if needed; existing files are overwritten and
// files not in the archive are left alone). False for a corrupt or empty archive.
bool ExtractZip(const std::filesystem::path& zip_path, const std::filesystem::path& dest_dir) {
    const QStringList extracted = QtCommon::Compress::extractDir(
        QString::fromStdString(PathToUtf8(zip_path)),
        QString::fromStdString(PathToUtf8(dest_dir)),
        [](std::size_t, std::size_t) { return true; });
    return !extracted.isEmpty();
}

std::string FormatRfc3339(std::filesystem::file_time_type ftime) {
    // file_time_type's clock isn't guaranteed to be system_clock pre-C++20-fully-conforming
    // stdlibs; clock_cast is the portable way to get a system_clock time_point out of it.
    const auto system_tp = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
    return fmt::format("{:%Y-%m-%dT%H:%M:%SZ}",
                       std::chrono::floor<std::chrono::seconds>(system_tp));
}

// One staging zip per (title, profile) under Lichii's cache dir — not the save dir itself, so a
// half-written zip can never be mistaken for part of the actual save data.
std::filesystem::path StagingZipPath(u64 program_id, const Common::UUID& profile_id) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "romm_sync_staging" /
           fmt::format("{:016X}_{}.zip", program_id, profile_id.RawString());
}

// Zips the whole save directory into one archive and describes *that* to the sync protocol,
// rather than walking it and sending one SaveFileMeta per real file. This is deliberate, not just
// simpler: some titles' saves are a folder with many small files — Super Smash Bros. Ultimate's
// Mii data, Tomodachi Life's custom food/clothing/Mii face paint — and negotiating file-by-file
// would turn one save into dozens of separate sync operations with no atomicity between them (a
// pull that downloads half of them before failing leaves a half-old, half-new save on disk). One
// zip is one hash, one operation, one all-or-nothing transfer either way.
//
// This always re-zips and re-hashes rather than caching: save data is KB-to-low-MB even for a
// folder-heavy title, so compressing it fresh costs a fraction of a second, and it's the only
// reliable way to notice local changes at all.
std::optional<Common::RommApi::SaveFileMeta> BuildSaveArchiveMeta(
    const std::filesystem::path& save_dir, const std::filesystem::path& staging_zip_path) {
    if (!ZipDirectory(save_dir, staging_zip_path)) {
        LOG_WARNING(Frontend, "RommSync: couldn't zip '{}'", save_dir.string());
        return std::nullopt;
    }

    const auto hash = Common::RommApi::Sha1HexOfFile(staging_zip_path.string());
    if (!hash.has_value()) {
        LOG_WARNING(Frontend, "RommSync: couldn't hash the zipped save archive");
        return std::nullopt;
    }

    std::error_code mtime_ec;
    const auto ftime = std::filesystem::last_write_time(staging_zip_path, mtime_ec);
    std::error_code size_ec;
    const auto size = std::filesystem::file_size(staging_zip_path, size_ec);

    Common::RommApi::SaveFileMeta meta;
    meta.filename = "save.zip";
    meta.sha1_hex = *hash;
    meta.mtime_rfc3339 = mtime_ec ? std::string{} : FormatRfc3339(ftime);
    meta.size_bytes = size_ec ? 0 : static_cast<u64>(size);
    return meta;
}

// A stable, fixed slot name for every sync of every title — RomM pairs a client save with its
// server counterpart on (rom_id, slot), and a save sent without one is treated as a one-off
// archival upload that can never be matched back for a Download (see NegotiateSync's own
// comment). One save per (rom, profile) is all Lichii ever has, so one constant slot is correct;
// separate profiles are already separated by separate RomM accounts/devices, not by slot.
constexpr const char* SaveSlot = "default";

// Shared setup for both downloads and Push(): resolves the rom_id, registers the device, and
// negotiates. What differs between the two directions is only which operation types get acted on
// afterwards, so that part stays in the callers.
struct SyncContext {
    Common::RommConfig::Credentials credentials;
    Common::RommApi::NegotiateResult negotiation;
    std::filesystem::path staging_zip_path; // Already contains the current local save, zipped.
    u64 rom_id = 0;
    std::string device_id;
};

// Matching is manual-only: the person's pick (see romm_config.h's TitleOverride) is the only
// source of a rom_id, asked once per title. rom_id 0 is the "never sync this title" sentinel.
// Prompting blocks on a dialog, so `allow_prompt` must only be true from the UI thread.
std::optional<Common::RommApi::Rom> ResolveRom(u64 program_id, const std::string& game_file_path,
                                               bool allow_prompt) {
    std::optional<Common::RommApi::Rom> rom;
    if (const auto override_id = Common::RommConfig::TitleOverride::GetRomId(program_id);
        override_id.has_value()) {
        if (*override_id == 0) {
            return std::nullopt; // Person said "never sync this title" — respected silently.
        }
        rom = Common::RommApi::GetRom(*override_id);
        if (!rom.has_value()) {
            LOG_WARNING(Frontend,
                        "RommSync: the manually-picked rom_id={} for title_id={:016X} no longer "
                        "exists on this instance — will ask again if a prompt is allowed here",
                        *override_id, program_id);
        }
    }

    if (!rom.has_value() && allow_prompt) {
        const auto pick = RommTitleMatchDialog::PickRomForTitle(
            std::filesystem::path(game_file_path).filename().string(), nullptr);
        switch (pick.action) {
        case RommTitleMatchDialog::Action::Selected:
            Common::RommConfig::TitleOverride::SetRomId(program_id, pick.rom_id);
            rom = Common::RommApi::GetRom(pick.rom_id);
            break;
        case RommTitleMatchDialog::Action::SkipPermanently:
            Common::RommConfig::TitleOverride::SetRomId(program_id, 0);
            return std::nullopt;
        case RommTitleMatchDialog::Action::SkipOnce:
            return std::nullopt;
        }
    }

    if (!rom.has_value()) {
        LOG_INFO(Frontend,
                 "RommSync: no title match for title_id={:016X} — skipping sync for this title",
                 program_id);
    }
    return rom;
}

std::optional<SyncContext> Negotiate(u64 program_id, const Common::UUID& profile_id,
                                     const std::filesystem::path& save_dir,
                                     const std::string& profile_display_name,
                                     const std::string& game_file_path, bool allow_prompt,
                                     bool pretend_local_empty = false) {
    const auto credentials = Common::RommConfig::Profile::GetCredentials(profile_id);
    if (!credentials.IsSet()) {
        return std::nullopt;
    }

    const auto rom = ResolveRom(program_id, game_file_path, allow_prompt);
    if (!rom.has_value()) {
        return std::nullopt;
    }

    const auto device_id = Common::RommApi::RegisterDeviceIfNeeded(
        credentials, fmt::format("Lichii - {}", profile_display_name), "");
    if (!device_id.has_value()) {
        LOG_WARNING(Frontend, "RommSync: device registration failed, skipping sync");
        return std::nullopt;
    }

    const auto staging_zip_path = StagingZipPath(program_id, profile_id);
    std::error_code mkdir_ec;
    std::filesystem::create_directories(staging_zip_path.parent_path(), mkdir_ec);

    // When the caller only wants to know what the server has (or to force-download it), the local
    // side is reported as an empty, ancient save so the server always sees its own copy as the
    // newer one instead of us zipping a directory that may not even exist yet.
    std::optional<Common::RommApi::SaveFileMeta> meta;
    if (pretend_local_empty) {
        Common::RommApi::SaveFileMeta empty;
        empty.filename = "save.zip";
        empty.sha1_hex = "da39a3ee5e6b4b0d3255bfef95601890afd80709"; // SHA-1 of zero bytes
        empty.mtime_rfc3339 = "1970-01-01T00:00:00Z";
        empty.size_bytes = 0;
        meta = empty;
    } else {
        meta = BuildSaveArchiveMeta(save_dir, staging_zip_path);
    }
    if (!meta.has_value()) {
        return std::nullopt;
    }

    auto negotiation =
        Common::RommApi::NegotiateSync(credentials, *device_id, rom->id, SaveSlot, *meta);
    if (!negotiation.ok) {
        LOG_WARNING(Frontend, "RommSync: negotiate failed: {}", negotiation.error);
        return std::nullopt;
    }
    return SyncContext{credentials,          std::move(negotiation), staging_zip_path,
                       rom->id,              *device_id};
}

} // Anonymous namespace

std::optional<std::filesystem::path> ResolveSaveDataPath(u64 program_id,
                                                          const Common::UUID& profile_id,
                                                          bool has_user_save) {
    const auto save_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::SaveDir);
    auto vfs_save_dir = QtCommon::vfs->OpenDirectory(Common::FS::PathToUTF8String(save_dir),
                                                     FileSys::OpenMode::Read);

    const std::string relative_path =
        has_user_save
            ? FileSys::SaveDataFactory::GetFullPath({}, vfs_save_dir,
                                                    FileSys::SaveDataSpaceId::User,
                                                    FileSys::SaveDataType::Account, program_id,
                                                    profile_id.AsU128(), 0)
            : FileSys::SaveDataFactory::GetFullPath({}, vfs_save_dir,
                                                    FileSys::SaveDataSpaceId::User,
                                                    FileSys::SaveDataType::Account, program_id, {},
                                                    0);

    const auto path = Common::FS::ConcatPathSafe(save_dir, relative_path);
    if (!Common::FS::CreateDirs(path)) {
        LOG_ERROR(Frontend, "RommSync: unable to create save data directory for program_id={:016X}",
                  program_id);
        return std::nullopt;
    }
    return path;
}

bool WarnIfLoginUnavailable(const Common::UUID& profile_id,
                            const std::string& profile_display_name) {
    const auto credentials = Common::RommConfig::Profile::GetCredentials(profile_id);
    if (!credentials.IsSet() ||
        Common::RommConfig::Secrets::GetPassword(credentials.secret_key).has_value()) {
        return false;
    }
    QMessageBox::warning(
        nullptr, QObject::tr("RomM cloud save"),
        QObject::tr("The RomM password for profile \"%1\" isn't in the system credential store "
                    "(it may have been removed, or the store isn't available), so its saves "
                    "won't sync. Log in again from the profile's RomM Account settings.")
            .arg(QString::fromStdString(profile_display_name)));
    return true;
}

bool LocalSaveExists(const std::filesystem::path& save_dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(save_dir, ec)) {
        return false;
    }
    for (std::filesystem::recursive_directory_iterator it(save_dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code file_ec;
        if (it->is_regular_file(file_ec)) {
            return true;
        }
    }
    return false;
}

namespace {

// Shared by PullIfNoLocalSave() and DownloadSave(): lists every save RomM holds for the title,
// lets the person pick one, and extracts it into `save_dir`. When `replace_existing` is set the
// local save is expected to exist: the person is asked to confirm the overwrite after picking and
// the current contents are copied to a backup folder first.
DownloadResult DownloadRemoteSave(u64 program_id, const Common::UUID& profile_id,
                                  const std::filesystem::path& save_dir,
                                  const std::string& profile_display_name,
                                  const std::string& game_file_path, const QString& header,
                                  bool replace_existing) {
    const auto credentials = Common::RommConfig::Profile::GetCredentials(profile_id);
    if (!credentials.IsSet()) {
        return DownloadResult::Unavailable;
    }
    const auto rom = ResolveRom(program_id, game_file_path, /*allow_prompt=*/true);
    if (!rom.has_value()) {
        return DownloadResult::Unavailable;
    }

    const auto saves = Common::RommApi::ListSaves(credentials, rom->id);
    if (!saves.has_value()) {
        return DownloadResult::Unavailable;
    }
    if (saves->empty()) {
        return DownloadResult::NoRemoteSave;
    }

    const auto chosen = RommSavePickerDialog::PickSave(*saves, header, nullptr);
    if (!chosen.has_value()) {
        return DownloadResult::Declined;
    }

    if (replace_existing && LocalSaveExists(save_dir)) {
        const auto answer = QMessageBox::warning(
            nullptr, QObject::tr("RomM cloud save"),
            QObject::tr("This will replace the local save of profile \"%1\" with the selected "
                        "one from RomM. The current local save is backed up first.\n\nContinue?")
                .arg(QString::fromStdString(profile_display_name)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return DownloadResult::Declined;
        }

        const auto backup_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) /
                                "romm_sync_backup" /
                                fmt::format("{:016X}_{}", program_id, profile_id.RawString());
        std::error_code ec;
        std::filesystem::remove_all(backup_dir, ec);
        std::filesystem::create_directories(backup_dir, ec);
        std::filesystem::copy(save_dir, backup_dir,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing,
                              ec);
        if (ec) {
            LOG_ERROR(Frontend, "RommSync: couldn't back up the local save ({}), not overwriting it",
                      ec.message());
            return DownloadResult::Unavailable;
        }
        LOG_INFO(Frontend, "RommSync: previous local save backed up to '{}'", backup_dir.string());
    }

    const auto bytes = Common::RommApi::DownloadSave(credentials, *chosen);
    if (!bytes.has_value()) {
        return DownloadResult::Unavailable;
    }
    // Saves uploaded by other tools aren't guaranteed to be zip archives (a bare .sav, for one).
    if (bytes->size() < 4 || (*bytes)[0] != 'P' || (*bytes)[1] != 'K') {
        LOG_WARNING(Frontend, "RommSync: save {} isn't a zip archive, can't restore it", *chosen);
        return DownloadResult::NotRestorable;
    }

    const auto staging_zip_path = StagingZipPath(program_id, profile_id);
    std::error_code mkdir_ec;
    std::filesystem::create_directories(staging_zip_path.parent_path(), mkdir_ec);
    {
        std::ofstream out(staging_zip_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return DownloadResult::Unavailable;
        }
        out.write(reinterpret_cast<const char*>(bytes->data()),
                  static_cast<std::streamsize>(bytes->size()));
    }

    // NOTE: this extracts on top of the existing save directory rather than clearing it first, so
    // a file the chosen save doesn't have stays behind locally.
    const bool extracted = ExtractZip(staging_zip_path, save_dir);
    std::error_code rm_ec;
    std::filesystem::remove(staging_zip_path, rm_ec);
    if (!extracted) {
        return DownloadResult::NotRestorable;
    }

    // Best effort: let the server's sync bookkeeping know this device now has the downloaded save
    // (as the old always-pull flow did), so the next upload doesn't treat it as an unrelated
    // local save. Failure here doesn't undo the download.
    if (const auto ctx = Negotiate(program_id, profile_id, save_dir, profile_display_name,
                                   game_file_path, /*allow_prompt=*/false,
                                   /*pretend_local_empty=*/true);
        ctx.has_value()) {
        u32 completed = 0;
        for (const auto& op : ctx->negotiation.operations) {
            if (op.save_id == *chosen) {
                completed = 1;
                break;
            }
        }
        Common::RommApi::CompleteSession(ctx->credentials, ctx->negotiation.session_id, completed,
                                         0, {});
    }
    return DownloadResult::Downloaded;
}

} // Anonymous namespace

void PullIfNoLocalSave(u64 program_id, const Common::UUID& profile_id, bool has_user_save,
                       const std::string& profile_display_name,
                       const std::string& game_file_path) {
    if (!Common::RommConfig::Profile::HasAccount(profile_id)) {
        return;
    }
    const auto save_dir = ResolveSaveDataPath(program_id, profile_id, has_user_save);
    if (!save_dir.has_value() || LocalSaveExists(*save_dir)) {
        return; // A local save already exists: never touch the network at boot for it.
    }
    const auto result = DownloadRemoteSave(
        program_id, profile_id, *save_dir, profile_display_name, game_file_path,
        QObject::tr("\"%1\" has no local save for profile \"%2\", but RomM has "
                                   "saves for it. Choose one to download before starting, or "
                                   "cancel to start without a save.")
                           .arg(QString::fromStdString(
                               std::filesystem::path(game_file_path).filename().string()))
                           .arg(QString::fromStdString(profile_display_name)),
        /*replace_existing=*/false);
    if (result == DownloadResult::NotRestorable) {
        QMessageBox::warning(nullptr, QObject::tr("RomM cloud save"),
                             QObject::tr("The selected save couldn't be restored, so the game "
                                         "will start without it."));
    }
}

DownloadResult DownloadSave(u64 program_id, const Common::UUID& profile_id, bool has_user_save,
                            const std::string& profile_display_name,
                            const std::string& game_file_path) {
    if (!Common::RommConfig::Profile::HasAccount(profile_id)) {
        return DownloadResult::Unavailable;
    }
    const auto save_dir = ResolveSaveDataPath(program_id, profile_id, has_user_save);
    if (!save_dir.has_value()) {
        return DownloadResult::Unavailable;
    }
    return DownloadRemoteSave(program_id, profile_id, *save_dir, profile_display_name,
                              game_file_path,
                              QObject::tr("Choose which save to download for profile \"%1\".")
                                  .arg(QString::fromStdString(profile_display_name)),
                              /*replace_existing=*/true);
}

void Push(u64 program_id, const Common::UUID& profile_id, bool has_user_save,
         const std::string& profile_display_name, const std::string& game_file_path) {
    LOG_INFO(Frontend, "RommSync: push started for title_id={:016X} (user save: {})", program_id,
             has_user_save);
    if (!Common::RommConfig::Profile::HasAccount(profile_id)) {
        LOG_INFO(Frontend, "RommSync: push skipped, the profile has no RomM account");
        return;
    }
    const auto save_dir = ResolveSaveDataPath(program_id, profile_id, has_user_save);
    if (!save_dir.has_value()) {
        LOG_WARNING(Frontend, "RommSync: push skipped, couldn't resolve the save directory");
        return;
    }
    if (!LocalSaveExists(*save_dir)) {
        // Nothing to upload: never push an empty archive over the remote save.
        LOG_INFO(Frontend, "RommSync: push skipped, no files in '{}'",
                 Common::FS::PathToUTF8String(*save_dir));
        return;
    }
    LOG_INFO(Frontend, "RommSync: pushing the save in '{}'",
             Common::FS::PathToUTF8String(*save_dir));
    const auto ctx = Negotiate(program_id, profile_id, *save_dir, profile_display_name,
                               game_file_path, /*allow_prompt=*/false);
    if (!ctx.has_value()) {
        LOG_WARNING(Frontend, "RommSync: push stopped before the upload (see the lines above)");
        return;
    }
    LOG_INFO(Frontend, "RommSync: negotiation done for rom_id={}: {} operation(s)", ctx->rom_id,
             ctx->negotiation.operations.size());

    if (ctx->negotiation.operations.empty()) {
        LOG_INFO(Frontend, "RommSync: the server returned no operations, nothing to upload");
    }
    u32 completed = 0;
    u32 failed = 0;
    for (const auto& op : ctx->negotiation.operations) {
        LOG_INFO(Frontend, "RommSync: server says '{}' for '{}' — {}",
                 static_cast<int>(op.type), op.file_name, op.reason);
        if (op.type != Common::RommApi::SyncOpType::Upload) {
            continue; // Conflicts are left alone: a pull is only ever explicit, see DownloadSave().
        }
        // Negotiate() already zipped the current save into staging_zip_path to compute its hash
        // for this same negotiation — reusing that file here instead of re-zipping means the
        // bytes uploaded are guaranteed to be exactly what was hashed, not a second, possibly
        // different, zip of a directory that could in principle change between the two steps.
        std::ifstream in(ctx->staging_zip_path, std::ios::binary);
        if (!in) {
            ++failed;
            continue;
        }
        const std::vector<u8> data((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
        if (!Common::RommApi::UploadSave(ctx->credentials, ctx->rom_id, SaveSlot, ctx->device_id,
                                         ctx->negotiation.session_id, op.file_name, data)) {
            LOG_WARNING(Frontend, "RommSync: the upload of '{}' ({} bytes) failed", op.file_name,
                        data.size());
            ++failed;
            continue;
        }
        LOG_INFO(Frontend, "RommSync: uploaded '{}' ({} bytes)", op.file_name, data.size());
        ++completed;
    }
    LOG_INFO(Frontend, "RommSync: push finished ({} uploaded, {} failed)", completed, failed);

    std::error_code ec;
    std::filesystem::remove(ctx->staging_zip_path, ec);
    Common::RommApi::CompleteSession(ctx->credentials, ctx->negotiation.session_id, completed,
                                     failed, {});
}

} // namespace RommSync
