// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <vector>

#include <QSize>
#include <QWidget>

#include "common/common_types.h"
#include "render/adventure_script.h"

class MainWindow;
class QBoxLayout;
class QComboBox;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QScrollArea;
class QTimer;
class QToolButton;

/// A game's adventure records, read from its newest save. Nothing is written: the numbers change
/// when the game saves. A game is added with a module, notes/<program id>.js in the user folder:
/// it reads the save and returns what to show, so the panel knows no save format.
/// It has two layouts: a tall one with its tabs on a side rail, and a wide strip.
class AdventureNotes : public QWidget {
    Q_OBJECT

public:
    AdventureNotes(MainWindow* parent, u64 program_id);
    ~AdventureNotes() override;

signals:
    void closed();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void Refresh();
    void StartDrag(QPoint position);
    void FillViews();
    void LoadGame();
    void ReadSave();
    void ClearTabs();
    void ClearRows();
    void ShowMessage(const QString& text);
    void CreateModule();
    void CheckFiles();
    void TickModule();
    void ApplyLayout();
    void SwitchLayout();
    void ToggleFold();
    void BuildTabs();
    void MarkTabs();
    void BuildKeys();
    void SelectTab(int index);
    void SetAccent(const QString& color);
    void SaveSettings();
    [[nodiscard]] QWidget* MakeRow(const NotesRow& line, std::vector<QLabel*>& values,
                                   const QString& key);
    void LoadFavorites();
    void AddFavoritesTab();
    void ToggleFavorite(const QString& key);

    std::unique_ptr<NotesScript> script;
    NotesReport report;
    QString slot;
    QString accent;
    u64 program_id = 0;
    std::filesystem::file_time_type module_time{};
    std::filesystem::file_time_type save_time{};
    bool strip = false;
    bool collapsed = false;
    int current_tab = 0;
    QString tab_name;  ///< Kept when the tabs are rebuilt, to come back to the same page.
    QString view_name;
    QString tick_error;  ///< The last tick() error shown, so a failing tick shows it once.
    QStringList favorites;      ///< "tab/view/title" of the starred lines, in the order starred.
    QStringList favorite_rows;  ///< The key of each line of the Favorites view.
    std::array<QSize, 2> sizes;
    QSize open_size;
    QLabel* header = nullptr;
    QLabel* subtitle = nullptr;
    QScrollArea* scroll = nullptr;
    QLabel* grip = nullptr;
    QComboBox* views = nullptr;
    QWidget* body = nullptr;
    QGridLayout* rows = nullptr;
    QWidget* rail = nullptr;
    QBoxLayout* rail_layout = nullptr;
    QWidget* content = nullptr;
    QHBoxLayout* top = nullptr;
    QBoxLayout* column = nullptr;
    QWidget* tab_box = nullptr;
    QBoxLayout* tab_layout = nullptr;
    QWidget* key_box = nullptr;
    QHBoxLayout* key_layout = nullptr;
    QWidget* tools = nullptr;
    QBoxLayout* tools_layout = nullptr;
    QToolButton* collapse = nullptr;
    QToolButton* flip = nullptr;
    QToolButton* reload = nullptr;
    QToolButton* create = nullptr;
    QTimer* timer = nullptr;
    QTimer* ticker = nullptr;
    QPoint drag_start;
    bool dragging = false;
};
