// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/model.h"
#include "lichii/game/common.h"
#include "lichii/game/game_card.h"
#include "lichii/game/carousel.h"

GameCarousel::GameCarousel(QWidget* parent) : QListView{parent} {
    m_gameCard = new GameCard(this);
    setItemDelegate(m_gameCard);
    setObjectName(QStringLiteral("gameCarouselView"));

    setViewMode(QListView::IconMode);
    setMovement(QListView::Static);
    setUniformItemSizes(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setMouseTracking(true);

    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);

    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setContextMenuPolicy(Qt::CustomContextMenu);

    setSpacing(12);
    setWordWrap(true);
    setTextElideMode(Qt::ElideRight);
    setFlow(QListView::LeftToRight);
    setWrapping(false);
    setStyleSheet(QStringLiteral(
        "QListView {"
        "  border: none;"
        "  outline: none;"
        "  padding: 8px 10px 12px 10px;"
        "}"));
}

void GameCarousel::SetModel(GameListModel* model) {
    QListView::setModel(model);
    UpdateIconSize();
}

void GameCarousel::ApplyFilter(const QString& edit_filter_text, GameListModel* model) {
    int row_count = model->rowCount();

    for (int i = 0; i < row_count; ++i) {
        QStandardItem* item = model->item(i, 0);
        if (!item)
            continue;

        if (Lichii::FilterMatches(edit_filter_text, item)) {
            setRowHidden(i, false);
        } else {
            setRowHidden(i, true);
        }
    }
}

void GameCarousel::UpdateIconSize() {
    const bool show_game_name = UISettings::values.show_game_name.GetValue();
    const u32 display_icon_size = Lichii::CardIconDisplaySize(show_game_name);

    int heightMargin = 0;
    int widthMargin = 80;

    // TODO(crueter): get rid of this nonsense
    if (show_game_name) {
        widthMargin = 110;
        heightMargin = 155;
    } else {
        widthMargin = 24;
        heightMargin = 24;
    }

    const int min_item_width = display_icon_size + widthMargin;
    const int min_item_height = display_icon_size + heightMargin;
    const int grid_height = std::max(min_item_height, viewport()->height());

    QSize content_size(min_item_width, min_item_height);
    QSize grid_size(min_item_width, grid_height);
    if (gridSize() != grid_size) {
        setUpdatesEnabled(false);

        setGridSize(grid_size);
        m_gameCard->setSize(grid_size, content_size, 0, 0);

        setUpdatesEnabled(true);
    }
}

QModelIndex GameCarousel::indexAt(const QPoint& point) const {
    QModelIndex index = QListView::indexAt(point);
    if (!index.isValid())
        return {};
    if (m_gameCard && !m_gameCard->hitTest(point, index, this, visualRect(index)))
        return {};
    return index;
}
