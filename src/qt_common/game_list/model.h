// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <QFileSystemWatcher>
#include <QStandardItemModel>
#include <QStringList>
#include <QVector>

#include "common/common_types.h"
#include "frontend_common/play_time_manager.h"
#include "qt_common/config/uisettings.h"

namespace Core {
class System;
}

class GameListDir;
class GameListWorker;
class QStandardItem;

namespace FileSys {
class ManualContentProvider;
class VfsFilesystem;
} // namespace FileSys

class GameListModel : public QStandardItemModel {
    Q_OBJECT

public:
    enum Column {
        COLUMN_NAME,
        COLUMN_FILE_TYPE,
        COLUMN_SIZE,
        COLUMN_PLAY_TIME,
        COLUMN_ADD_ONS,
        COLUMN_COUNT,
    };

    explicit GameListModel(std::shared_ptr<FileSys::VfsFilesystem> vfs_,
                           FileSys::ManualContentProvider* provider_,
                           const PlayTime::PlayTimeManager& play_time_manager_,
                           Core::System& system_, QObject* parent = nullptr);
    ~GameListModel() override;

    void AddDirEntry(GameListDir* entry_items);
    void AddEntry(const QList<QStandardItem*>& entry_items, GameListDir* parent);
    void DonePopulating(const QStringList& watch_list);
    /// Shows the size of a loose dump's folder in every row of that game (favorites included).
    void SetFolderSize(const QString& game_path, u64 size_bytes);
    /// Sorts by the column, then favorites first in every folder (a second, stable pass), so
    /// every view showing the list (List, Modern grid, Carousel, the styles) has them in front.
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

    void PopulateAsync(QVector<UISettings::GameDir>& game_dirs);
    void WorkerEvent();
    void SetScanEnabled(bool enabled, bool stop_worker = true);

    bool IsEmpty() const;

    void ToggleFavorite(u64 program_id);

    void RefreshGameDirectory();
    void RefreshExternalContent();
    void ResetExternalWatcher();

    void RetranslateUI();

    QFileSystemWatcher* GetWatcher() const;

signals:
    void ShowList(bool show);
    void PopulatingCompleted(const QStringList& watch_list);
    void PopulatingStarted();
    void SaveConfig();

private:
    friend class GameListWorker;

    void AddFavorite(u64 program_id);
    void RemoveFavorite(u64 program_id);
    void Repopulate();
    void StopWorker();

    bool scan_enabled = true;

    std::shared_ptr<FileSys::VfsFilesystem> vfs;
    FileSys::ManualContentProvider* provider;
    const PlayTime::PlayTimeManager& play_time_manager;
    Core::System& system;

    std::unique_ptr<GameListWorker> current_worker;
    QFileSystemWatcher* watcher = nullptr;
    QFileSystemWatcher* external_watcher = nullptr;
};
