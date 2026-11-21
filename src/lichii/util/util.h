// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2015 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QFont>
#include <QIcon>
#include <QPixmap>
#include <QSize>
#include <QString>
#include "common/uuid.h"

/// Returns a QFont object appropriate to use as a monospace font for debugging widgets, etc.
[[nodiscard]] QFont GetMonospaceFont();

/// A menu-action icon from the Lichii set. The tone follows the native menu on macOS (system
/// appearance) and the app theme elsewhere; the highlighted row uses a white icon. Other themes
/// keep menus without icons, so this returns a null icon unless Lichii Dark is active.
[[nodiscard]] QIcon MenuIcon(const char* name);

/// An icon for an app widget or button from the Lichii set. Its tone always follows the app theme,
/// unlike MenuIcon, which follows the native menu appearance on macOS.
[[nodiscard]] QIcon WidgetIcon(const char* name);

/// An icon from the Lichii set at a logical size, backed by its @2x bitmap. Returns a null
/// pixmap for other themes so callers can keep their theme icon.
[[nodiscard]] QPixmap ThemePixmap(const char* name, const QSize& logical_size, qreal dpr = 2.0);

/**
 * Prompt the user for a profile ID. If there is only one valid profile, returns that profile.
 * @return The selected profile, or an std::nullopt if none were selected
 */
const std::optional<Common::UUID> GetProfileID();

/**
 * Prompt the user for a profile ID. If there is only one valid profile, returns that profile.
 * @return A string representation of the selected profile, or an empty string if none were seleeced
 */
std::string GetProfileIDString();
