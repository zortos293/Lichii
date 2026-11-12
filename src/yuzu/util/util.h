// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2015 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <QColor>
#include <QFont>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include "common/uuid.h"

/// Returns a QFont object appropriate to use as a monospace font for debugging widgets, etc.
[[nodiscard]] QFont GetMonospaceFont();

/// A menu icon from the Lichii icon set, white on the highlighted row. Other themes keep menus
/// without icons, so this returns a null icon unless Lichii Dark is active.
[[nodiscard]] QIcon MenuIcon(const char* name);

/// The accent colour of the custom theme in use, nullopt when it sets none.
[[nodiscard]] std::optional<QColor> ThemeAccent();

/// An accent coloured image of the Lichii icon set, in the custom theme's accent when it sets one.
/// scale is the image's pixel ratio: 2 for an @2x file.
[[nodiscard]] QPixmap AccentPixmap(const QString& path, qreal scale = 1.0);

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
