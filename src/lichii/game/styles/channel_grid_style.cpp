// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Channels library style. Concept by inxomnyaa.

#include <algorithm>
#include <cmath>

#include <QAbstractItemModel>
#include <QColor>
#include <QContextMenuEvent>
#include <QDate>
#include <QDateTime>
#include <QPolygonF>
#include <QEasingCurve>
#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QLocale>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QResizeEvent>
#include <QStringList>
#include <QTimer>
#include <QTime>
#include <QVariantAnimation>
#include <QWheelEvent>

#include "qt_common/game_list/game_list_p.h"
#include "qt_common/game_list/model.h"
#include "lichii/game/styles/channel_grid_style.h"
#include "lichii/game/styles/style_helpers.h"

namespace LibraryStyles {

namespace {

constexpr int NameHeight = 26;
constexpr int PageMs = 260;

/// TV-screen tile: rounded corners, the sides and the top and bottom bulging slightly.
QPainterPath CrtPath(const QRectF& r) {
    const qreal c = std::min(r.width(), r.height()) * 0.13;
    const qreal bs = r.width() * 0.012;
    const qreal bt = r.height() * 0.022;
    const qreal cx = r.center().x();
    const qreal cy = r.center().y();
    QPainterPath path;
    path.moveTo(r.left() + c, r.top());
    path.quadTo(cx, r.top() - bt, r.right() - c, r.top());
    path.quadTo(r.right(), r.top(), r.right(), r.top() + c);
    path.quadTo(r.right() + bs, cy, r.right(), r.bottom() - c);
    path.quadTo(r.right(), r.bottom(), r.right() - c, r.bottom());
    path.quadTo(cx, r.bottom() + bt, r.left() + c, r.bottom());
    path.quadTo(r.left(), r.bottom(), r.left(), r.bottom() - c);
    path.quadTo(r.left() - bs, cy, r.left(), r.top() + c);
    path.quadTo(r.left(), r.top(), r.left() + c, r.top());
    path.closeSubpath();
    return path;
}

bool IsDark(const QPalette& palette) {
    return palette.window().color().lightness() < 128;
}

int ItemType(const QModelIndex& index) {
    return index.data(GameListItem::TypeRole).toInt();
}

bool IsFolder(const QModelIndex& index) {
    const int type = ItemType(index);
    return type == static_cast<int>(GameListItemType::CustomDir) ||
           type == static_cast<int>(GameListItemType::SdmcDir) ||
           type == static_cast<int>(GameListItemType::UserNandDir) ||
           type == static_cast<int>(GameListItemType::SysNandDir);
}

bool IsGame(const QModelIndex& index) {
    return ItemType(index) == static_cast<int>(GameListItemType::Game);
}

QString TitleOf(const QModelIndex& index) {
    return LibraryStyleContext::Title(index);
}

/// True when a dataChanged with these roles can change the cover art or the title.
bool ArtRolesChanged(const QList<int>& roles) {
    if (roles.isEmpty()) {
        return true;
    }
    for (const int role : roles) {
        if (role == Qt::DecorationRole || role == Qt::DisplayRole ||
            role == GameListItemPath::CardIconRole || role == GameListItemPath::HasIconRole ||
            role == GameListItemPath::TitleRole) {
            return true;
        }
    }
    return false;
}

} // namespace

ChannelGridStyle::ChannelGridStyle(QWidget* parent) : LibraryStyleView(parent) {
    setFocusPolicy(Qt::StrongFocus);
    clock_timer.setSingleShot(true);
    connect(&clock_timer, &QTimer::timeout, this, [this] {
        update();
        ScheduleClock();
    });
    ScheduleClock();
    page_anim.setDuration(PageMs);
    page_anim.setEasingCurve(QEasingCurve::OutCubic);
    connect(&page_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        scroll = value.toReal();
        update();
    });
}

ChannelGridStyle::~ChannelGridStyle() = default;

void ChannelGridStyle::ScheduleClock() {
    // Fire right after the next minute starts so the clock never shows a stale minute.
    const QTime now = QTime::currentTime();
    const int ms = (60 - now.second()) * 1000 - now.msec() + 50;
    clock_timer.start(std::clamp(ms, 100, 61000));
}

