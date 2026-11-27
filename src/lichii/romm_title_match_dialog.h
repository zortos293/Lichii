// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

#include "common/common_types.h"

class QWidget;

// Shown from RommSync when no RomM entry has been picked for this title yet — the title's own name
// is unknown to RommSync at that point (it works from a save-data path and a rom file, not a
// display name), so this shows the title's file name as a hint and lets the person search the
// catalogue by name instead.
namespace RommTitleMatchDialog {

enum class Action {
    Selected,        // rom_id is the person's pick; record it and use it now.
    SkipPermanently, // Record "never ask about this title again"; don't sync this attempt either.
    SkipOnce,        // Closed/cancelled without a choice; don't sync this attempt, but ask again
                     // next time in case it was just bad timing (offline, wrong tab, etc.).
};

struct PickResult {
    Action action = Action::SkipOnce;
    u64 rom_id = 0; // Only meaningful when action == Selected.
};

// Blocks until the person picks one of the three outcomes above. Fetches the catalogue fresh each
// call: this only runs on the rare path where matching already failed once, so it isn't worth
// caching.
PickResult PickRomForTitle(const std::string& file_name_hint, QWidget* parent);

} // namespace RommTitleMatchDialog
