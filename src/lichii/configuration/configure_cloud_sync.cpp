// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include "common/settings.h"
#include "core/core.h"
#include "core/hle/service/acc/profile_manager.h"
#include "frontend_common/cloud_sync/google_auth.h"
#include "frontend_common/cloud_sync/profile_sync.h"
#include "frontend_common/cloud_sync/sync_engine.h"
#include "lichii/cloud_sync_controller.h"
#include "lichii/configuration/configure_cloud_sync.h"

namespace CloudSync = FrontendCommon::CloudSync;

ConfigureCloudSync::ConfigureCloudSync(Core::System& system_, QWidget* parent)
    : QWidget(parent), system{system_} {
    enabled = new QCheckBox;
    description = new QLabel;
    description->setWordWrap(true);
    web_service = new QCheckBox;

    account_status = new QLabel;
    account_status->setWordWrap(true);
    account_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    client_status = new QLabel;
    client_status->setWordWrap(true);
    client_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sign_in = new QPushButton;
    sign_out = new QPushButton;
    copy_code = new QPushButton;
    auto* const account_buttons = new QHBoxLayout;
    account_buttons->addWidget(sign_in);
    account_buttons->addWidget(copy_code);
    account_buttons->addWidget(sign_out);
    account_buttons->addStretch();

    // The OAuth client: collapsed unless an imported one is in use.
    advanced_toggle = new QToolButton;
    advanced_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advanced_toggle->setAutoRaise(true);
    advanced_toggle->setCheckable(true);
    advanced_description = new QLabel;
    advanced_description->setWordWrap(true);
    import_client = new QPushButton;
    auto* const import_row = new QHBoxLayout;
    import_row->addWidget(import_client);
    import_row->addStretch();
    advanced = new QWidget;
    auto* const advanced_layout = new QVBoxLayout(advanced);
    advanced_layout->setContentsMargins(0, 0, 0, 0);
    advanced_layout->addWidget(advanced_description);
    advanced_layout->addWidget(client_status);
    advanced_layout->addLayout(import_row);

    account_group = new QGroupBox;
    auto* const account_layout = new QVBoxLayout(account_group);
    account_layout->addWidget(account_status);
    account_layout->addLayout(account_buttons);
    account_layout->addWidget(advanced_toggle);
    account_layout->addWidget(advanced);

    local_profile = new QComboBox;
    remote_profile = new QComboBox;
    remote_profile->addItem(QString{});
    load_remote = new QPushButton;
    link = new QPushButton;
    link_status = new QLabel;
    link_status->setWordWrap(true);
    remote_row = new QHBoxLayout;
    remote_row->addWidget(remote_profile, 1);
    remote_row->addWidget(load_remote);
    remote_row->addWidget(link);
    link_group = new QGroupBox;
    link_layout = new QFormLayout(link_group);
    link_description = new QLabel;
    link_description->setWordWrap(true);
    link_layout->addRow(link_description);
    // Labels of their own: an empty label text makes none, for RetranslateUI to name.
    link_layout->addRow(new QLabel, local_profile);
    link_layout->addRow(new QLabel, remote_row);
    link_layout->addRow(link_status);

    sync_now = new QPushButton;
    auto* const sync_row = new QHBoxLayout;
    sync_row->addWidget(sync_now);
    sync_row->addStretch();

    auto* const layout = new QVBoxLayout(this);
    layout->addWidget(enabled);
    layout->addWidget(description);
    layout->addWidget(web_service);
    layout->addWidget(account_group);
    layout->addWidget(link_group);
    layout->addLayout(sync_row);
    layout->addStretch();

    enabled->setChecked(Settings::values.cloud_sync_enabled.GetValue());
    web_service->setChecked(Settings::values.cloud_sync_web_service.GetValue());
    if (auto* const controller = CloudSyncController::Instance()) {
        busy = controller->Busy();
        connect(controller, &CloudSyncController::BusyChanged, this, [this](bool busy_) {
            busy = busy_;
            ShowLocalProfiles();
            UpdateState();
        });
    }

    connect(sign_in, &QPushButton::clicked, this, &ConfigureCloudSync::SignIn);
    connect(sign_out, &QPushButton::clicked, this, &ConfigureCloudSync::SignOut);
    connect(import_client, &QPushButton::clicked, this, &ConfigureCloudSync::ImportClientSecret);
    connect(copy_code, &QPushButton::clicked, this,
            [this] { QGuiApplication::clipboard()->setText(user_code); });
    connect(advanced_toggle, &QToolButton::toggled, this, [this](bool open) {
        advanced->setVisible(open);
        advanced_toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    });
    connect(load_remote, &QPushButton::clicked, this, &ConfigureCloudSync::LoadRemoteProfiles);
    connect(link, &QPushButton::clicked, this, &ConfigureCloudSync::Link);
    connect(local_profile, &QComboBox::currentIndexChanged, this, [this] {
        ShowRemoteProfiles();
        UpdateState();
    });
    connect(enabled, &QCheckBox::toggled, this, &ConfigureCloudSync::UpdateState);
    connect(sync_now, &QPushButton::clicked, this, [this] {
        // Sync now uses the setting as it is shown.
        ApplyConfiguration();
        if (auto* const controller = CloudSyncController::Instance()) {
            controller->SyncAll();
        }
    });
    const bool imported = CloudSync::ClientImported();
    advanced_toggle->setChecked(imported);
    advanced->setVisible(imported);
    advanced_toggle->setArrowType(imported ? Qt::DownArrow : Qt::RightArrow);
    RetranslateUI();
    ShowLocalProfiles();
    UpdateState();
}