void ChannelGridStyle::SetModel(QAbstractItemModel* new_model) {
    if (model) {
        model->disconnect(this);
    }
    model = new_model;
    current = QPersistentModelIndex{};
    entries.clear();
    scroll = 0;
    cover_cache.clear();
    if (model) {
        const auto rebuild = [this] { ScheduleRebuild(); };
        connect(model, &QAbstractItemModel::modelReset, this, rebuild);
        connect(model, &QAbstractItemModel::layoutChanged, this, rebuild);
        connect(model, &QAbstractItemModel::rowsInserted, this, rebuild);
        connect(model, &QAbstractItemModel::rowsRemoved, this, rebuild);
        connect(model, &QAbstractItemModel::rowsMoved, this, rebuild);
        // Play time updates leave the art alone: only icon or title changes drop the cache.
        connect(model, &QAbstractItemModel::dataChanged, this,
                [this](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
                    if (ArtRolesChanged(roles)) {
                        cover_cache.clear();
                    }
                    update();
                });
    }
    Rebuild();
}

void ChannelGridStyle::ScheduleRebuild() {
    if (rebuild_pending) {
        return;
    }
    rebuild_pending = true;
    QTimer::singleShot(0, this, [this] {
        rebuild_pending = false;
        Rebuild();
    });
}

void ChannelGridStyle::Rebuild() {
    // Games are shown flat: top level games plus the games inside each folder row.
    // Favorites and the "add folder" row are not tiles.
    const QModelIndex before{current};
    entries.clear();
    if (model) {
        const int top_rows = model->rowCount();
        for (int r = 0; r < top_rows; ++r) {
            const QModelIndex idx = model->index(r, 0);
            if (IsGame(idx)) {
                entries.emplace_back(idx);
            } else if (IsFolder(idx)) {
                const int children = model->rowCount(idx);
                for (int c = 0; c < children; ++c) {
                    const QModelIndex child = model->index(c, 0, idx);
                    if (IsGame(child)) {
                        entries.emplace_back(child);
                    }
                }
            }
        }
    }
    const int count = static_cast<int>(entries.size());
    if (count == 0) {
        current = QPersistentModelIndex{};
    } else if (CurrentRow() < 0) {
        // Never set, or the row went away: stay near the last known position.
        current = entries[static_cast<size_t>(std::clamp(current_hint, 0, count - 1))];
    }
    ClampScroll();
    if (QModelIndex{current} != before) {
        emit CurrentChanged(QModelIndex{current});
    }
    update();
}

int ChannelGridStyle::RowCount() const {
    return static_cast<int>(entries.size());
}

QModelIndex ChannelGridStyle::IndexForRow(int row) const {
    return (row >= 0 && row < RowCount()) ? QModelIndex{entries[static_cast<size_t>(row)]}
                                           : QModelIndex{};
}

int ChannelGridStyle::CurrentRow() const {
    if (!current.isValid()) {
        return -1;
    }
    const auto it = std::find(entries.begin(), entries.end(), current);
    return it == entries.end() ? -1 : static_cast<int>(it - entries.begin());
}

QModelIndex ChannelGridStyle::CurrentIndex() const {
    return QModelIndex{current};
}

void ChannelGridStyle::SetCurrentIndex(const QModelIndex& index) {
    if (!model || !index.isValid() || index.model() != model) {
        return;
    }
    const QPersistentModelIndex wanted{index.sibling(index.row(), 0)};
    const auto it = std::find(entries.begin(), entries.end(), wanted);
    if (it != entries.end()) {
        SetCurrentRow(static_cast<int>(it - entries.begin()), false);
    }
}

void ChannelGridStyle::SetShowNames(bool show) {
    show_names = show;
    ClampScroll();
    update();
}

void ChannelGridStyle::SetShowInfo(bool show) {
    show_info = show;
    update();
}

ChannelGridStyle::Layout ChannelGridStyle::ComputeLayout() const {
    Layout l;
    const int w = std::max(1, width());
    const int h = std::max(1, height());
    // Dock along the bottom; its edge dips in the middle where the selected name sits.
    l.dock_h = std::clamp(h * 17 / 100, 96, 180);
    l.dock_top = h - l.dock_h;
    l.dip = std::clamp(l.dock_h * 38 / 100, 34, 66);
    l.top = std::clamp(h / 28, 10, 36);
    const int grid_h = std::max(1, l.dock_top - l.top - l.top / 2);

    // TV-shaped tiles packed close together. View > Game Icon Size picks how many rows fit;
    // the tiles then grow to fill the height. The list goes on to the right, a page at a time.
    const int wanted_h = std::max(16, LibraryStyleContext::ArtSize());
    l.gap = std::max(6, wanted_h / 16);
    l.name_h = show_names ? NameHeight : 0;
    l.rows = std::max(1, static_cast<int>(std::lround(static_cast<double>(grid_h + l.gap) /
                                                      (wanted_h + l.name_h + l.gap))));
    l.tile_h = std::max(16, (grid_h + l.gap) / l.rows - l.gap - l.name_h);
    l.tile_w = l.tile_h * 16 / 10;
    const int peek = std::max(28, l.tile_w / 4); // the next page shows at the edges
    if (l.tile_w > w - 2 * peek) {
        l.tile_w = std::max(32, w - 2 * peek);
        l.tile_h = l.tile_w * 10 / 16;
    }
    l.cols = std::max(1, (w - 2 * peek + l.gap) / (l.tile_w + l.gap));
    l.per_page = l.rows * l.cols;
    l.pages = std::max(1, (RowCount() + l.per_page - 1) / l.per_page);
    l.page_w = l.cols * (l.tile_w + l.gap);
    l.left = (w - (l.page_w - l.gap)) / 2;
    const int block_h = l.rows * (l.tile_h + l.name_h + l.gap) - l.gap;
    l.top += std::max(0, (grid_h - block_h) / 2);
    return l;
}

