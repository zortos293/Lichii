// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <utility>

#include <QAbstractItemModel>
#include <QApplication>
#include <QEasingCurve>
#include <QHeaderView>
#include <QImage>
#include <QPainter>
#include <QScroller>
#include <QScrollerProperties>
#include <QStyledItemDelegate>
#include <QVariant>
#include <QVariantAnimation>

#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/game_list_p.h"
#include "qt_common/game_list/model.h"
#include "lichii/game/common.h"
#include "lichii/game/game_tree.h"
#include "lichii/util/util.h"

namespace {

// Draws the name column the way Lichii Dark lays it out: a game's title over a muted
// monospace second line with a star for favourites, and a folder's game count after its path.
class LichiiNameDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const auto type = index.data(GameListItem::TypeRole).value<GameListItemType>();
        if (!IsLichiiTheme() || type == GameListItemType::AddDir) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        QStyleOptionViewItem item = option;
        initStyleOption(&item, index);
        const int icon_size = static_cast<int>(UISettings::values.game_icon_size.GetValue());
        const int icon_side =
            std::min({icon_size, item.rect.height() - 2, item.rect.width() - 8});
        const QStringList lines =
            item.text.replace(QChar::LineSeparator, QLatin1Char('\n')).split(QLatin1Char('\n'));
        item.text.clear();

        QPixmap decoration_picture = index.data(Qt::DecorationRole).value<QPixmap>();
        QSize decoration_size;
        if (!decoration_picture.isNull()) {
            if (type == GameListItemType::Game) {
                if (icon_side > 0) {
                    decoration_size = QSize(icon_side, icon_side);
                }
            } else {
                const qreal dpr = decoration_picture.devicePixelRatio() > 0.0
                                      ? decoration_picture.devicePixelRatio()
                                      : 1.0;
                decoration_size =
                    QSize(qRound(decoration_picture.width() / dpr),
                          qRound(decoration_picture.height() / dpr));
                const int max_side = std::max(0, item.rect.height() - 2);
                if (max_side > 0 && (decoration_size.width() > max_side ||
                                     decoration_size.height() > max_side)) {
                    decoration_size.scale(QSize(max_side, max_side), Qt::KeepAspectRatio);
                }
            }

            if (!decoration_size.isEmpty()) {
                // Keep the decoration slot for layout and text, but draw the bitmap ourselves so
                // its physical @2x pixels are used instead of QIcon scaling it through 1x.
                item.decorationSize = decoration_size;
                QPixmap placeholder(decoration_size);
                placeholder.fill(Qt::transparent);
                item.icon = QIcon(placeholder);
            }
        }