ConfigureCloudSync::~ConfigureCloudSync() {
    sign_in_stop.request_stop();
}

void ConfigureCloudSync::ApplyConfiguration() {
    Settings::values.cloud_sync_enabled = enabled->isChecked();
    Settings::values.cloud_sync_web_service = web_service->isChecked();
}

void ConfigureCloudSync::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }
    QWidget::changeEvent(event);
}

void ConfigureCloudSync::showEvent(QShowEvent* event) {
    ShowLocalProfiles();
    UpdateState();
    QWidget::showEvent(event);
}

void ConfigureCloudSync::RetranslateUI() {
    setAccessibleName(tr("Cloud Sync"));
    enabled->setText(tr("Sync profiles and saves with Google Drive"));
    description->setText(tr("Saves are downloaded before a game starts and uploaded after it "
                            "closes. Lichii only sees the files it puts in the Lichii folder of "
                            "your Drive."));
    web_service->setText(tr("Sync the web service login (username and token) on Sync Now"));
    web_service->setToolTip(tr("Only a login of your own is synced, not the default \"Eden\" "
                               "username with the token generated for this device. The token is "
                               "stored in the Lichii folder of your Drive."));
    account_group->setTitle(tr("Google Account"));
    sign_out->setText(tr("Sign Out"));
    copy_code->setText(tr("Copy Code"));
    advanced_toggle->setText(tr("Advanced"));
    advanced_description->setText(tr("Sign in through your own Google Cloud OAuth client (type "
                                     "\"TVs and Limited Input devices\") instead of the one built "
                                     "into Lichii."));
    import_client->setText(tr("Import client_secret.json..."));
    link_group->setTitle(tr("Profile Linking"));
    link_description->setText(tr("A profile that is the same person as a profile on Drive (made "
                                 "on another device) can sync as that one."));
    if (auto* const label = qobject_cast<QLabel*>(link_layout->labelForField(local_profile))) {
        label->setText(tr("Profile on this device:"));
    }
    if (auto* const label = qobject_cast<QLabel*>(link_layout->labelForField(remote_row))) {
        label->setText(tr("Syncs as:"));
    }
    remote_profile->setItemText(0, tr("Itself"));
    load_remote->setText(tr("Load Drive Profiles"));
    link->setText(tr("Link"));
    sync_now->setText(tr("Sync Now"));
    UpdateState();
}

void ConfigureCloudSync::ShowLocalProfiles() {
    if (busy || CloudSync::SyncRunning()) {
        return;
    }
    const int index = local_profile->currentIndex();
    const auto shown =
        index >= 0 ? std::optional{local_uuids[static_cast<std::size_t>(index)]} : std::nullopt;
    const QSignalBlocker blocker{local_profile};
    local_profile->clear();
    local_uuids.clear();
    const auto& profiles = system.GetProfileManager();
    for (const auto& uuid : profiles.GetAllUsers()) {
        if (!uuid.IsValid()) {
            continue;
        }
        local_uuids.push_back(uuid);
        local_profile->addItem(QString::fromStdString(CloudSync::ProfileNickname(profiles, uuid)));
        if (uuid == shown) {
            local_profile->setCurrentIndex(local_profile->count() - 1);
        }
    }
    // The blocker kept the selection change from filling the Drive profiles for it.
    ShowRemoteProfiles();
}

