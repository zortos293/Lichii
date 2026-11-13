// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Layout from a library concept by inxomnyaa.

#include <algorithm>

#include <QAbstractItemModel>
#include <QContextMenuEvent>
#include <QFontInfo>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <QScrollBar>
#include <QStringList>
#include <QTimer>
#include <QWheelEvent>

#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/game_list_p.h"
#include "lichii/game/styles/dense_grid_style.h"
#include "lichii/game/styles/style_helpers.h"

namespace LibraryStyles {

namespace {

constexpr int Margin = 12;
constexpr int Gap = 6;
constexpr int BarHeight = 44;
constexpr int Radius = 4;

bool IsFolder(GameListItemType type) {
    return type == GameListItemType::CustomDir || type == GameListItemType::SdmcDir ||
           type == GameListItemType::UserNandDir || type == GameListItemType::SysNandDir;
}

LibraryStyleView* Make(QWidget* parent) {
    return new DenseGridStyle(parent);
}

/// The rounded cover of `index` at `size` (device pixels), cached: building it scales the art,
/// too much to repeat for every tile of every frame.
QPixmap CachedCover(QCache<QString, QPixmap>& cache, const QModelIndex& index, const QSize& size,
                    const QPalette& palette, int radius) {
    const qint64 art = index.data(GameListItemPath::CardIconRole).value<QPixmap>().cacheKey();
    const QString key = QStringLiteral("dense_cover:%1:%2:%3x%4:%5:%6")
                            .arg(index.data(GameListItemPath::FullPathRole).toString())
                            .arg(art)
                            .arg(size.width())
                            .arg(size.height())
                            .arg(radius)
                            .arg(palette.window().color().rgb());
    if (const QPixmap* cached = cache.object(key)) {
        return *cached;
    }
    QPixmap pixmap(size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(pixmap.rect()), radius, radius);
    painter.setClipPath(clip);
    painter.drawPixmap(0, 0, LibraryStyleContext::Cover(index, size, palette));
    painter.end();
    cache.insert(key, new QPixmap(pixmap),
                 std::max(1, static_cast<int>(qint64(size.width()) * size.height() * 4 / 1024)));
    return pixmap;
}

QString TitleOf(const QModelIndex& index) {
    const QString title = index.data(GameListItemPath::TitleRole).toString();
    return title.isEmpty() ? LibraryStyleContext::Title(index) : title;
}

} // namespace

[[maybe_unused]] static const bool registered = LibraryStyles::Register(
    {"dense_grid", QT_TRANSLATE_NOOP("LibraryStyles", "Dense grid"), &Make});

DenseGridStyle::DenseGridStyle(QWidget* parent) : LibraryStyleView{parent} {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    scrollbar = new QScrollBar(Qt::Vertical, this);
    scrollbar->setSingleStep(24);
    connect(scrollbar, &QScrollBar::valueChanged, this, [this] { update(); });
}

void DenseGridStyle::SetModel(QAbstractItemModel* new_model) {
    if (model) {
        disconnect(model, nullptr, this, nullptr);
    }
    model = new_model;
    if (model) {
        // Population adds rows one by one: rebuild once per event loop pass, not once per row.
        const auto schedule = [this] {
            if (rebuild_pending) {
                return;
            }
            rebuild_pending = true;
            QTimer::singleShot(0, this, [this] {
                if (rebuild_pending) {
                    Rebuild();
                }
            });
        };
        connect(model, &QAbstractItemModel::modelReset, this, schedule);
        connect(model, &QAbstractItemModel::layoutChanged, this, schedule);
        connect(model, &QAbstractItemModel::rowsInserted, this, schedule);
        connect(model, &QAbstractItemModel::rowsRemoved, this, schedule);
        connect(model, &QAbstractItemModel::rowsMoved, this, schedule);
        connect(model, &QAbstractItemModel::dataChanged, this, [this] { update(); });
    }
    Rebuild();
}

QModelIndex DenseGridStyle::CurrentIndex() const {
    if (current < 0 || current >= entries.size()) {
        return {};
    }
    return entries[current];
}

void DenseGridStyle::SetCurrentIndex(const QModelIndex& index) {
    // A rebuild may still be queued after the model changed: run it first.
    if (rebuild_pending) {
        Rebuild();
    }
    const QPersistentModelIndex wanted{index.siblingAtColumn(0)};
    for (int i = 0; i < entries.size(); ++i) {
        if (entries[i] == wanted) {
            Select(i);
            return;
        }
    }
}

void DenseGridStyle::SetShowNames(bool show) {
    show_names = show;
    Relayout();
}

void DenseGridStyle::SetShowInfo(bool show) {
    show_info = show;
    Relayout(); // names under tiles: the info line under the selected tile needs its room
}

QString DenseGridStyle::CycleOption() {
    names_on_tiles = !names_on_tiles;
    Relayout();
    return names_on_tiles ? tr("Layout: names on tiles") : tr("Layout: names under tiles");
}

void DenseGridStyle::Rebuild() {
    rebuild_pending = false;
    const QPersistentModelIndex previous = CurrentIndex();
    entries.clear();
    // Games are shown flat: top level games plus the games inside each folder row. Favorites and
    // the "Add games folder" row are not tiles, like the other styles.
    if (model) {
        const auto add_games = [this](const QModelIndex& parent) {
            for (int row = 0; row < model->rowCount(parent); ++row) {
                const QModelIndex index = model->index(row, 0, parent);
                if (index.data(GameListItem::TypeRole).value<GameListItemType>() ==
                    GameListItemType::Game) {
                    entries.append(index);
                }
            }
        };
        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex index = model->index(row, 0);
            const auto type = index.data(GameListItem::TypeRole).value<GameListItemType>();
            if (IsFolder(type)) {
                add_games(index);
            } else if (type == GameListItemType::Game) {
                entries.append(index);
            }
        }
    }