int ChannelGridStyle::PageOf(const Layout& l, int row) const {
    return row / l.per_page;
}

int ChannelGridStyle::ShownPage(const Layout& l) const {
    return std::clamp(static_cast<int>(std::lround(scroll / l.page_w)), 0, l.pages - 1);
}

void ChannelGridStyle::ClampScroll() {
    const Layout l = ComputeLayout();
    const qreal max_scroll = static_cast<qreal>((l.pages - 1) * l.page_w);
    if (page_anim.state() == QAbstractAnimation::Running) {
        return; // the animation ends on a valid page
    }
    scroll = std::clamp(scroll, 0.0, max_scroll);
}

QRect ChannelGridStyle::TileRect(const Layout& l, int row) const {
    const int page = row / l.per_page;
    const int k = row % l.per_page;
    const int line = k / l.cols;
    const int col = k % l.cols;
    const int x = l.left + page * l.page_w + col * (l.tile_w + l.gap) -
                  static_cast<int>(std::lround(scroll));
    return QRect(x, l.top + line * (l.tile_h + l.name_h + l.gap), l.tile_w, l.tile_h);
}

int ChannelGridStyle::RowFor(const Layout& l, int column, int line) const {
    if (column < 0 || line < 0 || line >= l.rows) {
        return -1;
    }
    const int row = (column / l.cols) * l.per_page + line * l.cols + column % l.cols;
    return row < RowCount() ? row : -1;
}

QRect ChannelGridStyle::ArrowRect(const Layout& l, bool right) const {
    const int aw = std::clamp(l.tile_h / 4, 18, 34);
    const int ah = aw * 2;
    const int cy = l.top + (l.rows * (l.tile_h + l.name_h + l.gap) - l.gap) / 2;
    const int x = right ? width() - aw - 6 : 6;
    return QRect(x, cy - ah / 2, aw, ah);
}

int ChannelGridStyle::RowAt(const QPoint& pos) const {
    const Layout l = ComputeLayout();
    if (pos.y() >= l.dock_top) {
        return -1;
    }
    for (int row = 0; row < RowCount(); ++row) {
        const QRect hit = TileRect(l, row).adjusted(-l.gap / 2, -l.gap / 2, l.gap / 2,
                                                    l.name_h + l.gap / 2);
        if (hit.contains(pos)) {
            return row;
        }
    }
    return -1;
}

void ChannelGridStyle::EnsureVisible(int row, bool animate) {
    const Layout l = ComputeLayout();
    const qreal target = static_cast<qreal>(PageOf(l, row) * l.page_w);
    const qreal end = page_anim.state() == QAbstractAnimation::Running
                          ? page_anim.endValue().toReal()
                          : scroll;
    if (std::abs(end - target) < 0.5) {
        return;
    }
    page_anim.stop();
    if (!animate || !isVisible()) {
        scroll = target;
        return;
    }
    page_anim.setStartValue(scroll);
    page_anim.setEndValue(target);
    page_anim.start();
}

void ChannelGridStyle::TurnPage(int step) {
    const Layout l = ComputeLayout();
    const int row = std::max(0, CurrentRow());
    const int page = std::clamp(PageOf(l, row) + step, 0, l.pages - 1);
    if (page == PageOf(l, row)) {
        return;
    }
    // Same spot on the new page, or its last game when that page is shorter.
    const int k = row % l.per_page;
    SetCurrentRow(std::min(page * l.per_page + k, RowCount() - 1), true);
}

