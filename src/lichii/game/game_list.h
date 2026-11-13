// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2015 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QPushButton>
#include <QStandardItemModel>
#include <QString>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#include "common/common_types.h"
#include "core/core.h"
#include "frontend_common/play_time_manager.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/model.h"
#include "qt_common/util/game.h"

class QVariantAnimation;

class GameCard;
class GameListModel;
class GameTree;
class GameGrid;
class GameListSearchField;
class ControllerNavigation;
class GameListWorker;
class GameListDir;
class MainWindow;
enum class AmLaunchType;
enum class StartGameType;

class GameCarousel;

class QAbstractItemView;
class QKeyEvent;
namespace LibraryStyles {
class FilterModel;
class LibraryStyleView;
struct StyleInfo;
} // namespace LibraryStyles
namespace Core {
class System;
}

namespace FileSys {
class ManualContentProvider;
class VfsFilesystem;
} // namespace FileSys

enum class GameListOpenTarget {
    SaveData,
    ModData,
};

enum class DumpRomFSTarget {
    Normal,
    SDMC,
};

class GameList : public QWidget {
    Q_OBJECT

public:
    explicit GameList(std::shared_ptr<FileSys::VfsFilesystem> vfs_,
                      FileSys::ManualContentProvider* provider_,
                      PlayTime::PlayTimeManager& play_time_manager_, Core::System& system_,
                      MainWindow* parent = nullptr);
    ~GameList() override;

    QString GetLastFilterResultItem() const;
    void ClearFilter();
    void SetFilterFocus();
    void SetFilterVisible(bool visibility);
    bool IsEmpty() const;

    void PopulateAsync(QVector<UISettings::GameDir>& game_dirs);
    void SetScanEnabled(bool enabled);
    void SetReportableProgramId(u64 program_id);

    void SaveInterfaceLayout();
    void LoadInterfaceLayout();

    QStandardItemModel* GetModel() const;

    /// Disables events from the emulated controller
    void UnloadController();

    void ResetViewMode();

    /// Hands the persisted Names and Info panel toggles to the active style.
    void UpdateStyleToggles();

public slots:
    void SetNupConversionRunning(bool running);
    void SetNxToolRunning(bool running);
    void RefreshGameDirectory();
    void RefreshExternalContent();
    void UpdateIconSizes();
    void RefreshThemeIcons();
    void OnPopulate();

signals:
    void ReportCompatibilityRequested(u64 program_id);
    void BootGame(const QString& game_path, StartGameType type);
    void GameChosen(const QString& game_path, const u64 title_id = 0);
    void OpenFolderRequested(u64 program_id, GameListOpenTarget target,
                             const std::string& game_path);
    void OpenTransferableShaderCacheRequested(u64 program_id);
    void PrepareShaderWarmRequested(u64 program_id, const std::string& game_path);
    void DeleteShaderWarmRequested(u64 program_id);
    void RemoveInstalledEntryRequested(u64 program_id, QtCommon::Game::InstalledEntryType type);
    void RemoveFileRequested(u64 program_id, QtCommon::Game::GameListRemoveTarget target,
                             const std::string& game_path);
    void RemovePlayTimeRequested(u64 program_id);
    void SetPlayTimeRequested(u64 program_id);
    void DumpRomFSRequested(u64 program_id, const std::string& game_path, DumpRomFSTarget target);
    void VerifyIntegrityRequested(const std::string& game_path);
    void ConvertToNupRequested(const std::string& game_path);
    void ExtractNupRequested(const std::string& game_path);
    void EditNupMetadataRequested(const std::string& game_path);
    void ConvertToNxRequested(const std::string& game_path);
    void AddToNxRequested(const std::string& game_path);
    void RemoveFromNxRequested(const std::string& game_path);
    void EditNxMetadataRequested(const std::string& game_path);
    void CopyTIDRequested(u64 program_id);
    void CreateShortcut(u64 program_id, const std::string& game_path,
                        const QtCommon::Game::ShortcutTarget target);
    void OpenPerGameGeneralRequested(const std::string& file);
    void LinkToRyujinxRequested(const u64& program_id);
    void OpenDirectory(const QString& directory);
    void AddDirectory();
    void ShowList(bool show);
    void PopulatingCompleted();
    void SaveConfig();

