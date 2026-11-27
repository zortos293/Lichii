// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QStandardItemModel>

#include "qt_common/game_list/game_list_p.h"
#include "lichii/game/common.h"
#include "lichii/game/styles/library_style_filter.h"

namespace LibraryStyles {

FilterModel::FilterModel(QObject* parent) : QSortFilterProxyModel{parent} {
    // A folder or the favorites row is kept when one of its games is.
    setRecursiveFilteringEnabled(true);
    setDynamicSortFilter(true);
}

void FilterModel::SetFilterText(const QString& text) {
    const QString lowered = text.toLower();
    if (lowered == filter_text) {
        return;
    }
    filter_text = lowered;
    invalidateFilter();
}

bool FilterModel::filterAcceptsRow(int source_row, const QModelIndex& source_parent) const {
    const auto* model = qobject_cast<const QStandardItemModel*>(sourceModel());
    if (model == nullptr) {
        return true;
    }
    const QModelIndex index = model->index(source_row, 0, source_parent);
    const QStandardItem* item = model->itemFromIndex(index);
    if (item == nullptr) {
        return false;
    }
    switch (item->data(GameListItem::TypeRole).value<GameListItemType>()) {
    case GameListItemType::Game:
        return Lichii::FilterMatches(filter_text, item);
    case GameListItemType::AddDir:
        return true;
    case GameListItemType::Favorites:
        // Only through its games (recursive filtering), and never while empty.
        return false;
    default:
        // Folders: shown as long as no search is typed, otherwise through their games.
        return filter_text.isEmpty();
    }
}

} // namespace LibraryStyles
