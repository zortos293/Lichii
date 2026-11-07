// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include "common/net/net.h"

class QRadioButton;
namespace Ui {
class UpdateDialog;
}

class UpdateDialog : public QDialog {
    Q_OBJECT

public:
    explicit UpdateDialog(const Common::Net::Release& release, QWidget* parent = nullptr);
    ~UpdateDialog();

private slots:
    void Download();

private:
#ifdef _WIN32
    /// Extracts the downloaded package and starts a helper that waits for this process to exit,
    /// copies the new files over the install and starts the emulator again.
    bool InstallAndRestart(const QString& package_path);
#endif

    Ui::UpdateDialog* ui;
    QList<QRadioButton*> m_buttons;
    Common::Net::Asset m_asset;
};
