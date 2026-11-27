// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <vector>

#include <QString>

#include "common/common_types.h"
#include "common/romm_api.h"

class QWidget;

// Lets the person choose which of the saves RomM holds for a title to download. By default only
// the saves Lichii itself uploaded are listed; a checkbox adds the ones uploaded some other way
// (another emulator, a tool, or by hand in RomM's web UI).
namespace RommSavePickerDialog {

// Blocks until the person picks a save (returns its save_id) or cancels (nullopt). `header` is
// shown above the list and should say why the person is being asked.
std::optional<u64> PickSave(const std::vector<Common::RommApi::RemoteSave>& saves,
                            const QString& header, QWidget* parent);

} // namespace RommSavePickerDialog
