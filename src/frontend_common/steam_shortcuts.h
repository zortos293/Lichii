// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

/// Adds games to Steam as non-Steam shortcuts, the way Steam's own "Add a Non-Steam Game" does:
/// an entry in userdata/<account>/config/shortcuts.vdf, a binary VDF file.
namespace Steam {

struct Shortcut {
    std::string app_name;
    std::string exe;       ///< The program to start, unquoted
    std::string start_dir; ///< Its working directory, unquoted
    std::string icon;      ///< An image file, or empty
    std::string launch_options;
};

/// The id Steam gives a non-Steam shortcut: CRC-32 of the quoted exe and the name, top bit set.
[[nodiscard]] u32 ShortcutAppId(std::string_view quoted_exe, std::string_view app_name);

/// shortcuts.vdf with the shortcut added. An entry with the same program and launch options is
/// replaced instead, and every other entry is kept as it was. Empty input starts a new file.
/// Returns nothing when the existing bytes are not a shortcuts file.
[[nodiscard]] std::optional<std::vector<u8>> AddToShortcutsFile(std::span<const u8> file,
                                                                const Shortcut& shortcut,
                                                                bool* replaced = nullptr);

/// The Steam install folder, or nothing when Steam isn't installed.
[[nodiscard]] std::optional<std::filesystem::path> FindSteamRoot();

/// The config folder of the Steam account that signed in last, or nothing when there is none.
[[nodiscard]] std::optional<std::filesystem::path> FindAccountConfigDir(
    const std::filesystem::path& steam_root);

/// Whether Steam is running. It rewrites shortcuts.vdf from memory, so a change made while it
/// runs would be lost.
[[nodiscard]] bool IsSteamRunning();

enum class AddResult {
    Added,
    Updated,
    SteamNotFound,
    SteamRunning,
    AccountNotFound,
    UnreadableFile,
    WriteFailed,
};

/// Adds the shortcut to the last signed-in account's shortcuts.vdf. The previous file is kept
/// next to it as shortcuts.vdf.bak.
[[nodiscard]] AddResult AddShortcut(const Shortcut& shortcut);

} // namespace Steam