void ConfigureCloudSync::SignIn() {
    if (signing_in) {
        sign_in_stop.request_stop();
        return;
    }
    sign_in_stop = std::stop_source{};
    signing_in = true;
    user_code.clear();
    account_status->setText(tr("Asking Google for a sign-in code..."));
    UpdateState();

    auto* const watcher = new QFutureWatcher<CloudSync::SignInResult>(this);
    connect(watcher, &QFutureWatcher<CloudSync::SignInResult>::finished, this, [this, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        signing_in = false;
        user_code.clear();
        UpdateState();
        switch (result.status) {
        case CloudSync::SignInStatus::SignedIn:
            break;
        case CloudSync::SignInStatus::Denied:
            account_status->setText(tr("You refused access to Google Drive."));
            break;
        case CloudSync::SignInStatus::Expired:
            account_status->setText(tr("The sign-in code expired. Sign in again."));
            break;
        case CloudSync::SignInStatus::Cancelled:
            break;
        case CloudSync::SignInStatus::Failed:
            account_status->setText(
                tr("Sign-in failed: %1").arg(QString::fromStdString(result.error)));
            break;
        }
    });
    const QPointer<ConfigureCloudSync> self{this};
    watcher->setFuture(QtConcurrent::run([self, stop = sign_in_stop.get_token()] {
        return CloudSync::SignIn(
            [self, stop](const CloudSync::DeviceCode& code) {
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(),
                    [self, stop, url = QString::fromStdString(code.verification_url),
                     code_text = QString::fromStdString(code.user_code)] {
                        // Cancelled since: the code is no longer wanted.
                        if (!self || stop.stop_requested()) {
                            return;
                        }
                        self->user_code = code_text;
                        self->account_status->setText(
                            tr("Open %1 and enter the code %2 to let Lichii use Google Drive.")
                                .arg(url, code_text));
                        self->UpdateState();
                        QDesktopServices::openUrl(QUrl{url});
                    },
                    Qt::QueuedConnection);
            },
            stop);
    }));
}

void ConfigureCloudSync::SignOut() {
    sign_out->setEnabled(false);
    auto* const watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher] {
        const bool removed = watcher->result();
        watcher->deleteLater();
        UpdateState();
        if (!removed) {
            QMessageBox::warning(this, tr("Cloud Sync"),
                                 tr("Signed out, but the sign-in file in the cloud_sync folder of "
                                    "the configuration folder could not be deleted. Delete it, or "
                                    "Lichii may be signed in again after a restart."));
        }
    });
    watcher->setFuture(QtConcurrent::run([] { return CloudSync::SignOut(); }));
}

void ConfigureCloudSync::ImportClientSecret() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Select client_secret.json"), {},
                                                      tr("JSON files (*.json);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly) ||
        !CloudSync::ImportClientSecret(file.readAll().toStdString())) {
        QMessageBox::warning(this, tr("Cloud Sync"),
                             tr("This file has no installed-app (TVs and Limited Input devices) "
                                "OAuth client, or it could not be saved."));
        return;
    }
    UpdateState();
    QMessageBox::information(this, tr("Cloud Sync"),
                             tr("The OAuth client was imported. Sign in again to use it."));
}

void ConfigureCloudSync::LoadRemoteProfiles() {
    load_remote->setEnabled(false);
    using Profiles = std::optional<std::vector<CloudSync::RemoteProfile>>;
    auto* const watcher = new QFutureWatcher<Profiles>(this);
    connect(watcher, &QFutureWatcher<Profiles>::finished, this, [this, watcher] {
        const auto profiles = watcher->result();
        watcher->deleteLater();
        load_remote->setEnabled(true);
        if (!profiles) {
            link_status->setText(tr("Could not reach Google Drive."));
            return;
        }
        remote_profiles = *profiles;
        ShowRemoteProfiles();
        UpdateState();
    });
    watcher->setFuture(QtConcurrent::run([] { return CloudSync::ListRemoteProfiles(); }));
}

