// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <future>
#include <memory>
#include <optional>
#include <vector>

#include "common/common_types.h"

#include <QByteArray>
#include <QJSValue>
#include <QObject>
#include <QString>
#include <QStringList>

class QJSEngine;

/// One line of the panel: a value, a tick (check is 0 or 1) or a bar (done and total).
struct NotesRow {
    QString title;
    QString value;
    int check = -1;
    int done = -1;
    int total = 0;
};

struct NotesView {
    QString name;
    std::vector<NotesRow> rows;
};

struct NotesTab {
    QString name;
    std::vector<NotesView> views;
};

/// What a module's read() returned.
struct NotesReport {
    QString title;
    std::vector<NotesRow> keys; ///< The few numbers the strip keeps in view.
    std::vector<NotesTab> tabs;
};

/// The only things a module can reach: no disk or network beyond this.
class NotesHost : public QObject {
    Q_OBJECT

public:
    NotesHost(QJSEngine* engine, QStringList library_dirs);
    ~NotesHost() override;

    void SetSlot(const QString& directory);

    Q_INVOKABLE QByteArray unzip(const QByteArray& data, int offset, int end);
    Q_INVOKABLE QString utf8(const QByteArray& data, int offset, int size);
    Q_INVOKABLE QByteArray file(const QString& name);
    Q_INVOKABLE void include(const QString& name);
    /// Up to 4 KiB of the running game's memory, read only; null when a page is not mapped.
    Q_INVOKABLE QJSValue memory(const QString& address, int size);
    /// Looks for a byte pattern (hex) in the game's heap on a worker thread: null while it runs,
    /// then the addresses found (hex strings, at most 64).
    Q_INVOKABLE QJSValue search(const QString& pattern);
    /// A small text the module keeps between sessions, in notes/state.
    Q_INVOKABLE QString load(const QString& name);
    Q_INVOKABLE void store(const QString& name, const QString& text);

private:
    QString StatePath(const QString& name) const;

    QJSEngine* engine;
    QStringList libraries;
    QString slot;
    std::atomic<bool> stopping{false};
    QString search_pattern;
    std::future<std::vector<u64>> search_job;
};

/// A game's module: a script that knows its save. The emulator knows no save format.
class NotesScript {
public:
    NotesScript();
    ~NotesScript();

    [[nodiscard]] bool Load(const QString& path, const QStringList& library_dirs, QString& error);
    [[nodiscard]] std::optional<NotesReport> Read(const QString& path, QString& error);
    /// Calls the module's tick(), if it has one, ten times a second while the game runs.
    /// True when the module asks for its report to be read again.
    [[nodiscard]] bool Tick(QString& error);

    [[nodiscard]] const QString& Title() const {
        return title;
    }
    [[nodiscard]] const QString& Accent() const {
        return accent;
    }
    [[nodiscard]] const QString& Folder() const {
        return folder;
    }
    [[nodiscard]] const QString& File() const {
        return file;
    }

private:
    std::unique_ptr<QJSEngine> engine;
    std::unique_ptr<NotesHost> host;
    QString title;
    QString accent;
    QString folder;
    QString file;
    bool tick_stopped = false; ///< tick() ran over its time: it is not called again until Load.
};