    current = -1;
    for (int i = 0; previous.isValid() && i < entries.size(); ++i) {
        if (entries[i] == previous) {
            current = i;
            break;
        }
    }
    if (current < 0 && !entries.isEmpty()) {
        current = 0;
    }
    Relayout();
    if (QPersistentModelIndex(CurrentIndex()) != previous) {
        emit CurrentChanged(CurrentIndex());
    }
}

int DenseGridStyle::TileSize() const {
    // Same art size as every other style; this one is dense through its tight gaps.
    return LibraryStyleContext::ArtSize();
}

QFont DenseGridStyle::TileFont() const {
    // The Tinfoil concept: 12 px names on a 120 px tile, growing with the tile.
    QFont f = font();
    f.setPixelSize(std::clamp(qRound(12.0 * TileSize() / 120.0), 10, 18));
    return f;
}

QFont DenseGridStyle::InfoFont() const {
    // 11 px info under 12 px names in the concept.
    QFont f = font();
    f.setPixelSize(std::max(9, qRound(TileFont().pixelSize() * 11.0 / 12.0)));
    return f;
}

int DenseGridStyle::TextTop() const {
    // Visually even gaps: the space a line box leaves above the capitals (first line) and
    // below the baseline (last line) is not the same, so shift the text by the difference.
    const QFontMetrics first(show_names ? TileFont() : InfoFont());
    const QFontMetrics last(show_info ? InfoFont() : TileFont());
    const int above = first.ascent() - first.capHeight();
    const int below = last.lineSpacing() - last.ascent();
    return std::max(0, Gap + below - above);
}

bool DenseGridStyle::TextBelow() const {
    return !names_on_tiles && (show_names || show_info);
}

int DenseGridStyle::CellHeight() const {
    const int tile = TileSize();
    if (!TextBelow()) {
        return tile; // the caption sits on the art
    }
    // The text, then the row gap: as much space under the text as between the art and it.
    int height = tile + TextTop();
    if (show_names) {
        height += QFontMetrics(TileFont()).lineSpacing() * LibraryStyleContext::TitleLines(3);
    }
    if (show_info) {
        height += (show_names ? 2 : 0) + QFontMetrics(InfoFont()).lineSpacing();
    }
    return height;
}

QRect DenseGridStyle::GridArea() const {
    return QRect(0, 0, width() - scrollbar->sizeHint().width(), std::max(0, height() - BarHeight));
}

void DenseGridStyle::Relayout() {
    const QRect area = GridArea();
    const int tile = TileSize();
    columns = std::max(1, (area.width() - Margin * 2 + Gap) / (tile + Gap));
    const int rows = (static_cast<int>(entries.size()) + columns - 1) / columns;
    const int content = Margin * 2 + rows * CellHeight() + std::max(0, rows - 1) * Gap;

    scrollbar->setGeometry(area.right() + 1, 0, scrollbar->sizeHint().width(), area.height());
    scrollbar->setPageStep(area.height());
    scrollbar->setRange(0, std::max(0, content - area.height()));
    scrollbar->setVisible(scrollbar->maximum() > 0);
    EnsureVisible(current);
    update();
}

QRect DenseGridStyle::TileRect(int position) const {
    const int tile = TileSize();
    const int column = position % columns;
    const int row = position / columns;
    return QRect(Margin + column * (tile + Gap),
                 Margin + row * (CellHeight() + Gap) - scrollbar->value(), tile, tile);
}

