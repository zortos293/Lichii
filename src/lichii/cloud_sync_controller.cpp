// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <limits>
#include <thread>
#include <utility>

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QLocale>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QTimer>
#include <QWidget>
#include <QtConcurrent/QtConcurrent>

#include "common/settings.h"
#include "core/core.h"
#include "frontend_common/cloud_sync/google_auth.h"
#include "lichii/cloud_sync_controller.h"
#include "qt_common/qt_common.h"

namespace CloudSync = FrontendCommon::CloudSync;

namespace {

CloudSyncController* instance = nullptr;

QString FormatTime(std::chrono::system_clock::time_point time) {
    if (time.time_since_epoch().count() == 0) {
        return QCoreApplication::translate("CloudSyncController", "unknown");
    }
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
    return QLocale{}.toString(QDateTime::fromSecsSinceEpoch(seconds), QLocale::LongFormat);
}

QStringList Errors(const CloudSync::SyncResult& result) {
    QStringList errors;
    for (const auto& error : result.errors) {
        errors.push_back(QString::fromStdString(error));
    }
    return errors;
}

} // namespace

CloudSyncController::CloudSyncController(QWidget* window_) : QObject(window_), window{window_} {
    instance = this;
}

CloudSyncController::~CloudSyncController() {
    Shutdown();
    if (instance == this) {
        instance = nullptr;
    }
}

void CloudSyncController::Shutdown() {
    closing = true;
    // Answered first: a sync waiting for an answer would keep the wait below from ending.
    // Taken out first: a closed box's finished handler erases from prompts.
    const auto open_prompts = std::exchange(prompts, {});
    for (const auto& [box, answer] : open_prompts) {
        answer(CloudSync::ConflictChoice::Skip);
        if (box) {
            box->close();
        }
    }
    // A push started as the game closed gets ShutdownGraceSeconds to finish before it is stopped.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{ShutdownGraceSeconds};
    for (const auto& future : futures) {
        while (future.isRunning()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                stop.request_stop();
            }
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    }
    stop.request_stop();
    futures.clear();
}

CloudSyncController* CloudSyncController::Instance() {
    return instance;
}

bool CloudSyncController::Enabled() const {
    return Settings::values.cloud_sync_enabled.GetValue() && CloudSync::SignedInSnapshot();
}

bool CloudSyncController::Busy() const {
    return running > 0;
}

void CloudSyncController::SetRunning(int change) {
    running += change;
    emit BusyChanged(running > 0);
}

void CloudSyncController::SetTitleSource(std::function<std::vector<CloudSync::Title>()> source) {
    title_source = std::move(source);
}

CloudSync::SyncCallbacks CloudSyncController::Callbacks() {
    return {
        .resolve_conflict =
            [this](const CloudSync::Conflict& conflict, std::stop_token cancel) {
                // Set on the GUI thread once the question shows, to close it when the wait ends
                // without an answer.
                const auto box = std::make_shared<QPointer<QMessageBox>>();
                const auto choice = CloudSync::WaitForAnswer(cancel, [&](Answer give) {
                    QMetaObject::invokeMethod(
                        this,
                        [this, conflict, box, answer = std::move(give)] {
                            *box = AskConflict(conflict, answer);
                        },
                        Qt::QueuedConnection);
                });
                QMetaObject::invokeMethod(
                    this,
                    [box] {
                        if (*box) {
                            (*box)->close();
                        }
                    },
                    Qt::QueuedConnection);
                return choice;
            },
        .progress = {},
    };
}

