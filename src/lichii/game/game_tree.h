// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QTreeView>

class QPainter;
class QVariantAnimation;
class GameListModel;

class GameTree : public QTreeView {
    Q_OBJECT

public:
    explicit GameTree(QWidget* parent = nullptr);

    void SetModel(GameListModel* model);

    QString GetLastFilterResultItem() const;
    int FilterClosedResultCount(GameListModel* model);
    void ApplyFilter(const QString& edit_filter_text, GameListModel* model);

    void SaveInterfaceLayout();
    void LoadInterfaceLayout();

    void UpdateColumnVisibility(GameListModel* model);

protected:
    void drawBranches(QPainter* painter, const QRect& rect,
                      const QModelIndex& index) const override;

private:
    void AnimateArrow(const QModelIndex& index, bool expanded);
    void ClearArrowAnimations();

    QHash<QPersistentModelIndex, qreal> arrow_angles;
    QHash<QPersistentModelIndex, QVariantAnimation*> arrow_animations;
    QPixmap arrow_pixmap;

signals:
    void ItemExpandedChanged(const QModelIndex& item);
    void FilterResultReady(int visible, int total);

private slots:
    void OnItemExpanded(const QModelIndex& item);
};