void ChannelGridStyle::SetCurrentRow(int row, bool notify) {
    const int count = RowCount();
    if (count == 0) {
        return;
    }
    row = std::clamp(row, 0, count - 1);
    const QModelIndex index = IndexForRow(row);
    if (!index.isValid()) {
        return; // stale entry while a rebuild is pending
    }
    const bool changed = CurrentRow() != row;
    current = QPersistentModelIndex{index};
    current_hint = row;
    EnsureVisible(row);
    update();
    if (changed && notify) {
        emit CurrentChanged(index);
    }
}

void ChannelGridStyle::resizeEvent(QResizeEvent* event) {
    LibraryStyleView::resizeEvent(event);
    cover_cache.clear();
    if (CurrentRow() >= 0) {
        EnsureVisible(CurrentRow(), false);
    }
    ClampScroll();
}

void ChannelGridStyle::keyPressEvent(QKeyEvent* event) {
    const int count = RowCount();
    if (count == 0) {
        LibraryStyleView::keyPressEvent(event);
        return;
    }
    const Layout l = ComputeLayout();
    const int row = std::max(0, CurrentRow());
    const int k = row % l.per_page;
    const int column = PageOf(l, row) * l.cols + k % l.cols;
    const int line = k / l.cols;
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_A:
        if (const int to = RowFor(l, column - 1, line); to >= 0) {
            SetCurrentRow(to, true);
        }
        break;
    case Qt::Key_Right:
    case Qt::Key_D: {
        // Same line in the next column; on a short last page, the nearest game above it.
        int to = -1;
        for (int ln = line; ln >= 0 && to < 0; --ln) {
            to = RowFor(l, column + 1, ln);
        }
        if (to >= 0) {
            SetCurrentRow(to, true);
        }
        break;
    }
    case Qt::Key_Up:
    case Qt::Key_W:
        if (const int to = RowFor(l, column, line - 1); to >= 0) {
            SetCurrentRow(to, true);
        }
        break;
    case Qt::Key_Down:
    case Qt::Key_S:
        if (const int to = RowFor(l, column, line + 1); to >= 0) {
            SetCurrentRow(to, true);
        }
        break;
    case Qt::Key_Home:
        SetCurrentRow(0, true);
        break;
    case Qt::Key_End:
        SetCurrentRow(count - 1, true);
        break;
    case Qt::Key_PageUp:
        TurnPage(-1);
        break;
    case Qt::Key_PageDown:
        TurnPage(1);
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (CurrentRow() >= 0) {
            emit LaunchRequested(QModelIndex{current});
        }
        break;
    default:
        LibraryStyleView::keyPressEvent(event);
        return;
    }
    event->accept();
}

void ChannelGridStyle::contextMenuEvent(QContextMenuEvent* event) {
    // Keyboard menu key: open the menu for the selected tile.
    const int row = CurrentRow();
    if (event->reason() != QContextMenuEvent::Keyboard || row < 0) {
        event->ignore();
        return;
    }
    const QPoint center = TileRect(ComputeLayout(), row).center();
    emit ContextMenuRequested(IndexForRow(row), mapToGlobal(center));
    event->accept();
}

void ChannelGridStyle::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    if (event->button() == Qt::LeftButton) {
        const Layout l = ComputeLayout();
        const int page = ShownPage(l);
        if (page > 0 && ArrowRect(l, false).contains(event->position().toPoint())) {
            TurnPage(-1);
            event->accept();
            return;
        }
        if (page < l.pages - 1 && ArrowRect(l, true).contains(event->position().toPoint())) {
            TurnPage(1);
            event->accept();
            return;
        }
    }
    const int row = RowAt(event->position().toPoint());
    if (row >= 0 && IndexForRow(row).isValid() && (event->button() == Qt::LeftButton || event->button() == Qt::RightButton)) {
        SetCurrentRow(row, true);
        if (event->button() == Qt::RightButton) {
            emit ContextMenuRequested(IndexForRow(row), event->globalPosition().toPoint());
        }
        event->accept();
        return;
    }
    LibraryStyleView::mousePressEvent(event);
}

void ChannelGridStyle::mouseDoubleClickEvent(QMouseEvent* event) {
    const int row = RowAt(event->position().toPoint());
    if (row >= 0 && event->button() == Qt::LeftButton && IndexForRow(row).isValid()) {
        SetCurrentRow(row, true);
        emit LaunchRequested(IndexForRow(row));
        event->accept();
        return;
    }
    LibraryStyleView::mouseDoubleClickEvent(event);
}