int DenseGridStyle::PositionAt(const QPoint& point) const {
    if (!GridArea().contains(point)) {
        return -1;
    }
    const int tile = TileSize();
    const int x = point.x() - Margin;
    const int y = point.y() + scrollbar->value() - Margin;
    if (x < 0 || y < 0) {
        return -1;
    }
    const int column = x / (tile + Gap);
    const int row = y / (CellHeight() + Gap);
    if (column >= columns || x % (tile + Gap) >= tile || y % (CellHeight() + Gap) >= CellHeight()) {
        return -1;
    }
    const int position = row * columns + column;
    return position < entries.size() ? position : -1;
}

void DenseGridStyle::Select(int position) {
    if (entries.isEmpty()) {
        return;
    }
    position = std::clamp(position, 0, static_cast<int>(entries.size()) - 1);
    if (position == current) {
        return;
    }
    current = position;
    EnsureVisible(current);
    update();
    emit CurrentChanged(CurrentIndex());
}

void DenseGridStyle::EnsureVisible(int position) {
    if (position < 0 || position >= entries.size()) {
        return;
    }
    const QRect area = GridArea();
    QRect cell = TileRect(position);
    cell.setHeight(CellHeight());
    if (cell.top() < Margin) {
        scrollbar->setValue(scrollbar->value() + cell.top() - Margin);
    } else if (cell.bottom() > area.bottom() - Margin) {
        scrollbar->setValue(scrollbar->value() + cell.bottom() - area.bottom() + Margin);
    }
}

void DenseGridStyle::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QPalette& pal = palette();
    const QRect area = GridArea();

    painter.save();
    painter.setClipRect(area);
    const QFont tile_font = TileFont();
    const QFontMetrics tile_metrics(tile_font);
    for (int i = 0; i < entries.size(); ++i) {
        const QRect tile = TileRect(i);
        if (tile.bottom() + CellHeight() < 0 || tile.top() > area.bottom()) {
            continue;
        }
        const QModelIndex index = entries[i];
        const qreal ratio = devicePixelRatioF();
        painter.drawPixmap(tile, CachedCover(cover_cache, index, tile.size() * ratio, pal,
                                             static_cast<int>(Radius * ratio)));
        const bool selected = i == current;
        if (names_on_tiles) {
            // Concept overlay: the selected tile always shows it, Names / Info on every tile.
            const bool name = show_names || selected;
            const bool info = show_info || selected;
            if (name || info) {
                LibraryStyleContext::PaintCoverCaption(&painter, tile, index, tile_font,
                                                       InfoFont(), name, info,
                                                       selected ? this : nullptr);
            }
        } else if (TextBelow()) {
            // Names under the tiles: the name (accent when selected), then its info line.
            int text_top = tile.bottom() + 1 + TextTop();
            // A little inset so names of neighbouring tiles never run together.
            const QRect column = tile.adjusted(3, 0, -3, 0);
            if (show_names) {
                const int lines = LibraryStyleContext::TitleLines(3);
                const QRect name(column.left(), text_top, column.width(),
                                 tile_metrics.lineSpacing() * lines);
                LibraryStyleContext::PaintTitle(
                    &painter, name, TitleOf(index),
                    selected ? LibraryStyleContext::Accent(pal) : pal.text().color(), tile_font,
                    Qt::AlignHCenter | Qt::AlignTop, lines, selected ? this : nullptr);
                const int used = std::max<int>(
                    1, LibraryStyleContext::NameLines(TitleOf(index), tile_font, column.width(),
                                                      lines)
                           .size());
                text_top += tile_metrics.lineSpacing() * used + 2;
            }
            if (show_info && index.data(GameListItem::TypeRole).value<GameListItemType>() ==
                                 GameListItemType::Game) {
                const QFont info_font = InfoFont();
                const GameInfo info = LibraryStyleContext::Info(index);
                // Packed the same way on every tile (S3).
                const QStringList info_lines = LibraryStyleContext::InfoLines(
                    info, LibraryStyleContext::LayoutInfo(info_font, column.width(), 1),
                    info_font);
                LibraryStyleContext::PaintInfoLines(
                    &painter,
                    QRect(column.left(), text_top, column.width(),
                          QFontMetrics(info_font).lineSpacing()),
                    info_lines, LibraryStyleContext::MutedText(pal), info_font,
                    Qt::AlignHCenter | Qt::AlignTop);
            }
        }
        if (i == current) {
            LibraryStyleContext::PaintSelection(&painter, tile, pal, Radius, 3);
        }
    }
    painter.restore();
    const QFontMetrics metrics(font());

    // Status bar: selected title (and its info line) on the left, the controls on the right.
    const QRect bar(0, area.bottom() + 1, width(), BarHeight);
    QColor line = pal.text().color();
    line.setAlpha(40);
    painter.setPen(line);
    painter.drawLine(bar.topLeft(), bar.topRight());

    const QStringList hints{tr("A Launch"), tr("B Names"), tr("Y Info"), tr("X Layout"),
                            tr("L R Style")};
    const QString hint_text = hints.join(QStringLiteral("    "));
    const int hint_width = metrics.horizontalAdvance(hint_text);
    painter.setPen(LibraryStyleContext::MutedText(pal));
    painter.drawText(bar.adjusted(Margin, 0, -Margin, 0), Qt::AlignRight | Qt::AlignVCenter,
                     hint_text);

    const QModelIndex selected = CurrentIndex();
    if (selected.isValid()) {
        // The bar follows the same toggles as the tiles: no names, no title down here either.
        // One line: the title, then the info parts that fit after it (S3), never cut inside.
        const GameInfo info = LibraryStyleContext::Info(selected);
        QFont bold = font();
        bold.setBold(true);
        const QRect text_rect(bar.left() + Margin, bar.top(),
                              std::max(0, bar.width() - Margin * 3 - hint_width), bar.height());
        const int gap = metrics.horizontalAdvance(QStringLiteral("    "));
        const int title_w = show_names ? QFontMetrics(bold).horizontalAdvance(info.title) : 0;
        // The info gets what the title leaves, the title keeps at least half of the line.
        const int title_room = show_names ? std::min(title_w, text_rect.width() / 2) + gap : 0;
        const int info_room = std::max(0, text_rect.width() - title_room);
        const QStringList info_lines =
            show_info ? LibraryStyleContext::InfoLines(
                            info, LibraryStyleContext::LayoutInfo(bold, info_room, 1, &info), bold)
                      : QStringList{};
        const int info_w =
            info_lines.isEmpty() ? 0 : QFontMetrics(bold).horizontalAdvance(info_lines.first()) + 2;
        const int name_w = std::max(0, text_rect.width() - (info_w > 0 ? info_w + gap : 0));
        if (show_names) {
            LibraryStyleContext::PaintTitle(
                &painter, QRect(text_rect.left(), text_rect.top(), name_w, text_rect.height()),
                info.title, pal.text().color(), bold, Qt::AlignLeft | Qt::AlignVCenter, 1, this);
        }
        if (!info_lines.isEmpty()) {
            const int left = show_names ? text_rect.left() + std::min(title_w + 2, name_w) + gap
                                        : text_rect.left();
            LibraryStyleContext::PaintInfoLines(
                &painter, QRect(left, text_rect.top(), info_w, text_rect.height()), info_lines,
                pal.text().color(), bold, Qt::AlignLeft | Qt::AlignVCenter);
        }
    }
}

