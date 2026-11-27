// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;

// Tools > RomM > Master Login — configures the one "master" RomM account used for
// browsing/downloading the catalogue, which has its own entry (Tools > RomM > Catalogue). See
// romm_config.h for why this is separate from the per-profile accounts, which
// live inside each profile's own editor instead, mirroring how a real Switch links a Nintendo
// Account per-profile rather than system-wide).
class RommSettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit RommSettingsDialog(QWidget* parent = nullptr);

private:
    void LoadFromConfig();
    void OnTestConnection();
    void OnSave();
    void OnClear();


    QLineEdit* instance_url_edit;
    QLineEdit* username_edit;
    QLineEdit* password_edit;
    QLabel* status_label;
    QPushButton* test_button;
    QPushButton* clear_button;
};
