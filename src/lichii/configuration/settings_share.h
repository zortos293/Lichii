// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <QString>
#include "common/settings_common.h"

class QDialog;
class QDialogButtonBox;

namespace ConfigurationShared {
class Builder;
}

/// Settings share codes: one tab's (or every shareable tab's) settings as a short line of text
/// that can be pasted into the global configuration or a game's properties.
///
/// Format: `LX<version>[z].<section>.<payload>`. A code stands for every setting of the section;
/// to stay short, the payload lists only values that differ from the defaults, as `key=value`
/// pairs joined by `;` (values percent-encoded, using each setting's ToString/LoadString), and
/// pasting puts every unlisted setting back to its default. With `z`, the payload is the
/// base64url (no padding) of the qCompress'd pair list, used when that is shorter.
namespace SettingsShare {

constexpr int FormatVersion = 1;

struct Entry {
    std::string key;
    std::string value;
};

struct Decoded {
    QString section;
    std::vector<Entry> entries;
};

QString Encode(const QString& section, const std::vector<Entry>& entries);
std::optional<Decoded> Decode(const QString& code, QString& error);

struct DialogHooks {
    /// Section id of the visible tab, or empty when it holds no shareable settings.
    std::function<QString()> current_section;
    /// The dialog's widget builder: codes are read from and pasted into its fields.
    const ConfigurationShared::Builder* builder = nullptr;
};

/// Adds a "Share settings" button (Copy tab / Copy all / Paste code) to a dialog's button box.
/// Pasting sets the section's fields to match the code; OK or Apply keeps them, Cancel drops them.
void AddShareButton(QDialog* dialog, QDialogButtonBox* box, DialogHooks hooks);

} // namespace SettingsShare
