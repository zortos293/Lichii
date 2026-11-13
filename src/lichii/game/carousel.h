// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QListView>
#include <QString>

class GameCard;
class GameListModel;
class QAbstractProxyModel;
class QResizeEvent;

class GameCarousel : public QListView {
    Q_OBJECT

public:
    explicit GameCarousel(QWidget* parent = nullptr);

    void SetModel(GameListModel* model);
    /// Sorts the games like the list does (its sort column and order).
    void SetSort(int column, Qt::SortOrder order);
    void ApplyFilter(const QString& edit_filter_text, GameListModel* model);
    void UpdateIconSize();

    QModelIndex indexAt(const QPoint& point) const override;

private:
    GameCard* m_gameCard = nullptr;
    /// The library model's games in one row, built from the loaded library (no scan).
    QAbstractProxyModel* m_flatModel = nullptr;
};