void ChannelGridStyle::wheelEvent(QWheelEvent* event) {
    // A wheel notch (either direction) turns one page.
    const QPoint angle = event->angleDelta();
    wheel_rest += std::abs(angle.y()) >= std::abs(angle.x()) ? -angle.y() : -angle.x();
    while (std::abs(wheel_rest) >= 120) {
        const int step = wheel_rest > 0 ? 1 : -1;
        wheel_rest -= step * 120;
        TurnPage(step);
    }
    event->accept();
}

QPixmap ChannelGridStyle::CoverFor(const QModelIndex& index, const QSize& size, qreal radius,
                                   qreal dpr) {
    const QPalette pal = palette();
    const QColor accent = pal.highlight().color();
    const QString key = QStringLiteral("%1|%2|%3x%4|%5|%6|%7")
                            .arg(index.data(GameListItemPath::FullPathRole).toString())
                            .arg(index.data(GameListItemPath::ProgramIdRole).toULongLong())
                            .arg(size.width())
                            .arg(size.height())
                            .arg(radius)
                            .arg(dpr)
                            .arg(accent.rgba());
    if (const QPixmap* cached = cover_cache.object(key)) {
        return *cached;
    }
    const QSize device(std::max(1, int(std::lround(size.width() * dpr))),
                       std::max(1, int(std::lround(size.height() * dpr))));
    QPixmap out(device);
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QRectF rect(QPointF(0, 0), QSizeF(size));
    p.setClipPath(CrtPath(rect));

    QPixmap source;
    if (IsGame(index) && index.data(GameListItemPath::HasIconRole).toBool()) {
        source = index.data(GameListItemPath::CardIconRole).value<QPixmap>();
        if (source.isNull()) {
            source = index.data(Qt::DecorationRole).value<QPixmap>();
        }
    }
    if (!source.isNull()) {
        // Crop to fill: scale so both sides cover the target, then cut the centre.
        const QPixmap scaled =
            source.scaled(device, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        p.drawPixmap(rect, scaled,
                     QRectF((scaled.width() - device.width()) / 2.0,
                            (scaled.height() - device.height()) / 2.0, device.width(),
                            device.height()));
    } else {
        // Default art: a gradient from the palette with the initial of the title.
        const bool dark = IsDark(pal);
        QLinearGradient bg(rect.topLeft(), rect.bottomRight());
        bg.setColorAt(0.0, accent.lighter(dark ? 115 : 135));
        bg.setColorAt(1.0, accent.darker(dark ? 170 : 125));
        p.fillRect(rect, bg);
        const QString title = TitleOf(index);
        QFont f = font();
        f.setBold(true);
        f.setPixelSize(std::max(12, int(rect.height() * 0.42)));
        p.setFont(f);
        QColor ink = pal.highlightedText().color();
        ink.setAlpha(210);
        p.setPen(ink);
        p.drawText(rect, Qt::AlignCenter, title.isEmpty() ? QString{} : title.left(1).toUpper());
    }
    p.end();
    const int cost = std::max(1, device.width() * device.height() * 4 / 1024);
    cover_cache.insert(key, new QPixmap(out), cost);
    return out;
}

void ChannelGridStyle::PaintTile(QPainter& p, const Layout& l, int row, bool selected) {
    const QModelIndex index = IndexForRow(row);
    const QPalette pal = palette();
    const bool dark = IsDark(pal);
    const QRectF tile = TileRect(l, row);
    const QPainterPath shape = CrtPath(tile);
    const QColor accent = LibraryStyleContext::Accent(pal);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, dark ? 55 : 22));
    p.drawPath(shape.translated(0, 3));

    // Glow for the selected tile, under the frame so only the outside shows.
    if (selected) {
        p.setBrush(Qt::NoBrush);
        for (int i = 8; i >= 1; --i) {
            QColor glow = accent;
            glow.setAlpha(10 + (8 - i) * 7);
            p.setPen(QPen(glow, 2.0 * i));
            p.drawPath(shape);
        }
    }

    // Thin pale frame around the screen.
    QLinearGradient frame(tile.topLeft(), tile.bottomLeft());
    frame.setColorAt(0.0, dark ? pal.window().color().lighter(175) : QColor(0xfb, 0xfb, 0xfc));
    frame.setColorAt(1.0, dark ? pal.window().color().lighter(135) : QColor(0xe4, 0xe5, 0xe8));
    QColor edge = pal.text().color();
    edge.setAlpha(dark ? 70 : 60);
    p.setPen(QPen(edge, 1.0));
    p.setBrush(frame);
    p.drawPath(shape);

    const qreal bezel = std::max(2.5, tile.height() * 0.035);
    const QRectF art = tile.adjusted(bezel, bezel, -bezel, -bezel);
    p.drawPixmap(art.topLeft(), CoverFor(index, art.size().toSize(), 0, devicePixelRatioF()));
    QColor inner = Qt::black;
    inner.setAlpha(dark ? 110 : 40);
    p.setPen(QPen(inner, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawPath(CrtPath(art));

    if (selected) {
        p.setPen(QPen(accent, 3.0));
        p.drawPath(shape);
    }

    if (show_names) {
        QFont f = font();
        f.setPixelSize(std::clamp(l.tile_w / 20, 11, 15));
        f.setBold(selected);
        // One line (S4); the selected tile's long name glides with Scroll Long Names.
        const QRectF text(tile.left() + 4, tile.bottom() + 4, tile.width() - 8, l.name_h - 4);
        LibraryStyleContext::PaintTitle(&p, text.toAlignedRect(), TitleOf(index),
                                        pal.windowText().color(), f,
                                        Qt::AlignHCenter | Qt::AlignTop, 1,
                                        selected ? this : nullptr, 4);
    }
}

void ChannelGridStyle::PaintDock(QPainter& p, const Layout& l) {
    const QPalette pal = palette();
    const bool dark = IsDark(pal);
    const QColor accent = LibraryStyleContext::Accent(pal);
    const int w = width();
    const int h = height();
    const qreal top = l.dock_top;
    const qreal low = top + l.dip;

    // Edge: level at the sides, dipping in a smooth curve under the middle.
    const qreal x1 = w * 0.12;
    const qreal x2 = w * 0.26;
    const qreal k = (x2 - x1) * 0.55;
    QPainterPath edge_path;
    edge_path.moveTo(-2, top);
    edge_path.lineTo(x1, top);
    edge_path.cubicTo(x1 + k, top, x2 - k, low, x2, low);
    edge_path.lineTo(w - x2, low);
    edge_path.cubicTo(w - x2 + k, low, w - x1 - k, top, w - x1, top);
    edge_path.lineTo(w + 2, top);
    QPainterPath body = edge_path;
    body.lineTo(w + 2, h + 2);
    body.lineTo(-2, h + 2);
    body.closeSubpath();

    p.save();
    p.setBrush(Qt::NoBrush);
    for (int i = 1; i <= 4; ++i) {
        p.setPen(QPen(QColor(0, 0, 0, (dark ? 18 : 8) * (5 - i) / 4), 2.0));
        p.drawPath(edge_path.translated(0, -2.0 * i));
    }
    const QColor base = pal.window().color();
    QLinearGradient fill(0, top, 0, h);
    fill.setColorAt(0.0, dark ? base.lighter(135) : base.lighter(104));
    fill.setColorAt(1.0, dark ? base.lighter(112) : base.darker(108));
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawPath(body);
    QColor line = accent;
    line.setAlpha(dark ? 200 : 220);
    p.setPen(QPen(line, 3.0));
    p.setBrush(Qt::NoBrush);
    p.drawPath(edge_path.translated(0, 1));
    p.restore();

    // In the dip: the selected name in the accent colour, its info under it.
    const QModelIndex index{current};
    if (index.isValid()) {
        const QRect dip(static_cast<int>(x2), static_cast<int>(top) + 2,
                        std::max(0, static_cast<int>(w - 2 * x2)), l.dip - 4);
        QFont tf = font();
        tf.setBold(true);
        tf.setPixelSize(std::clamp(l.dip * 40 / 100, 13, 26));
        QFont mf = font();
        mf.setPixelSize(std::clamp(l.dip * 24 / 100, 10, 15));
        const QStringList meta =
            show_info ? LibraryStyleContext::InfoLines(
                            LibraryStyleContext::Info(index),
                            LibraryStyleContext::LayoutInfo(mf, dip.width(), 1), mf)
                      : QStringList{};
        const int title_h = QFontMetrics(tf).height();
        const int meta_h = meta.isEmpty() ? 0 : QFontMetrics(mf).height();
        const int y = dip.top() + (dip.height() - title_h - meta_h) / 2;
        LibraryStyleContext::PaintTitle(&p, QRect(dip.left(), y, dip.width(), title_h),
                                        TitleOf(index), accent, tf,
                                        Qt::AlignHCenter | Qt::AlignVCenter, 1, this);
        if (!meta.isEmpty()) {
            LibraryStyleContext::PaintInfoLines(
                &p, QRect(dip.left(), y + title_h, dip.width(), meta_h), meta,
                LibraryStyleContext::MutedText(pal), mf, Qt::AlignHCenter | Qt::AlignVCenter);
        }
    }

    // Round buttons in the corners: Lichii on the left, the game count on the right.
    const qreal radius = std::clamp(l.dock_h * 0.36, 26.0, 60.0);
    const qreal cy = top + l.dock_h * 0.56;
    const qreal side = std::max(14.0, w * 0.035) + radius;
    const auto circle = [&](qreal cx) {
        const QRectF r(cx - radius, cy - radius, radius * 2, radius * 2);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, dark ? 60 : 30));
        p.drawEllipse(r.translated(0, 3));
        QLinearGradient g(r.topLeft(), r.bottomLeft());
        g.setColorAt(0.0, dark ? base.lighter(175) : QColor(0xff, 0xff, 0xff));
        g.setColorAt(1.0, dark ? base.lighter(130) : QColor(0xe2, 0xe4, 0xe8));
        p.setBrush(g);
        p.setPen(QPen(accent, std::max(2.0, radius * 0.07)));
        p.drawEllipse(r);
        return r;
    };
    {
        const QRectF r = circle(side);
        static const QPixmap logo(QStringLiteral(":/icons/default/256x256/lichii.png"));
        if (!logo.isNull()) {
            const qreal s = radius * 1.15;
            p.drawPixmap(QRectF(r.center().x() - s / 2, r.center().y() - s / 2, s, s), logo,
                         QRectF(logo.rect()));
        }
    }
    {
        const QRectF r = circle(w - side);
        QFont nf = font();
        nf.setBold(true);
        nf.setPixelSize(std::clamp(static_cast<int>(radius * 0.62), 12, 34));
        QFont lf = font();
        lf.setPixelSize(std::clamp(static_cast<int>(radius * 0.26), 9, 14));
        const int nh = QFontMetrics(nf).height();
        const int lh = QFontMetrics(lf).height();
        const qreal y = r.center().y() - (nh + lh) / 2.0;
        p.setPen(pal.windowText().color());
        p.setFont(nf);
        p.drawText(QRectF(r.left(), y, r.width(), nh), Qt::AlignCenter,
                   QString::number(RowCount()));
        p.setPen(LibraryStyleContext::MutedText(pal));
        p.setFont(lf);
        p.drawText(QRectF(r.left(), y + nh - 2, r.width(), lh), Qt::AlignCenter,
                   tr("games"));
    }

    // Under the dip, between the buttons: day, date and time, the button hints under them.
    const int inner_l = static_cast<int>(side + radius + 16);
    const int inner_w = std::max(0, w - 2 * inner_l);
    const QLocale locale;
    const QDateTime now = QDateTime::currentDateTime();
    const QString date_text = QStringLiteral("%1 %2   %3")
                                  .arg(locale.dayName(now.date().dayOfWeek(), QLocale::ShortFormat),
                                       locale.toString(now.date(), QLocale::ShortFormat),
                                       locale.toString(now.time(), QLocale::ShortFormat));
    QFont df = font();
    df.setBold(true);
    df.setPixelSize(std::clamp((h - static_cast<int>(low)) * 30 / 100, 14, 38));
    QFont hint_font = font();
    hint_font.setBold(true);
    hint_font.setPixelSize(std::clamp(l.dock_h * 10 / 100, 10, 13));
    const qreal hint_r = hint_font.pixelSize() * 0.75;
    const auto hints = LibraryStyleContext::StandardHints();
    const qreal hints_w = LibraryStyleContext::HintsWidth(hints, hint_font, hint_r);
    const bool show_hints = hints_w <= inner_w;
    const int date_h = QFontMetrics(df).height();
    const int block = date_h + (show_hints ? 6 + static_cast<int>(hint_r * 2) : 0);
    const int by = static_cast<int>(low) + (h - static_cast<int>(low) - block) / 2;
    QColor ink = pal.windowText().color();
    ink.setAlpha(200);
    p.setPen(ink);
    p.setFont(df);
    p.drawText(QRect(inner_l, by, inner_w, date_h), Qt::AlignCenter,
               QFontMetrics(df).elidedText(date_text, Qt::ElideRight, inner_w));
    if (show_hints) {
        LibraryStyleContext::PaintHints(&p, hints, w / 2.0 - hints_w / 2,
                                        by + date_h + 6 + hint_r, hint_font, hint_r,
                                        LibraryStyleContext::MutedText(pal));
    }
}

