// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

#include <QString>

#include "lichii/game/styles/library_style_view.h"

/// Framework side of the style registry, used by GameList, MainWindow and Configure > UI. Style
/// files only need library_style_view.h.
namespace LibraryStyles {

/// The registered style with this id, or nullptr.
const StyleInfo* Find(const std::string& id);

/// The translated display name of a style.
QString DisplayName(const StyleInfo& info);

/// The style the setting asks for, or nullptr when the Modern grid (or Classic list) is in use.
const StyleInfo* Active();

} // namespace LibraryStyles