void DenseGridStyle::resizeEvent(QResizeEvent* event) {
    LibraryStyleView::resizeEvent(event);
    Relayout();
}

void DenseGridStyle::keyPressEvent(QKeyEvent* event) {
    if (entries.isEmpty()) {
        LibraryStyleView::keyPressEvent(event);
        return;
    }
    const int rows_per_page = std::max(1, GridArea().height() / (CellHeight() + Gap));
    switch (event->key()) {
    case Qt::Key_Left:
        Select(current - 1);
        break;
    case Qt::Key_Right:
        Select(current + 1);
        break;
    case Qt::Key_Up:
        Select(current - columns);
        break;
    case Qt::Key_Down:
        Select(current + columns);
        break;
    case Qt::Key_PageUp:
        Select(current - columns * rows_per_page);
        break;
    case Qt::Key_PageDown:
        Select(current + columns * rows_per_page);
        break;
    case Qt::Key_Home:
        Select(0);
        break;
    case Qt::Key_End:
        Select(static_cast<int>(entries.size()) - 1);
        break;
    default:
        LibraryStyleView::keyPressEvent(event);
        return;
    }
    event->accept();
}

void DenseGridStyle::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    const int position = PositionAt(event->position().toPoint());
    if (position >= 0) {
        Select(position);
    }
    LibraryStyleView::mousePressEvent(event);
}

void DenseGridStyle::mouseDoubleClickEvent(QMouseEvent* event) {
    const int position = PositionAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && position >= 0) {
        Select(position);
        emit LaunchRequested(CurrentIndex());
        return;
    }
    LibraryStyleView::mouseDoubleClickEvent(event);
}

void DenseGridStyle::contextMenuEvent(QContextMenuEvent* event) {
    const int position = PositionAt(event->pos());
    if (position >= 0) {
        Select(position);
    }
    emit ContextMenuRequested(position >= 0 ? QModelIndex{entries[position]} : QModelIndex{},
                              event->globalPos());
}

void DenseGridStyle::wheelEvent(QWheelEvent* event) {
    scrollbar->setValue(scrollbar->value() - event->angleDelta().y());
    event->accept();
}

} // namespace LibraryStyles