void ChannelGridStyle::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QPalette pal = palette();
    const bool dark = IsDark(pal);
    const Layout l = ComputeLayout();

    // Background: a gentle vertical gradient with faint pinstripes, rendered once per size
    // and palette.
    const QColor base = pal.window().color();
    const qreal dpr = devicePixelRatioF();
    if (background.isNull() || background_size != size() || background_dpr != dpr ||
        background_base != base.rgba()) {
        background = QPixmap(QSize(std::max(1, static_cast<int>(std::ceil(width() * dpr))),
                                   std::max(1, static_cast<int>(std::ceil(height() * dpr)))));
        background.setDevicePixelRatio(dpr);
        QPainter bp(&background);
        QLinearGradient bg(0, 0, 0, height());
        bg.setColorAt(0.0, dark ? base.lighter(118) : base.lighter(105));
        bg.setColorAt(1.0, dark ? base : base.darker(104));
        bp.fillRect(QRect(0, 0, width(), height()), bg);
        QColor stripe = dark ? QColor(255, 255, 255, 6) : QColor(0, 0, 0, 7);
        for (int y = 3; y < height(); y += 4) {
            bp.fillRect(QRect(0, y, width(), 1), stripe);
        }
        background_size = size();
        background_dpr = dpr;
        background_base = base.rgba();
    }
    p.drawPixmap(0, 0, background);

    // Tiles of the pages in view (the current one and the edges of its neighbours).
    const int count = RowCount();
    const int current_row = CurrentRow();
    if (count > 0) {
        p.save();
        p.setClipRect(QRect(0, 0, width(), l.dock_top));
        const int first_page =
            std::max(0, static_cast<int>(std::floor((scroll - l.left) / l.page_w)));
        const int last_page = std::min(
            l.pages - 1, static_cast<int>(std::floor((scroll + width()) / l.page_w)));
        static const QPixmap logo(QStringLiteral(":/icons/default/256x256/lichii.png"));
        for (int page = first_page; page <= last_page; ++page) {
            const int end = std::min(count, (page + 1) * l.per_page);
            for (int row = page * l.per_page; row < end; ++row) {
                if (row != current_row) {
                    PaintTile(p, l, row, false);
                }
            }
            // Empty slots fill the rest of the page: a faint frame with a faint Lichii icon.
            for (int row = end; row < (page + 1) * l.per_page; ++row) {
                const QRectF slot = TileRect(l, row);
                const QPainterPath shape = CrtPath(slot);
                QColor fill = pal.text().color();
                fill.setAlpha(dark ? 10 : 8);
                QColor edge = pal.text().color();
                edge.setAlpha(dark ? 32 : 30);
                p.setPen(QPen(edge, 1.0));
                p.setBrush(fill);
                p.drawPath(shape);
                if (!logo.isNull()) {
                    const qreal side = slot.height() * 0.5;
                    p.save();
                    p.setOpacity(dark ? 0.07 : 0.10);
                    p.drawPixmap(QRectF(slot.center().x() - side / 2, slot.center().y() - side / 2,
                                        side, side),
                                 logo, QRectF(logo.rect()));
                    p.restore();
                }
            }
        }
        // The selected tile goes last so its glow is not covered by neighbours.
        if (current_row >= 0) {
            const QRect t = TileRect(l, current_row);
            if (t.right() > -20 && t.left() < width() + 20) {
                PaintTile(p, l, current_row, true);
            }
        }
        p.restore();

        // Page arrows at the edges.
        const int page = ShownPage(l);
        const QColor accent = LibraryStyleContext::Accent(pal);
        for (const bool right : {false, true}) {
            if (right ? page >= l.pages - 1 : page <= 0) {
                continue;
            }
            const QRectF a = ArrowRect(l, right);
            QPolygonF tri;
            if (right) {
                tri << a.topLeft() << QPointF(a.right(), a.center().y()) << a.bottomLeft();
            } else {
                tri << a.topRight() << QPointF(a.left(), a.center().y()) << a.bottomRight();
            }
            QPainterPath arrow;
            arrow.addPolygon(tri);
            arrow.closeSubpath();
            p.setPen(QPen(dark ? QColor(20, 24, 30) : QColor(255, 255, 255), 3.0,
                          Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            QLinearGradient g(a.topLeft(), a.bottomLeft());
            g.setColorAt(0.0, accent.lighter(130));
            g.setColorAt(1.0, accent);
            p.setBrush(g);
            p.drawPath(arrow);
        }
    }

    PaintDock(p, l);
}

namespace {

LibraryStyleView* Make(QWidget* parent) {
    return new ChannelGridStyle(parent);
}

[[maybe_unused]] const bool registered =
    LibraryStyles::Register(
        {"channel_grid", QT_TRANSLATE_NOOP("LibraryStyles", "Channels"), &Make});

} // namespace

} // namespace LibraryStyles
