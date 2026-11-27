// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "common/common_types.h"
#include "common/uuid.h"

// Frontend-side orchestration for RomM cloud save sync: resolving the on-disk save path, zipping
// and hashing the save, and driving Common::RommApi's Device Sync Protocol calls. Lives in
// src/yuzu (not src/common) because it needs QtCommon::vfs and FileSys::SaveDataFactory — the
// network/protocol layer in src/common stays frontend-agnostic, this doesn't.
namespace RommSync {

// Mirrors the exact path resolution MainWindow::OnGameListOpenFolder uses for
// GameListOpenTarget::SaveData, creating the directory if it doesn't exist yet (a fresh profile
// with no prior save for this title won't have one). Returns nullopt if resolution or directory
// creation fails. `has_user_save` must be worked out by the caller the same way
// OnGameListOpenFolder does (from the title's control metadata) — duplicating that lookup here
// would mean loading the NACP a second time for no reason.
std::optional<std::filesystem::path> ResolveSaveDataPath(u64 program_id,
                                                          const Common::UUID& profile_id,
                                                          bool has_user_save);

// Shows a warning and returns true if this profile has a RomM account but its password can't be
// found in the system credential store (it was removed, or the store is unavailable), in which
// case nothing can sync until the person logs in again. No network involved. Returns false
// without a dialog for profiles with no RomM account or a password that is there. Call from the
// UI thread; the first call may make the credential store prompt to unlock.
bool WarnIfLoginUnavailable(const Common::UUID& profile_id,
                            const std::string& profile_display_name);

// True if the save directory holds at least one regular file, i.e. the profile has played this
// title (or restored a save) before. Pure filesystem check, no network.
bool LocalSaveExists(const std::filesystem::path& save_dir);

enum class DownloadResult {
    Downloaded,   // The remote save was extracted into the local save directory.
    NoRemoteSave, // RomM has no save at all for this title.
    Declined,     // The person cancelled the picker or the overwrite confirmation.
    NotRestorable, // The chosen save isn't a zip archive Lichii can restore (e.g. a bare .sav).
    Unavailable,  // Not configured, title skipped/unmatched, or the instance couldn't be reached.
};

// Called from BootGame() before LoadROM(). Never downloads on its own while a local save exists
// — in that case it returns immediately without touching the network. Only when this profile has
// no local save for the title does it look up the saves RomM holds for it and, if there are any,
// shows a picker (cancel = start without a save). Synchronous by design: a downloaded save must be
// on disk before the game reads it. Bounded by RommApi's per-request timeout. No-ops silently if
// the profile has no RomM account. If the title has no manually-picked rom_id yet (see
// romm_config.h's TitleOverride) this may show the blocking match-picker dialog, which is safe for
// the same reason (already on the UI thread, before boot). `game_file_path` is only used to show
// the file name in dialogs.
void PullIfNoLocalSave(u64 program_id, const Common::UUID& profile_id, bool has_user_save,
                       const std::string& profile_display_name, const std::string& game_file_path);

// Explicit, person-triggered download (game list context menu): lists the saves RomM holds for
// the title, lets the person pick one, and extracts it over the local save. If a local save
// exists the person is asked to confirm after picking, and its current contents are first copied
// to a backup folder under Lichii's cache directory (one backup per title+profile, replaced each
// time). Same blocking/UI-thread characteristics as PullIfNoLocalSave.
DownloadResult DownloadSave(u64 program_id, const Common::UUID& profile_id, bool has_user_save,
                            const std::string& profile_display_name,
                            const std::string& game_file_path);

// Safe to run on a background thread (call via QtConcurrent::run from OnEmulationStopped) since
// nothing reads or writes these save files again until the next boot of this title — by which
// point this will long since have finished for any realistic instance. Same no-op-if-unconfigured
// behavior as PullIfNoLocalSave(), and it skips titles whose save directory is empty so a game
// that never wrote a save can't overwrite the remote one with an empty archive.
void Push(u64 program_id, const Common::UUID& profile_id, bool has_user_save,
         const std::string& profile_display_name, const std::string& game_file_path);

} // namespace RommSync