void ConfigureCloudSync::ShowRemoteProfiles() {
    const int local_index = local_profile->currentIndex();
    const auto local = local_index >= 0
                           ? std::optional{local_uuids[static_cast<std::size_t>(local_index)]}
                           : std::nullopt;
    const int remote_index = remote_profile->currentIndex();
    const auto selected =
        remote_index > 0 ? std::optional{remote_uuids[static_cast<std::size_t>(remote_index - 1)]}
                         : std::nullopt;
    while (remote_profile->count() > 1) {
        remote_profile->removeItem(1);
    }
    remote_uuids.clear();
    for (const auto& profile : remote_profiles) {
        if (profile.uuid == local) {
            continue;
        }
        remote_uuids.push_back(profile.uuid);
        remote_profile->addItem(tr("%1 (%2)").arg(
            QString::fromStdString(profile.nickname),
            QString::fromStdString(profile.uuid.FormattedString().substr(0, 8))));
        if (profile.uuid == selected) {
            remote_profile->setCurrentIndex(remote_profile->count() - 1);
        }
    }
}

void ConfigureCloudSync::Link() {
    const int local_index = local_profile->currentIndex();
    const int remote_index = remote_profile->currentIndex();
    if (local_index < 0 || busy || CloudSync::SyncRunning()) {
        return;
    }
    const auto& profiles = system.GetProfileManager();
    const auto local = local_uuids[static_cast<std::size_t>(local_index)];
    // Deleted in the Profiles tab since the picker was filled.
    if (!profiles.UserExists(local)) {
        ShowLocalProfiles();
        UpdateState();
        return;
    }
    const auto remote =
        remote_index > 0 ? std::optional{remote_uuids[static_cast<std::size_t>(remote_index - 1)]}
                         : std::nullopt;
    if (!CloudSync::LinkProfile(profiles, local, remote)) {
        QMessageBox::warning(this, tr("Cloud Sync"),
                             tr("Another profile on this device already syncs as that profile, "
                                "or the cloud sync state could not be saved."));
    }
    UpdateState();
}

void ConfigureCloudSync::UpdateState() {
    // Doesn't wait for a token refresh in flight, as SignedIn would.
    const bool signed_in = CloudSync::SignedInSnapshot();
    if (!signing_in) {
        account_status->setText(signed_in ? tr("Signed in to Google Drive.")
                                          : tr("Not signed in."));
    }
    if (const auto client = CloudSync::LoadClientCredentials()) {
        const QString id = QString::fromStdString(client->client_id);
        client_status->setText(CloudSync::ClientImported()
                                   ? tr("OAuth client: imported client_secret.json (%1)").arg(id)
                                   : tr("OAuth client: built into this build (%1)").arg(id));
    } else {
        client_status->setText(tr("OAuth client: none. Import a client_secret.json to sign in."));
    }
    sign_in->setText(signing_in ? tr("Cancel Sign-In") : tr("Sign In"));
    sign_in->setEnabled(signing_in || !signed_in);
    sign_out->setEnabled(signed_in && !signing_in);
    copy_code->setVisible(signing_in && !user_code.isEmpty());
    import_client->setEnabled(!signing_in);

    // Linking waits for the sync state, which a running sync holds.
    const bool syncing = busy || CloudSync::SyncRunning();
    const int local_index = local_profile->currentIndex();
    load_remote->setEnabled(signed_in);
    link->setEnabled(signed_in && !syncing && local_index >= 0);
    if (syncing) {
        link_status->setText(tr("A sync is running."));
    } else if (local_index >= 0) {
        const auto linked =
            CloudSync::LinkedProfile(local_uuids[static_cast<std::size_t>(local_index)]);
        link_status->setText(linked ? tr("Syncs as the Drive profile %1.")
                                          .arg(QString::fromStdString(linked->FormattedString()))
                                    : tr("Syncs as itself."));
    } else {
        link_status->clear();
    }
    sync_now->setEnabled(signed_in && enabled->isChecked() && !syncing);
    web_service->setEnabled(enabled->isChecked());
}
