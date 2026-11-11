// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>

#include <QUrl>

namespace QtCommon {

/// Open a URL in the host's default handler (browser or file manager).
/// On Linux the handler is spawned via xdg-open without an overridden
/// XDG_CONFIG_HOME, so external apps use the session default config
/// instead of the emulator's (which lacks e.g. kdeglobals and breaks
/// theming in file managers like Dolphin).
bool OpenUrl(const QUrl& url);

/// Convenience wrappers for opening a local folder in the file manager.
bool OpenLocalFolder(const QString& path);
bool OpenLocalFolder(const std::filesystem::path& path);

} // namespace QtCommon
