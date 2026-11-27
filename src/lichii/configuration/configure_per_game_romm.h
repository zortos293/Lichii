// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

#include <QWidget>

#include "common/common_types.h"

class QLabel;
class QPushButton;

// The "RomM" tab in a game's Properties dialog — lets the person change or clear whatever
// TitleOverride decision applies to this title (see romm_config.h), for two cases the automatic
// flow can't handle on its own: they picked wrong and want to redo it, or they picked "never sync"
// before this title existed on their RomM instance and want to turn syncing back on for it now.
class ConfigurePerGameRomm : public QWidget {
    Q_OBJECT

public:
    ConfigurePerGameRomm(QWidget* parent, u64 title_id_, const std::string& file_name_);

private:
    void Refresh();
    void OnChooseTitle();
    void OnClearOverride();

    u64 title_id;
    std::string file_name;

    QLabel* status_label;
    QPushButton* choose_button;
    QPushButton* clear_button;
};