QMessageBox* CloudSyncController::AskConflict(const CloudSync::Conflict& conflict, Answer answer) {
    if (closing) {
        answer(CloudSync::ConflictChoice::Skip);
        return nullptr;
    }
    if (conflict.web_service) {
        auto* const box = new QMessageBox(
            QMessageBox::Question, tr("Cloud Sync Conflict"),
            tr("The web service login changed both on this device and on Google Drive.\n\n"
               "This device: %1\nGoogle Drive: %2 (%3)\n\n"
               "Which one do you want to keep? Google Drive keeps old versions of its login.")
                .arg(QString::fromStdString(conflict.nickname),
                     QString::fromStdString(conflict.remote_nickname),
                     FormatTime(conflict.remote_time)),
            QMessageBox::NoButton, window);
        return ShowConflict(box, std::move(answer));
    }
    QString what;
    if (conflict.title_id == 0) {
        what = tr("The profile %1").arg(QString::fromStdString(conflict.nickname));
    } else if (conflict.nickname.empty()) {
        what = tr("The shared save of %1").arg(QString::fromStdString(conflict.title_name));
    } else {
        what = tr("%1's save of %2")
                   .arg(QString::fromStdString(conflict.nickname),
                        QString::fromStdString(conflict.title_name));
    }
    auto* const box = new QMessageBox(
        QMessageBox::Question, tr("Cloud Sync Conflict"),
        tr("%1 changed both on this device and on Google Drive.\n\n"
           "This device: %2\nGoogle Drive: %3\n\n"
           "Which one do you want to keep? The other one is kept as a backup "
           "(Google Drive keeps old versions; this device keeps backups in the "
           "cloud_sync folder of the configuration folder).")
            .arg(what, FormatTime(conflict.local_time), FormatTime(conflict.remote_time)),
        QMessageBox::NoButton, window);
    return ShowConflict(box, std::move(answer));
}

QMessageBox* CloudSyncController::ShowConflict(QMessageBox* box, Answer answer) {
    box->setAttribute(Qt::WA_DeleteOnClose);
    // Names from profiles and Drive, not markup.
    box->setTextFormat(Qt::PlainText);
    // Over the progress dialog of a pull before boot, which is application modal too.
    box->setWindowModality(Qt::ApplicationModal);
    auto* const keep_local = box->addButton(tr("Keep This Device's"), QMessageBox::AcceptRole);
    auto* const keep_remote = box->addButton(tr("Keep Google Drive's"), QMessageBox::AcceptRole);
    box->addButton(tr("Skip"), QMessageBox::RejectRole);
    prompts.emplace_back(box, answer);
    connect(box, &QMessageBox::finished, this, [this, box, keep_local, keep_remote, answer] {
        const auto* const clicked = box->clickedButton();
        answer(clicked == keep_local    ? CloudSync::ConflictChoice::KeepLocal
               : clicked == keep_remote ? CloudSync::ConflictChoice::KeepRemote
                                        : CloudSync::ConflictChoice::Skip);
        std::erase_if(prompts, [box](const auto& prompt) { return prompt.first == box; });
    });
    box->show();
    return box;
}

void CloudSyncController::PrepareBoot(const CloudSync::Title& title) {
    const bool pull = Enabled();
    QProgressDialog progress(tr("Waiting for the cloud sync of this game to stop..."), tr("Cancel"),
                             0, 0, window);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setWindowFlag(Qt::WindowCloseButtonHint, false);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    progress.setMinimumDuration(std::numeric_limits<int>::max());

    std::stop_source pull_stop;
    bool timed_out = false;
    connect(&progress, &QProgressDialog::canceled, &progress, [&] {
        pull_stop.request_stop();
        progress.setLabelText(tr("Stopping..."));
    });
    QTimer budget;
    budget.setSingleShot(true);
    connect(&budget, &QTimer::timeout, &progress, [&] {
        timed_out = true;
        pull_stop.request_stop();
        progress.setLabelText(tr("Stopping..."));
    });
    // Shown only when this takes a while, so that a boot without anything to wait for doesn't
    // flash it.
    QTimer show_later;
    show_later.setSingleShot(true);
    connect(&show_later, &QTimer::timeout, &progress, [&progress] { progress.show(); });

    SetRunning(1);
    QFutureWatcher<CloudSync::SyncResult> watcher;
    QEventLoop loop;
    connect(&watcher, &QFutureWatcher<CloudSync::SyncResult>::finished, &loop, &QEventLoop::quit);
    const QPointer<QProgressDialog> label{&progress};
    watcher.setFuture(QtConcurrent::run([this, title, pull, label, callbacks = Callbacks(),
                                         token = pull_stop.get_token()] {
        // Not cut short by Cancel: nothing may touch the title once it boots.
        guest_registration = CloudSync::BeginGuest(title.id);
        if (!pull || token.stop_requested()) {
            return CloudSync::SyncResult{.cancelled = token.stop_requested()};
        }
        QMetaObject::invokeMethod(
            this,
            [label] {
                if (label) {
                    label->setLabelText(tr("Syncing saves with Google Drive..."));
                }
            },
            Qt::QueuedConnection);
        return CloudSync::PullTitle(QtCommon::system->GetProfileManager(), title, callbacks, token);
    }));
    budget.start(PullBudgetSeconds * 1000);
    show_later.start(300);
    loop.exec();
    budget.stop();
    show_later.stop();
    SetRunning(-1);

    const auto result = watcher.result();
    if (timed_out) {
        QMessageBox::information(
            window, tr("Cloud Sync"),
            tr("Syncing the saves took too long. The game starts with the saves on this device."));
    } else if (!result.cancelled && !result.errors.empty()) {
        QMessageBox::warning(window, tr("Cloud Sync"),
                             tr("Some saves could not be synced; the game starts with the saves "
                                "on this device.\n\n%1")
                                 .arg(Errors(result).join(QLatin1Char('\n'))));
    }
}