        const QWidget* widget = item.widget;
        QStyle* style = widget != nullptr ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);

        if (!decoration_picture.isNull() && !decoration_size.isEmpty()) {
            const QRect slot =
                style->subElementRect(QStyle::SE_ItemViewItemDecoration, &item, widget);
            QSize draw_size = decoration_size;
            if (draw_size.width() > slot.width() || draw_size.height() > slot.height()) {
                draw_size.scale(slot.size(), Qt::KeepAspectRatio);
            }
            if (!draw_size.isEmpty()) {
                const QRect target(slot.left() + (slot.width() - draw_size.width()) / 2,
                                   slot.top() + (slot.height() - draw_size.height()) / 2,
                                   draw_size.width(), draw_size.height());
                painter->drawPixmap(target, decoration_picture);
            }
        }

        const int text_left =
            style->subElementRect(QStyle::SE_ItemViewItemText, &item, widget).left() + 4;
        const QRect rect(text_left, item.rect.top(), item.rect.right() - text_left - 4,
                         item.rect.height());

        const bool selected = item.state & QStyle::State_Selected;
        QFont title_font = item.font;
        title_font.setWeight(QFont::DemiBold);

        painter->save();
        painter->setClipRect(item.rect);
        painter->setFont(title_font);
        const QFontMetrics title_metrics = painter->fontMetrics();
        const QString title =
            title_metrics.elidedText(lines.value(0), Qt::ElideRight, rect.width());
        painter->setPen(selected ? QColor(Qt::white) : QColor(0xec, 0xec, 0xef));

        if (type != GameListItemType::Game) {
            painter->drawText(rect, Qt::AlignVCenter | Qt::AlignLeft, title);
            const int games = index.model()->rowCount(index);
            const int x = rect.left() + title_metrics.horizontalAdvance(title) + 10;
            painter->setFont(item.font);
            painter->setPen(QColor(0x6f, 0x71, 0x78));
            painter->drawText(QRect(x, rect.top(), std::max(0, rect.right() - x), rect.height()),
                              Qt::AlignVCenter | Qt::AlignLeft,
                              games == 1
                                  ? QCoreApplication::translate("GameList", "1 game")
                                  : QCoreApplication::translate("GameList", "%1 games").arg(games));
            painter->restore();
            return;
        }

        QFont id_font = GetMonospaceFont();
        if (item.font.pixelSize() > 0) {
            id_font.setPixelSize(std::max(1, item.font.pixelSize() * 17 / 20));
        } else {
            id_font.setPointSizeF(item.font.pointSizeF() * 0.85);
        }
        const bool two_lines = lines.size() > 1;
        const int middle = rect.top() + rect.height() / 2;
        const int flags = Qt::AlignLeft | Qt::TextDontClip;

        QRect title_rect;
        painter->drawText(
            two_lines ? QRect(rect.left(), rect.top(), rect.width(), middle - rect.top() - 1)
                      : rect,
            flags | (two_lines ? Qt::AlignBottom : Qt::AlignVCenter), title, &title_rect);

        const u64 program_id = index.data(GameListItemPath::ProgramIdRole).toULongLong();
        if (program_id != 0 && UISettings::values.favorited_ids.contains(program_id)) {
            const int star_x = title_rect.right() + 8;
            const int star_y = title_rect.center().y() - 7;
            if (star_x + 14 <= rect.right()) {
                QPixmap star(QStringLiteral(":/lichii/icons/star_accent@2x.png"));
                star.setDevicePixelRatio(2.0);
                painter->drawPixmap(QRect(star_x, star_y, 14, 14), star);
            }
        }

        if (two_lines) {
            painter->setFont(id_font);
            painter->setPen(selected ? QColor(0xc4, 0xc5, 0xca) : QColor(0x6f, 0x71, 0x78));
            painter->drawText(
                QRect(rect.left(), middle + 2, rect.width(), rect.bottom() - middle - 2),
                flags | Qt::AlignTop,
                painter->fontMetrics().elidedText(lines.at(1).trimmed(), Qt::ElideRight,
                                                  rect.width()));
        }
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        if (!IsLichiiTheme()) {
            return size;
        }
        const auto type = index.data(GameListItem::TypeRole).value<GameListItemType>();
        if (type == GameListItemType::Game) {
            const int icon = static_cast<int>(UISettings::values.game_icon_size.GetValue());
            size.setHeight(std::max(size.height(), icon + 16));
        } else {
            size.setHeight(std::max(size.height(), 34));
        }
        return size;
    }
};

QPixmap LoadTreeChevron() {
    QPixmap full(QStringLiteral(":/lichii/icons/tree_chevron@2x.png"));
    if (full.isNull()) {
        return {};
    }
    full.setDevicePixelRatio(2.0);
    const QImage image = full.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int min_x = image.width();
    int min_y = image.height();
    int max_x = -1;
    int max_y = -1;
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) == 0) {
                continue;
            }
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
        }
    }
    if (max_x < min_x || max_y < min_y) {
        return {};
    }
    QPixmap cropped = QPixmap::fromImage(
        image.copy(QRect(min_x, min_y, max_x - min_x + 1, max_y - min_y + 1)));
    cropped.setDevicePixelRatio(2.0);
    return cropped;
}

} // namespace

GameTree::GameTree(QWidget* parent) : QTreeView{parent} {
    arrow_pixmap = LoadTreeChevron();
    setItemDelegateForColumn(GameListModel::COLUMN_NAME, new LichiiNameDelegate(this));
    setAlternatingRowColors(true);
    setSelectionMode(QHeaderView::SingleSelection);
    setSelectionBehavior(QHeaderView::SelectRows);
    setVerticalScrollMode(QHeaderView::ScrollPerPixel);
    setHorizontalScrollMode(QHeaderView::ScrollPerPixel);
    setSortingEnabled(true);
    setEditTriggers(QHeaderView::NoEditTriggers);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    setStyleSheet(QStringLiteral("QTreeView{ border: none; }"));

    connect(this, &QTreeView::expanded, this, &GameTree::OnItemExpanded);
    connect(this, &QTreeView::collapsed, this, &GameTree::OnItemExpanded);
}

