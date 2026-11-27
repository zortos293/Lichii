// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QString>
#include <QThread>

#include <qtkeychain/keychain.h>

#include "common/romm_config.h"
#include "romm_secret_store.h"

namespace RommSecretStore {

namespace {

// Entries show up under this name in the credential store (e.g. in KWallet or Seahorse).
const QString ServiceName = QStringLiteral("Lichii RomM");

// Runs `fn` on the UI thread and returns what it returns; from the UI thread itself it just calls
// it. Every QtKeychain job has to be created, run and destroyed there: the jobs hand themselves to
// a process-wide executor that lives on the UI thread, and a job built on a worker thread makes
// that executor receive queued calls (QKeychain::JobExecutor::jobFinished, jobDestroyed) that
// outlive the job, which is a local of the caller. The result is a segmentation fault in the UI
// thread a moment after a game stops and the upload of its save starts on a worker thread.
template <typename Fn>
bool OnUiThread(Fn&& fn) {
    QCoreApplication* const app = QCoreApplication::instance();
    if (app == nullptr || QThread::currentThread() == app->thread()) {
        return fn();
    }
    bool result = false;
    QMetaObject::invokeMethod(
        app, [&] { result = fn(); }, Qt::BlockingQueuedConnection);
    return result;
}

// Runs a QtKeychain job to completion. The jobs are asynchronous, so wait on a local event loop:
// that works from the UI thread (a nested loop while e.g. a wallet unlock prompt is open). Only
// call it on the UI thread (see OnUiThread).
void RunToCompletion(QKeychain::Job& job) {
    job.setAutoDelete(false);
    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();
}

std::string ErrorMessage(const QKeychain::Job& job) {
    const QString message = job.errorString();
    if (!message.isEmpty()) {
        return message.toStdString();
    }
    return "The system credential store reported an unknown error";
}

bool ReadOnUiThread(const std::string& key, std::string& value, std::string& error) {
    QKeychain::ReadPasswordJob job{ServiceName};
    job.setKey(QString::fromStdString(key));
    RunToCompletion(job);

    if (job.error() == QKeychain::NoError) {
        value = job.textData().toStdString();
        return true;
    }
    if (job.error() != QKeychain::EntryNotFound) {
        error = ErrorMessage(job);
    }
    return false;
}

bool WriteOnUiThread(const std::string& key, const std::string& value, std::string& error) {
    QKeychain::WritePasswordJob job{ServiceName};
    job.setKey(QString::fromStdString(key));
    job.setTextData(QString::fromStdString(value));
    RunToCompletion(job);

    if (job.error() != QKeychain::NoError) {
        error = ErrorMessage(job);
        return false;
    }
    return true;
}

bool RemoveOnUiThread(const std::string& key, std::string& error) {
    QKeychain::DeletePasswordJob job{ServiceName};
    job.setKey(QString::fromStdString(key));
    RunToCompletion(job);

    if (job.error() != QKeychain::NoError && job.error() != QKeychain::EntryNotFound) {
        error = ErrorMessage(job);
        return false;
    }
    return true;
}

bool Read(const std::string& key, std::string& value, std::string& error) {
    return OnUiThread([&] { return ReadOnUiThread(key, value, error); });
}

bool Write(const std::string& key, const std::string& value, std::string& error) {
    return OnUiThread([&] { return WriteOnUiThread(key, value, error); });
}

bool Remove(const std::string& key, std::string& error) {
    return OnUiThread([&] { return RemoveOnUiThread(key, error); });
}

} // Anonymous namespace

void Install() {
    Common::RommConfig::Secrets::SetBackend({
        .read = Read,
        .write = Write,
        .remove = Remove,
    });
}

} // namespace RommSecretStore