void CloudSyncController::GuestStopped(const CloudSync::Title& title, bool ran) {
    CloudSync::EndGuest(guest_registration);
    if (!ran || closing || !Enabled()) {
        return;
    }
    RunInBackground(
        tr("Uploading the saves of %1").arg(QString::fromStdString(title.name)),
        [title, callbacks = Callbacks()](std::stop_token token) {
            return CloudSync::PushTitle(QtCommon::system->GetProfileManager(), title, callbacks,
                                        token);
        },
        false);
}

void CloudSyncController::SyncTitle(const CloudSync::Title& title) {
    if (Busy()) {
        return;
    }
    RunInBackground(
        tr("Syncing the saves of %1").arg(QString::fromStdString(title.name)),
        [title, callbacks = Callbacks()](std::stop_token token) {
            return CloudSync::SyncSaves(QtCommon::system->GetProfileManager(), {&title, 1},
                                        callbacks, token);
        },
        true);
}

void CloudSyncController::SyncAll() {
    auto titles = title_source ? title_source() : std::vector<CloudSync::Title>{};
    std::optional<std::optional<CloudSync::WebServiceLogin>> web_service;
    if (Settings::values.cloud_sync_web_service.GetValue()) {
        web_service = CloudSync::LocalWebServiceLogin();
    }
    RunInBackground(
        tr("Syncing profiles and saves"),
        [selected = std::move(titles), web_service,
         callbacks = Callbacks()](std::stop_token token) {
            return CloudSync::SyncAll(QtCommon::system->GetProfileManager(), selected, callbacks,
                                      token, web_service);
        },
        true);
}

void CloudSyncController::RunInBackground(const QString& what, Work work, bool report_in_dialog) {
    if (closing) {
        return;
    }
    SetRunning(1);
    emit StatusMessage(tr("%1...").arg(what));
    auto future =
        QtConcurrent::run([job = std::move(work), token = stop.get_token()] { return job(token); });
    std::erase_if(futures, [](const auto& done) { return done.isFinished(); });
    futures.push_back(future);
    auto* watcher = new QFutureWatcher<CloudSync::SyncResult>(this);
    connect(watcher, &QFutureWatcher<CloudSync::SyncResult>::finished, this,
            [this, watcher, what, report_in_dialog] {
                const auto result = watcher->result();
                watcher->deleteLater();
                if (result.web_service_login) {
                    Settings::values.eden_username = result.web_service_login->username;
                    Settings::values.eden_token = result.web_service_login->token;
                    emit SettingsChanged();
                }
                SetRunning(-1);
                Report(what, result, report_in_dialog);
            });
    watcher->setFuture(future);
}

void CloudSyncController::Report(const QString& what, const CloudSync::SyncResult& result,
                                 bool report_in_dialog) {
    if (closing || result.cancelled) {
        return;
    }
    if (result.errors.empty()) {
        emit StatusMessage(tr("%1: done (%2 uploaded, %3 downloaded, %4 skipped).")
                               .arg(what)
                               .arg(result.pushed)
                               .arg(result.pulled)
                               .arg(result.skipped));
        if (report_in_dialog) {
            QMessageBox::information(window, tr("Cloud Sync"),
                                     tr("Synced: %1 uploaded, %2 downloaded, %3 skipped.")
                                         .arg(result.pushed)
                                         .arg(result.pulled)
                                         .arg(result.skipped));
        }
        return;
    }
    const QStringList errors = Errors(result);
    emit StatusMessage(tr("%1 failed: %2").arg(what, errors.front()));
    if (report_in_dialog) {
        QMessageBox::warning(window, tr("Cloud Sync"),
                             tr("%1 failed:\n\n%2").arg(what, errors.join(QLatin1Char('\n'))));
    }
}
