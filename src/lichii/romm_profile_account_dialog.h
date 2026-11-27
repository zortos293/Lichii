// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

#include "common/uuid.h"

class QLabel;
class QLineEdit;
class QPushButton;

// Opened from the profile editor's context menu ("RomM Account...") for one specific profile.
// Sets or clears that profile's own RomM account, used only for that profile's cloud save sync —
// independent of the master account configured via Tools > RomM..., and independent of any other
// profile's account. A profile with nothing saved here simply never gets a sync attempt.
class RommProfileAccountDialog : public QDialog {
    Q_OBJECT

public:
    RommProfileAccountDialog(const Common::UUID& profile_id, const QString& profile_display_name,
                             QWidget* parent = nullptr);

private:
    void LoadFromConfig();
    void OnTestConnection();
    void OnSave();
    void OnClear();

    Common::UUID profile_id;

    QLineEdit* instance_url_edit;
    QLineEdit* username_edit;
    QLineEdit* password_edit;
    QLabel* status_label;
    QPushButton* test_button;
    QPushButton* clear_button;
};
