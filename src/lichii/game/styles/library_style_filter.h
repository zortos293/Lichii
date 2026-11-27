// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QSortFilterProxyModel>
#include <QString>

namespace LibraryStyles {

/**
 * The model the style views get: the game list model with the tree shape kept (favorites row,
 * folder rows with their games, the "Add games folder" row), minus the rows the search box filters
 * out. A folder stays while one of its games matches; favorites only while it has games.
 */
class FilterModel : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit FilterModel(QObject* parent = nullptr);

    void SetFilterText(const QString& text);
    const QString& FilterText() const {
        return filter_text;
    }

protected:
    bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;

private:
    QString filter_text;
};

} // namespace LibraryStyles
