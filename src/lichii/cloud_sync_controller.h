// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <stop_token>
#include <vector>

#include <QFuture>
#include <QObject>
#include <QPointer>
#include <QString>

#include "common/common_types.h"
#include "frontend_common/cloud_sync/sync_engine.h"

class QMessageBox;
class QWidget;

// Google Drive sync (frontend_common/cloud_sync) for the Qt frontend: the dialogs, timers and
// status messages around the shared core's pull before boot, push after exit, "Sync save" and
// "Sync now". Syncs run on worker threads; conflicts are asked on the GUI thread without blocking
// it.
class CloudSyncController : public QObject {
    Q_OBJECT

public:
    // How long the pull before boot may run before it is asked to stop and the game starts
    // without it.
    static constexpr int PullBudgetSeconds = 20;
    // How long Shutdown lets running syncs finish before stopping them.
    static constexpr int ShutdownGraceSeconds = 15;

    explicit CloudSyncController(QWidget* window);
    ~CloudSyncController() override;

    // The one the main window made, for the configuration tabs; nullptr without one.
    static CloudSyncController* Instance();

    // Sync is on in the settings and an account was signed in when last seen. Doesn't wait.
    bool Enabled() const;
    // A sync this frontend started runs.
    bool Busy() const;

    // The game list's games, for "Sync now".
    void SetTitleSource(std::function<std::vector<FrontendCommon::CloudSync::Title>()> source);

    // Before a title boots, sync on or off: stops the syncs touching it and waits for them
    // (BeginGuest), then pulls its saves if sync is on. A modal dialog appears when this takes a
    // while; its Cancel and PullBudgetSeconds stop only the pull. Returns once nothing touches the
    // title. GuestStopped must follow, whether the title booted or not.
    void PrepareBoot(const FrontendCommon::CloudSync::Title& title);
    // After the guest stopped or failed to boot: ends BeginGuest, and pushes the title's saves in
    // the background if it ran.
    void GuestStopped(const FrontendCommon::CloudSync::Title& title, bool ran);
    // Syncs one title's saves both ways, in the background, without the profiles.
    void SyncTitle(const FrontendCommon::CloudSync::Title& title);
    // Syncs the profiles (unless a game runs) and every game's saves, in the background.
    void SyncAll();

    // Answers the open conflict questions with Skip, lets the syncs still running finish for a
    // while, then stops them and waits for them. For the main window to call while closing.
    void Shutdown();

signals:
    void BusyChanged(bool busy);
    void StatusMessage(const QString& message);
    // A sync changed the settings (the web service login): they should be saved.
    void SettingsChanged();

private:
    using Work = std::function<FrontendCommon::CloudSync::SyncResult(std::stop_token)>;
    using Answer = std::function<void(FrontendCommon::CloudSync::ConflictChoice)>;

    FrontendCommon::CloudSync::SyncCallbacks Callbacks();
    // Shows the question without blocking and answers through answer when the user picks.
    QMessageBox* AskConflict(const FrontendCommon::CloudSync::Conflict& conflict, Answer answer);
    // Adds the choices to the question, shows it and answers through answer.
    QMessageBox* ShowConflict(QMessageBox* box, Answer answer);
    void RunInBackground(const QString& what, Work work, bool report_in_dialog);
    void Report(const QString& what, const FrontendCommon::CloudSync::SyncResult& result,
                bool report_in_dialog);
    void SetRunning(int change);

    QWidget* window;
    std::stop_source stop;
    bool closing{};
    int running{};
    // PrepareBoot's BeginGuest, for GuestStopped to end.
    u64 guest_registration{};
    std::function<std::vector<FrontendCommon::CloudSync::Title>()> title_source;
    // The background syncs, for Shutdown to wait for.
    std::vector<QFuture<FrontendCommon::CloudSync::SyncResult>> futures;
    // The open conflict questions, for Shutdown to answer.
    std::vector<std::pair<QPointer<QMessageBox>, Answer>> prompts;
};
