// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include "common/romm_api.h"
#include "common/romm_config.h"
#include "romm_settings_dialog.h"

RommSettingsDialog::RommSettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("RomM Master Login"));
    setMinimumWidth(420);

    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel(
        tr("This is the shared \"master\" account used to browse and download games, updates "
           "and DLC from your RomM instance. Each profile can separately add its own RomM "
           "account for cloud save sync — see that profile's own settings for that."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout();
    instance_url_edit = new QLineEdit(this);
    instance_url_edit->setPlaceholderText(tr("https://romm.example.com"));
    form->addRow(tr("Instance URL:"), instance_url_edit);

    username_edit = new QLineEdit(this);
    form->addRow(tr("Username:"), username_edit);

    password_edit = new QLineEdit(this);
    password_edit->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Password:"), password_edit);
    layout->addLayout(form);

    auto* test_row = new QVBoxLayout();
    test_button = new QPushButton(tr("Test Connection"), this);
    status_label = new QLabel(this);
    status_label->setWordWrap(true);
    test_row->addWidget(test_button);
    test_row->addWidget(status_label);
    layout->addLayout(test_row);

    clear_button = new QPushButton(tr("Remove Account"), this);
    layout->addWidget(clear_button);

    auto* button_box =
        new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    layout->addWidget(button_box);

    connect(test_button, &QPushButton::clicked, this, &RommSettingsDialog::OnTestConnection);
    connect(clear_button, &QPushButton::clicked, this, &RommSettingsDialog::OnClear);
    connect(button_box, &QDialogButtonBox::accepted, this, &RommSettingsDialog::OnSave);
    connect(button_box, &QDialogButtonBox::rejected, this, &RommSettingsDialog::reject);

    LoadFromConfig();
}

void RommSettingsDialog::LoadFromConfig() {
    const auto credentials = Common::RommConfig::GetMasterCredentials();
    instance_url_edit->setText(QString::fromStdString(credentials.instance_url));
    username_edit->setText(QString::fromStdString(credentials.username));
    password_edit->clear();
    clear_button->setEnabled(credentials.IsSet());
    if (!credentials.IsSet()) {
        password_edit->setPlaceholderText(QString{});
        status_label->clear();
    } else {
        // The saved password lives in the system credential store, never in these fields.
        password_edit->setPlaceholderText(tr("Leave empty to keep the saved login"));
        status_label->setText(
            tr("Saved login: %1.").arg(QString::fromStdString(credentials.username)));
    }
}

namespace {

struct LoginForm {
    std::string instance_url;
    std::string username;
    std::string password;
};

} // Anonymous namespace

void RommSettingsDialog::OnTestConnection() {
    const LoginForm form{
        .instance_url = instance_url_edit->text().trimmed().toStdString(),
        .username = username_edit->text().trimmed().toStdString(),
        .password = password_edit->text().toStdString(),
    };
    const auto stored = Common::RommConfig::GetMasterCredentials();
    if (form.password.empty() && (!stored.IsSet() || stored.instance_url != form.instance_url ||
                                  stored.username != form.username)) {
        status_label->setText(tr("Enter your password to test a new login."));
        return;
    }

    test_button->setEnabled(false);
    status_label->setText(tr("Testing..."));

    auto* watcher = new QFutureWatcher<Common::RommApi::TestConnectionResult>(this);
    connect(watcher, &QFutureWatcher<Common::RommApi::TestConnectionResult>::finished, this,
            [this, watcher]() {
                const auto result = watcher->result();
                watcher->deleteLater();
                test_button->setEnabled(true);
                if (result.ok) {
                    status_label->setText(result.server_version.empty()
                                              ? tr("Connected successfully.")
                                              : tr("Connected successfully (RomM %1).")
                                                    .arg(QString::fromStdString(
                                                        result.server_version)));
                } else {
                    status_label->setText(
                        tr("Connection failed: %1").arg(QString::fromStdString(result.error)));
                }
            });
    watcher->setFuture(QtConcurrent::run([form, stored]() {
        // With a password typed, test that login for real (without saving anything); otherwise
        // test the saved one.
        if (form.password.empty()) {
            return Common::RommApi::TestConnection(stored);
        }
        const auto login = Common::RommApi::Login(form.instance_url, form.username, form.password,
                                                  Common::RommConfig::CatalogScope);
        if (!login.ok) {
            Common::RommApi::TestConnectionResult failed;
            failed.error = login.error;
            return failed;
        }
        return Common::RommApi::TestConnection(login.credentials);
    }));
}

void RommSettingsDialog::OnSave() {
    const LoginForm form{
        .instance_url = instance_url_edit->text().trimmed().toStdString(),
        .username = username_edit->text().trimmed().toStdString(),
        .password = password_edit->text().toStdString(),
    };
    const auto stored = Common::RommConfig::GetMasterCredentials();

    if (form.password.empty()) {
        // Nothing to log in with: that's only fine if nothing is changing.
        if (stored.IsSet() && stored.instance_url == form.instance_url &&
            stored.username == form.username) {
            accept();
        } else if (!stored.IsSet() && form.instance_url.empty() && form.username.empty()) {
            accept();
        } else {
            status_label->setText(tr("Enter your password to log in."));
        }
        return;
    }

    test_button->setEnabled(false);
    status_label->setText(tr("Logging in..."));

    auto* watcher = new QFutureWatcher<Common::RommApi::LoginResult>(this);
    connect(watcher, &QFutureWatcher<Common::RommApi::LoginResult>::finished, this,
            [this, watcher, form]() {
                const auto login = watcher->result();
                watcher->deleteLater();
                test_button->setEnabled(true);
                if (!login.ok) {
                    status_label->setText(
                        tr("Login failed: %1").arg(QString::fromStdString(login.error)));
                    return;
                }
                // The password goes into the system credential store; if that refuses, nothing
                // is saved (it is never written anywhere else).
                std::string save_error;
                if (!Common::RommConfig::Save(login.credentials, form.password, save_error)) {
                    status_label->setText(
                        tr("Couldn't store the password in the system credential store: %1")
                            .arg(QString::fromStdString(save_error)));
                    return;
                }
                accept();
            });
    watcher->setFuture(QtConcurrent::run([form]() {
        return Common::RommApi::Login(form.instance_url, form.username, form.password, Common::RommConfig::CatalogScope);
    }));
}

void RommSettingsDialog::OnClear() {
    Common::RommApi::ForgetSession(Common::RommConfig::GetMasterCredentials());
    Common::RommConfig::Clear();
    instance_url_edit->clear();
    username_edit->clear();
    password_edit->clear();
    password_edit->setPlaceholderText(QString{});
    status_label->clear();
    clear_button->setEnabled(false);
}