void GameTree::SetModel(GameListModel* model) {
    ClearArrowAnimations();
    QTreeView::setModel(model);
    if (model != nullptr) {
        connect(model, &QAbstractItemModel::modelReset, this, &GameTree::ClearArrowAnimations,
                Qt::UniqueConnection);
    }
    const int icon_size = static_cast<int>(UISettings::values.game_icon_size.GetValue());
    setIconSize(QSize(icon_size, icon_size));
    LoadInterfaceLayout();
    UpdateColumnVisibility(model);
}

void GameTree::drawBranches(QPainter* painter, const QRect& rect,
                            const QModelIndex& index) const {
    // Keep the style's row background, then draw the rotated @2x chevron.
    QTreeView::drawBranches(painter, rect, index);
    if (!IsLichiiTheme() || !index.isValid() || model() == nullptr ||
        !model()->hasChildren(index)) {
        return;
    }

    if (arrow_pixmap.isNull()) {
        return;
    }

    const bool expanded = isExpanded(index);
    int depth = 0;
    for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent()) {
        ++depth;
    }
    const int indent = indentation();
    const QRect branch(rect.left() + depth * indent, rect.top(), indent, rect.height());

    qreal angle = expanded ? 90.0 : 0.0;
    if (!arrow_angles.isEmpty()) {
        const QPersistentModelIndex key(index);
        if (const auto it = arrow_angles.constFind(key); it != arrow_angles.constEnd()) {
            angle = it.value();
        }
    }

    // Center the visible (alpha-cropped) chevron, not its padded canvas, and rotate around
    // that exact center.
    const QSizeF arrow_size = arrow_pixmap.deviceIndependentSize();
    const QPointF center = QRectF(branch).center();
    const QRectF target(center.x() - arrow_size.width() / 2.0,
                        center.y() - arrow_size.height() / 2.0,
                        arrow_size.width(), arrow_size.height());

    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    const QPointF pivot = target.center();
    painter->translate(pivot);
    painter->rotate(angle);
    painter->translate(-pivot.x(), -pivot.y());
    painter->drawPixmap(target.topLeft(), arrow_pixmap);
    painter->restore();
}

void GameTree::AnimateArrow(const QModelIndex& index, bool expanded) {
    if (!IsLichiiTheme() || !index.isValid() || model() == nullptr ||
        !model()->hasChildren(index)) {
        return;
    }

    const QPersistentModelIndex key(index);
    const qreal end = expanded ? 90.0 : 0.0;
    const qreal start = arrow_angles.value(key, expanded ? 0.0 : 90.0);

    if (QVariantAnimation* old = arrow_animations.take(key)) {
        old->stop();
        old->deleteLater();
    }

    // Keep the first paint at the current angle; otherwise a new expansion would flash at 90°.
    arrow_angles[key] = start;

    auto* animation = new QVariantAnimation(this);
    animation->setStartValue(start);
    animation->setEndValue(end);
    animation->setDuration(160);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation, &QVariantAnimation::valueChanged, this,
            [this, key](const QVariant& value) {
                arrow_angles[key] = value.toReal();
                const QModelIndex current = static_cast<QModelIndex>(key);
                if (!current.isValid()) {
                    return;
                }
                const QRect row = visualRect(current);
                if (row.isValid()) {
                    viewport()->update(QRect(0, row.top(), viewport()->width(), row.height()));
                }
            });
    connect(animation, &QVariantAnimation::finished, this, [this, key, animation] {
        // A newer animation for this row may have replaced this one.
        if (arrow_animations.value(key) == animation) {
            arrow_animations.remove(key);
            arrow_angles.remove(key);
            const QModelIndex current = static_cast<QModelIndex>(key);
            if (current.isValid()) {
                const QRect row = visualRect(current);
                if (row.isValid()) {
                    viewport()->update(
                        QRect(0, row.top(), viewport()->width(), row.height()));
                }
            }
        }
        animation->deleteLater();
    });
    arrow_animations.insert(key, animation);
    animation->start();
}

