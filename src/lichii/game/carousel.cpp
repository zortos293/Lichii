// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <memory>
#include <utility>

#include <QAbstractProxyModel>
#include <QHash>
#include <QPersistentModelIndex>
#include <QTimer>
#include <QVector>

#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/game_list_p.h"
#include "qt_common/game_list/model.h"
#include "lichii/game/common.h"
#include "lichii/game/game_card.h"
#include "lichii/game/carousel.h"

namespace {

/// The Carousel's row: the games of every folder one after the other, then the "Add games
/// folder" entry, read from the library model as loaded. Switching to the Carousel needs no new
/// scan. The row list is rebuilt from memory, once per event loop pass, only when the library
/// model's rows change.
class FlatGamesModel final : public QAbstractProxyModel {
public:
    explicit FlatGamesModel(QObject* parent) : QAbstractProxyModel{parent} {
        rebuild_timer.setSingleShot(true);
        rebuild_timer.setInterval(0);
        QObject::connect(&rebuild_timer, &QTimer::timeout, this, [this] { Rebuild(); });
    }

    void setSourceModel(QAbstractItemModel* model) override {
        if (sourceModel() != nullptr) {
            sourceModel()->disconnect(this);
        }
        beginResetModel();
        QAbstractProxyModel::setSourceModel(model);
        source = qobject_cast<QStandardItemModel*>(model);
        Collect();
        endResetModel();
        if (source == nullptr) {
            return;
        }
        // Entries about to go away are dropped before their items are deleted.
        connect(source, &QAbstractItemModel::rowsAboutToBeRemoved, this,
                [this](const QModelIndex& parent, int first, int last) {
                    if (Affects(parent, first, last)) {
                        Clear();
                        rebuild_timer.start();
                    }
                });
        connect(source, &QAbstractItemModel::rowsInserted, this,
                [this](const QModelIndex& parent, int, int) {
                    if (!IsFavorites(parent)) {
                        rebuild_timer.start();
                    }
                });
        connect(source, &QAbstractItemModel::modelAboutToBeReset, this, [this] { Clear(); });
        connect(source, &QAbstractItemModel::modelReset, this, [this] { rebuild_timer.start(); });
        // Sorting: the items stay, their order changes.
        connect(source, &QAbstractItemModel::layoutChanged, this,
                [this] { rebuild_timer.start(); });
        connect(source, &QAbstractItemModel::rowsMoved, this, [this] { rebuild_timer.start(); });
        connect(source, &QAbstractItemModel::dataChanged, this,
                [this](const QModelIndex& top_left, const QModelIndex& bottom_right) {
                    for (int row = top_left.row(); row <= bottom_right.row(); ++row) {
                        const QStandardItem* item =
                            source->itemFromIndex(top_left.sibling(row, 0));
                        const auto found = row_of.constFind(item);
                        if (found != row_of.cend()) {
                            const QModelIndex changed = index(found.value(), 0);
                            emit dataChanged(changed, changed);
                        }
                    }
                });
    }

    /// The games are sorted across all folders, like the list's sort column (-1: as loaded).
    void SetSort(int column, Qt::SortOrder order) {
        if (column == sort_column && order == sort_order) {
            return;
        }
        sort_column = column;
        sort_order = order;
        if (source != nullptr && !entries.isEmpty()) {
            Rebuild();
        }
    }

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override {
        if (parent.isValid() || column != 0 || row < 0 || row >= entries.size()) {
            return {};
        }
        return createIndex(row, column);
    }
    QModelIndex parent(const QModelIndex&) const override {
        return {};
    }
    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(entries.size());
    }
    int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : 1;
    }
    bool hasChildren(const QModelIndex& parent = {}) const override {
        return !parent.isValid() && !entries.isEmpty();
    }
    QModelIndex mapToSource(const QModelIndex& proxy) const override {
        if (!proxy.isValid() || proxy.row() >= entries.size()) {
            return {};
        }
        return entries[proxy.row()]->index();
    }
    QModelIndex mapFromSource(const QModelIndex& source_index) const override {
        if (source == nullptr || !source_index.isValid()) {
            return {};
        }
        const auto found =
            row_of.constFind(source->itemFromIndex(source_index.siblingAtColumn(0)));
        return found == row_of.cend() ? QModelIndex{} : index(found.value(), 0);
    }

