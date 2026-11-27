// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stop_token>
#include <vector>

#include <QWidget>

#include "common/uuid.h"
#include "frontend_common/cloud_sync/profile_sync.h"

class QCheckBox;
class QComboBox;
class QFormLayout;
class QGroupBox;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QToolButton;

namespace Core {
class System;
}

// Google Drive sync settings: on or off, whether the web service login syncs, the Google account,
// the OAuth client, which Drive profile each local profile syncs as, and "Sync now".
class ConfigureCloudSync : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureCloudSync(Core::System& system, QWidget* parent = nullptr);
    ~ConfigureCloudSync() override;

    void ApplyConfiguration();

private:
    void changeEvent(QEvent* event) override;
    // Profiles may have been added, renamed or deleted since the picker was filled.
    void showEvent(QShowEvent* event) override;
    void RetranslateUI();
    // Fills the local picker with the profiles there are now, keeping the selected one. Not while
    // a sync runs, which may be changing them.
    void ShowLocalProfiles();
    void SignIn();
    void SignOut();
    void ImportClientSecret();
    void LoadRemoteProfiles();
    void ShowRemoteProfiles();
    void Link();
    // Shows the sign-in state and the selected profile's link, and enables what can be used now.
    void UpdateState();

    Core::System& system;
    std::stop_source sign_in_stop;
    bool signing_in{};
    // The code to enter at Google while signing in.
    QString user_code;
    bool busy{};

    QCheckBox* enabled;
    QLabel* description;
    QCheckBox* web_service;
    QGroupBox* account_group;
    QGroupBox* link_group;
    QFormLayout* link_layout;
    QHBoxLayout* remote_row;
    QLabel* link_description;
    QLabel* account_status;
    QLabel* client_status;
    QPushButton* sign_in;
    QPushButton* sign_out;
    QPushButton* copy_code;
    QToolButton* advanced_toggle;
    QWidget* advanced;
    QLabel* advanced_description;
    QPushButton* import_client;
    QComboBox* local_profile;
    QComboBox* remote_profile;
    QPushButton* load_remote;
    QPushButton* link;
    QLabel* link_status;
    QPushButton* sync_now;

    std::vector<Common::UUID> local_uuids;
    // The Drive profiles last loaded.
    std::vector<FrontendCommon::CloudSync::RemoteProfile> remote_profiles;
    // The remote picker's profiles after its first entry, which links to none: the Drive profiles
    // except the selected local profile's own, which that entry already is.
    std::vector<Common::UUID> remote_uuids;
};