void GameTree::ClearArrowAnimations() {
    for (auto* animation : std::as_const(arrow_animations)) {
        animation->stop();
        animation->deleteLater();
    }
    arrow_animations.clear();
    arrow_angles.clear();
}

void GameTree::OnItemExpanded(const QModelIndex& item) {
    const bool is_expanded = isExpanded(item);
    AnimateArrow(item, is_expanded);

    const auto type = item.data(GameListItem::TypeRole).value<GameListItemType>();
    const bool is_dir = type == GameListItemType::CustomDir || type == GameListItemType::SdmcDir ||
                        type == GameListItemType::UserNandDir ||
                        type == GameListItemType::SysNandDir;
    const bool is_fave = type == GameListItemType::Favorites;
    if (!is_dir && !is_fave) {
        return;
    }
    if (is_fave) {
        UISettings::values.favorites_expanded = is_expanded;
        return;
    }
    const int item_dir_index = item.data(GameListDir::GameDirRole).toInt();
    UISettings::values.game_dirs[item_dir_index].expanded = is_expanded;
}

void GameTree::SaveInterfaceLayout() {
    UISettings::values.gamelist_header_state = header()->saveState();
}

void GameTree::LoadInterfaceLayout() {
    auto* hdr = header();

    if (hdr->restoreState(UISettings::values.gamelist_header_state))
        return;

    hdr->resizeSection(GameListModel::COLUMN_NAME, 840);
}

void GameTree::UpdateColumnVisibility(GameListModel* model) {
    Q_UNUSED(model)
    setColumnHidden(GameListModel::COLUMN_ADD_ONS, !UISettings::values.show_add_ons);
    setColumnHidden(GameListModel::COLUMN_FILE_TYPE, !UISettings::values.show_types);
    setColumnHidden(GameListModel::COLUMN_SIZE, !UISettings::values.show_size);
    setColumnHidden(GameListModel::COLUMN_PLAY_TIME, !UISettings::values.show_play_time);
}

QString GameTree::GetLastFilterResultItem() const {
    QString file_path;

    auto* model = qobject_cast<GameListModel*>(QTreeView::model());
    if (!model)
        return {};

    for (int i = 1; i < model->rowCount() - 1; ++i) {
        const QStandardItem* folder = model->item(i, 0);
        const QModelIndex folder_index = folder->index();
        const int children_count = folder->rowCount();

        for (int j = 0; j < children_count; ++j) {
            if (isRowHidden(j, folder_index)) {
                continue;
            }

            const QStandardItem* child = folder->child(j, 0);
            file_path = child->data(GameListItemPath::FullPathRole).toString();
        }
    }

    return file_path;
}

int GameTree::FilterClosedResultCount(GameListModel* model) {
    int children_total = 0;

    auto hide_favorites_row = UISettings::values.favorited_ids.size() == 0;
    setRowHidden(0, model->invisibleRootItem()->index(), hide_favorites_row);

    for (int i = 1; i < model->rowCount() - 1; ++i) {
        auto* folder = model->item(i, 0);
        const QModelIndex folder_index = folder->index();
        const int children_count = folder->rowCount();
        for (int j = 0; j < children_count; ++j) {
            ++children_total;
            setRowHidden(j, folder_index, false);
        }
    }

    return children_total;
}

void GameTree::ApplyFilter(const QString& edit_filter_text, GameListModel* model) {
    int children_total = 0;
    int result_count = 0;

    if (edit_filter_text.isEmpty()) {
        children_total = FilterClosedResultCount(model);
        emit FilterResultReady(children_total, children_total);
        return;
    }

    setRowHidden(0, model->invisibleRootItem()->index(), true);

    for (int i = 1; i < model->rowCount() - 1; ++i) {
        auto* folder = model->item(i, 0);
        const QModelIndex folder_index = folder->index();
        const int children_count = folder->rowCount();

        for (int j = 0; j < children_count; ++j) {
            ++children_total;

            const QStandardItem* child = folder->child(j, 0);

            if (Lichii::FilterMatches(edit_filter_text, child)) {
                setRowHidden(j, folder_index, false);
                ++result_count;
            } else {
                setRowHidden(j, folder_index, true);
            }
        }
    }

    emit FilterResultReady(result_count, children_total);
}