private:
    static bool IsFolder(const QStandardItem* item) {
        switch (item->data(GameListItem::TypeRole).value<GameListItemType>()) {
        case GameListItemType::CustomDir:
        case GameListItemType::SdmcDir:
        case GameListItemType::UserNandDir:
        case GameListItemType::SysNandDir:
            return true;
        default:
            return false;
        }
    }

    static bool IsFavorites(const QModelIndex& parent) {
        return parent.isValid() && parent.data(GameListItem::TypeRole).value<GameListItemType>() ==
                                       GameListItemType::Favorites;
    }

    /// Whether removing these rows takes away one of the entries (folder rows without games,
    /// like an empty system folder, take none).
    bool Affects(const QModelIndex& parent, int first, int last) const {
        if (IsFavorites(parent)) {
            return false;
        }
        if (parent.isValid()) {
            return true;
        }
        for (int row = first; row <= last; ++row) {
            const QStandardItem* item = source->item(row, 0);
            if (item != nullptr && (item->hasChildren() || !IsFolder(item))) {
                return true;
            }
        }
        return false;
    }

    void Collect() {
        entries.clear();
        row_of.clear();
        if (source == nullptr) {
            return;
        }
        QStandardItem* add_dir = nullptr;
        // Each game with the item of the sort column, which does the comparing.
        std::vector<std::pair<QStandardItem*, const QStandardItem*>> games;
        for (int row = 0; row < source->rowCount(); ++row) {
            QStandardItem* item = source->item(row, 0);
            if (item == nullptr) {
                continue;
            }
            if (IsFolder(item)) {
                for (int child = 0; child < item->rowCount(); ++child) {
                    if (QStandardItem* game = item->child(child, 0)) {
                        const QStandardItem* key =
                            sort_column > 0 ? item->child(child, sort_column) : game;
                        games.emplace_back(game, key != nullptr ? key : game);
                    }
                }
            } else if (item->data(GameListItem::TypeRole).value<GameListItemType>() ==
                       GameListItemType::AddDir) {
                add_dir = item;
            }
        }
        if (sort_column >= 0) {
            // As QStandardItemModel::sort does it.
            std::stable_sort(games.begin(), games.end(), [this](const auto& a, const auto& b) {
                return sort_order == Qt::AscendingOrder ? *a.second < *b.second
                                                        : *b.second < *a.second;
            });
        }
        entries.reserve(static_cast<qsizetype>(games.size()) + 1);
        for (const auto& game : games) {
            row_of.insert(game.first, static_cast<int>(entries.size()));
            entries.append(game.first);
        }
        if (add_dir != nullptr) {
            row_of.insert(add_dir, static_cast<int>(entries.size()));
            entries.append(add_dir);
        }
    }

    void Clear() {
        rebuild_timer.stop();
        if (entries.isEmpty()) {
            return;
        }
        beginResetModel();
        entries.clear();
        row_of.clear();
        endResetModel();
    }

    void Rebuild() {
        beginResetModel();
        Collect();
        endResetModel();
    }

    QStandardItemModel* source = nullptr;
    QVector<QStandardItem*> entries;
    QHash<const QStandardItem*, int> row_of;
    QTimer rebuild_timer;
    int sort_column = -1;
    Qt::SortOrder sort_order = Qt::AscendingOrder;
};

} // namespace

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
    if (m_flatModel == nullptr) {
        m_flatModel = new FlatGamesModel(this);
        // A rebuild resets the row: keep the selected game.
        auto selected = std::make_shared<QPersistentModelIndex>();
        connect(m_flatModel, &QAbstractItemModel::modelAboutToBeReset, this, [this, selected] {
            *selected = m_flatModel->mapToSource(currentIndex());
        });
        connect(m_flatModel, &QAbstractItemModel::modelReset, this, [this, selected] {
            if (!selected->isValid()) {
                return;
            }
            const QModelIndex restored = m_flatModel->mapFromSource(*selected);
            *selected = QPersistentModelIndex{};
            if (restored.isValid()) {
                setCurrentIndex(restored);
                scrollTo(restored);
            }
        });
    }
    m_flatModel->setSourceModel(model);
    QListView::setModel(m_flatModel);
    UpdateIconSize();
}

void GameCarousel::SetSort(int column, Qt::SortOrder order) {
    if (m_flatModel != nullptr) {
        static_cast<FlatGamesModel*>(m_flatModel)->SetSort(column, order);
    }
}

void GameCarousel::ApplyFilter(const QString& edit_filter_text, GameListModel* model) {
    const int row_count = m_flatModel != nullptr ? m_flatModel->rowCount() : 0;

    for (int i = 0; i < row_count; ++i) {
        QStandardItem* item =
            model->itemFromIndex(m_flatModel->mapToSource(m_flatModel->index(i, 0)));
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

    // Same card shape as the grid: square artwork with the title band under it.
    const int widthMargin = Lichii::CardWidthMargin;
    const int heightMargin =
        GameCard::HeightMargin(static_cast<int>(Lichii::CardWidthBase()), show_game_name,
                               Lichii::ShowAnyGameInfo());

    const int min_item_width = static_cast<int>(Lichii::CardWidthBase()) + widthMargin;
    const int min_item_height = display_icon_size + heightMargin;
    // GameCard::getCardRect keeps the card cardMargin inside the cell top and bottom; without
    // that room the card got shorter than its content and squashed the art.
    const int grid_height =
        std::max(min_item_height + GameCard::cardMargin, viewport()->height());

    QSize content_size(min_item_width, min_item_height);
    QSize grid_size(min_item_width, grid_height);
    // The cell is as tall as the view, so it rarely changes; the card inside it does whenever
    // names or Game Info parts toggle. Always hand the card its new size, or its art stretches.
    if (gridSize() != grid_size) {
        setUpdatesEnabled(false);
        setGridSize(grid_size);
        setUpdatesEnabled(true);
    }
    m_gameCard->setSize(grid_size, content_size, 0, 0);
    viewport()->update();
}

QModelIndex GameCarousel::indexAt(const QPoint& point) const {
    QModelIndex index = QListView::indexAt(point);
    if (!index.isValid())
        return {};
    if (m_gameCard && !m_gameCard->hitTest(point, index, this, visualRect(index)))
        return {};
    return index;
}
