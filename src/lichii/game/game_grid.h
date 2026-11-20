// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QKeyEvent>
#include <QListView>
#include <QString>

class GameCard;
class GameListModel;
class QLabel;
class QResizeEvent;
class QToolButton;
class QWidget;

class GameGrid : public QListView {
    Q_OBJECT

public:
    explicit GameGrid(QWidget* parent = nullptr);

    void SetModel(GameListModel* model);
    void ApplyFilter(const QString& edit_filter_text, GameListModel* model);
    void UpdateIconSize();

    // The root view shows folders; browsing a folder shows only its games.
    void BrowseDirectory(int dir_index);
    void SetBrowsedDirectory(int dir_index);
    int BrowsedDirectory() const {
        return browsed_dir;
    }

    QModelIndex indexAt(const QPoint& point) const override;

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    GameCard* m_gameCard = nullptr;
    QWidget* navigation_bar = nullptr;
    QToolButton* back_button = nullptr;
    QLabel* folder_title = nullptr;
    int browsed_dir = -1;
    QString last_filter_text;

    void UpdateNavigationBarGeometry();
};