    /// Shared library style keys: B toggles the names, I / Y the info panel, [ and ] (LB / RB)
    /// switch to the previous or next style. R / X changed the style's own option to `label`.
    void ToggleNamesRequested();
    void ToggleInfoRequested();
    void CycleStyleRequested(int step);
    void StyleOptionChanged(const QString& label);

private slots:
    void OnTextChanged(const QString& new_text);
    void OnFilterCloseClicked();
    void OnPopulatingCompleted(const QStringList& watch_list);

private:
    void SetupViews();
    void SetupScrollAnimation();
    bool eventFilter(QObject* obj, QEvent* event) override;
    void changeEvent(QEvent*) override;

    void ValidateEntry(const QModelIndex& item);

    void ToggleFavorite(u64 program_id);
    void ReturnToGameDirectories();
    void UpdateFolderNavigation();

    void PopupContextMenu(const QPoint& menu_location);
    void PopupContextMenuAt(const QModelIndex& item, const QPoint& global_pos);

    void FocusStyleView();
    void ActivateStyle(const LibraryStyles::StyleInfo* info);
    bool IsStyleObject(const QObject* object) const;
    bool HandleStyleKey(QObject* target, const QKeyEvent* event);
    /// A controller button as a key press; held directions come again with auto_repeat set.
    void OnControllerKey(Qt::Key key, bool auto_repeat);
    void OnStyleLaunch(const QModelIndex& index);
    void OnStyleContextMenu(const QModelIndex& index, const QPoint& global_pos);
    void RestoreStyleSelection();
    void AddGamePopup(QMenu& context_menu, u64 program_id, const std::string& path);
    void AddCustomDirPopup(QMenu& context_menu, QModelIndex selected);
    void AddPermDirPopup(QMenu& context_menu, QModelIndex selected);
    void AddFavoritesPopup(QMenu& context_menu);

    void RetranslateUI();

    friend class GameListSearchField;

    std::shared_ptr<FileSys::VfsFilesystem> vfs;
    FileSys::ManualContentProvider* provider;
    GameListSearchField* search_field = nullptr;
    MainWindow* main_window = nullptr;
    QVBoxLayout* layout = nullptr;

    GameTree* tree_view = nullptr;
    GameGrid* grid_view = nullptr;
    GameCarousel* carousel_view = nullptr;
    GameListModel* item_model = nullptr;
    Settings::GameListMode game_list_mode;
    QWidget* folder_navigation = nullptr;
    QPushButton* folder_back_button = nullptr;
    QLabel* folder_title_label = nullptr;

    ControllerNavigation* controller_navigation = nullptr;

    QVariantAnimation* vertical_scroll = nullptr;
    QVariantAnimation* horizontal_scroll = nullptr;
    int vertical_scroll_target = 0;
    int horizontal_scroll_target = 0;

    const PlayTime::PlayTimeManager& play_time_manager;
    Core::System& system;

    bool m_isTreeMode = true;
    bool m_populating = false;
    int grid_root_dir_index_to_restore = -1;
    /// The first load after start opens the folder of the last played game (see
    /// OnPopulatingCompleted); later loads keep whatever the user opened.
    bool start_folder_opened = false;
    bool suppress_grid_double_click = false;
    bool nup_conversion_running{};
    bool nx_tool_running{};
    u64 reportable_program_id{};
    QAbstractItemView* m_currentView = nullptr;

    LibraryStyles::FilterModel* style_model = nullptr;
    LibraryStyles::LibraryStyleView* style_view = nullptr;
    std::string style_id;
    /// Path of the game selected in a style, kept across style switches and repopulates.
    QString style_selected_path;
    bool restoring_style_selection = false;
};

class GameListPlaceholder : public QWidget {
    Q_OBJECT
public:
    explicit GameListPlaceholder(MainWindow* parent = nullptr);
    ~GameListPlaceholder();

    void RefreshThemeIcon();

signals:
    void AddDirectory();

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void changeEvent(QEvent* event) override;
    void RetranslateUI();

    QVBoxLayout* layout = nullptr;
    QLabel* image = nullptr;
    QLabel* text = nullptr;
};
