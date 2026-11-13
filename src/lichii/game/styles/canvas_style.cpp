// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Canvas library style. Concept by inxomnyaa.

#include <algorithm>
#include <cmath>

#include <QAbstractItemModel>
#include <QColor>
#include <QContextMenuEvent>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QStringList>
#include <QTimer>
#include <QWheelEvent>

#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/game_list_p.h"
#include "lichii/game/styles/canvas_style.h"
#include "lichii/game/styles/style_helpers.h"

namespace LibraryStyles {

namespace {

LibraryStyleView* Make(QWidget* p) {
    return new CanvasStyle(p);
}

[[maybe_unused]] const bool registered =
    LibraryStyles::Register({"canvas", QT_TRANSLATE_NOOP("LibraryStyles", "Canvas"), &Make});

constexpr int SidePadding = 28;
constexpr int FooterBarHeight = 48;

bool IsGameType(GameListItemType type) {
    return type == GameListItemType::Game;
}

bool IsFolderType(GameListItemType type) {
    return type == GameListItemType::CustomDir || type == GameListItemType::SdmcDir ||
           type == GameListItemType::UserNandDir || type == GameListItemType::SysNandDir;
}

GameListItemType TypeOf(const QModelIndex& index) {
    return index.data(GameListItem::TypeRole).value<GameListItemType>();
}

QColor HueColor(uint seed, int saturation, int lightness) {
    return QColor::fromHsl(static_cast<int>(seed % 360), saturation, lightness);
}

void DrawPlaceholder(QPainter* painter, const QRectF& rect, const QString& title, uint seed,
                     bool dark) {
    QLinearGradient grad(rect.topLeft(), rect.bottomRight());
    grad.setColorAt(0.0, HueColor(seed, dark ? 90 : 120, dark ? 70 : 150));
    grad.setColorAt(1.0, HueColor(seed + 40, dark ? 100 : 130, dark ? 40 : 100));
    painter->fillRect(rect, grad);
    const QString initial = title.trimmed().left(1).toUpper();
    if (initial.isEmpty()) {
        return;
    }
    QFont font = painter->font();
    font.setBold(true);
    font.setPixelSize(std::max(12, static_cast<int>(rect.height() * 0.42)));
    painter->setFont(font);
    painter->setPen(QColor(255, 255, 255, 190));
    painter->drawText(rect, Qt::AlignCenter, initial);
}

} // namespace

CanvasStyle::CanvasStyle(QWidget* parent) : LibraryStyleView(parent) {
    setObjectName(QStringLiteral("canvasLibraryStyle"));
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
}

CanvasStyle::~CanvasStyle() = default;

void CanvasStyle::SetModel(QAbstractItemModel* new_model) {
    if (model) {
        model->disconnect(this);
    }
    model = new_model;
    if (model) {
        const auto rebuild = [this] { ScheduleRebuild(); };
        connect(model, &QAbstractItemModel::modelReset, this, rebuild);
        connect(model, &QAbstractItemModel::layoutChanged, this, rebuild);
        connect(model, &QAbstractItemModel::rowsInserted, this, rebuild);
        connect(model, &QAbstractItemModel::rowsRemoved, this, rebuild);
        connect(model, &QAbstractItemModel::rowsMoved, this, rebuild);
        connect(model, &QAbstractItemModel::dataChanged, this, [this] { update(); });
    }
    current = -1;
    scroll = 0;
    Rebuild();
}

QModelIndex CanvasStyle::CurrentIndex() const {
    return IndexAt(current);
}

void CanvasStyle::SetCurrentIndex(const QModelIndex& index) {
    if (!index.isValid()) {
        SelectRow(-1);
        return;
    }
    if (rebuild_pending) {
        Rebuild();
    }
    const QPersistentModelIndex wanted{index.sibling(index.row(), 0)};
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i] == wanted) {
            SelectRow(static_cast<int>(i));
            return;
        }
    }
}

void CanvasStyle::SetShowNames(bool show) {
    show_names = show;
    // Also called when the cover size changes: the tile size follows it.
    UpdateLayout();
    EnsureVisible(current);
    update();
}

void CanvasStyle::SetShowInfo(bool show) {
    show_info = show;
    update();
}

QModelIndex CanvasStyle::IndexAt(int row) const {
    if (row < 0 || row >= static_cast<int>(entries.size())) {
        return {};
    }
    return QModelIndex(entries[static_cast<size_t>(row)]);
}

void CanvasStyle::ScheduleRebuild() {
    if (rebuild_pending) {
        return;
    }
    rebuild_pending = true;
    QTimer::singleShot(0, this, [this] {
        rebuild_pending = false;
        Rebuild();
    });
}

void CanvasStyle::Rebuild() {
    const QPersistentModelIndex previous{IndexAt(current)};
    entries.clear();
    if (model) {
        // Games are shown flat: top level games plus the games inside each folder row.
        // Favorites and "add folder" rows are not tiles.
        const int top_rows = model->rowCount();
        for (int r = 0; r < top_rows; ++r) {
            const QModelIndex idx = model->index(r, 0);
            const GameListItemType type = TypeOf(idx);
            if (IsGameType(type)) {
                entries.emplace_back(idx);
            } else if (IsFolderType(type)) {
                const int children = model->rowCount(idx);
                for (int c = 0; c < children; ++c) {
                    const QModelIndex child = model->index(c, 0, idx);
                    if (IsGameType(TypeOf(child))) {
                        entries.emplace_back(child);
                    }
                }
            }
        }
    }

    int new_current = -1;
    if (previous.isValid()) {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i] == previous) {
                new_current = static_cast<int>(i);
                break;
            }
        }
    }
    if (new_current < 0 && !entries.empty()) {
        new_current = std::clamp(current, 0, static_cast<int>(entries.size()) - 1);
    }
    const QModelIndex before = previous;
    current = new_current;
    UpdateLayout();
    if (IndexAt(current) != before) {
        emit CurrentChanged(IndexAt(current));
    }
    update();
}

CanvasStyle::Layout CanvasStyle::ComputeLayout() const {
    Layout l;
    l.gap = 18;
    // Tiles are exactly View > Game Icon Size; the grid is centred instead of stretched.
    const int target = LibraryStyleContext::ArtSize();
    const int avail = std::max(1, width() - 2 * SidePadding);
    l.columns = std::max(1, (avail + l.gap) / (target + l.gap));
    const int tile = std::min(target, avail);
    l.tile = tile;
    const int grid_width = l.columns * tile + (l.columns - 1) * l.gap;
    l.margin = std::max(SidePadding, (width() - grid_width) / 2);
    l.top = SidePadding;
    return l;
}

int CanvasStyle::FooterHeight() const {
    return FooterBarHeight + 24;
}

void CanvasStyle::UpdateLayout() {
    layout = ComputeLayout();
    scroll = std::clamp(scroll, 0, MaxScroll());
}

int CanvasStyle::MaxScroll() const {
    const int count = static_cast<int>(entries.size());
    const int rows = (count + layout.columns - 1) / layout.columns;
    const int content = layout.top + rows * (layout.tile + layout.gap) + FooterHeight();
    return std::max(0, content - height());
}

QRect CanvasStyle::TileRect(int row) const {
    const int col = row % layout.columns;
    const int line = row / layout.columns;
    return QRect(layout.margin + col * (layout.tile + layout.gap),
                 layout.top + line * (layout.tile + layout.gap), layout.tile, layout.tile);
}

int CanvasStyle::RowAt(const QPoint& pos) const {
    const int x = pos.x() - layout.margin;
    const int y = pos.y() + scroll - layout.top;
    if (x < 0 || y < 0) {
        return -1;
    }
    const int step = layout.tile + layout.gap;
    const int col = x / step;
    const int line = y / step;
    if (col >= layout.columns || x % step >= layout.tile || y % step >= layout.tile) {
        return -1;
    }
    const int row = line * layout.columns + col;
    return row < static_cast<int>(entries.size()) ? row : -1;
}

void CanvasStyle::SetScroll(int value) {
    value = std::clamp(value, 0, MaxScroll());
    if (value != scroll) {
        scroll = value;
        update();
    }
}

void CanvasStyle::EnsureVisible(int row) {
    if (row < 0) {
        return;
    }
    const QRect tile = TileRect(row);
    const int pad = 12;
    const int visible_bottom = height() - FooterHeight() + 16;
    if (tile.top() - pad < scroll) {
        SetScroll(tile.top() - pad);
    } else if (tile.bottom() + pad > scroll + visible_bottom) {
        SetScroll(tile.bottom() + pad - visible_bottom);
    }
}

void CanvasStyle::SelectRow(int row, bool notify) {
    if (row >= static_cast<int>(entries.size())) {
        row = static_cast<int>(entries.size()) - 1;
    }
    if (row < -1) {
        row = -1;
    }
    if (row == current) {
        EnsureVisible(row);
        return;
    }
    current = row;
    EnsureVisible(row);
    update();
    if (notify) {
        emit CurrentChanged(IndexAt(current));
    }
}

void CanvasStyle::resizeEvent(QResizeEvent* event) {
    LibraryStyleView::resizeEvent(event);
    UpdateLayout();
    EnsureVisible(current);
}

void CanvasStyle::changeEvent(QEvent* event) {
    LibraryStyleView::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        update();
    }
}

void CanvasStyle::contextMenuEvent(QContextMenuEvent* event) {
    event->accept();
    if (event->reason() == QContextMenuEvent::Keyboard) {
        if (current >= 0) {
            const QRect tile = TileRect(current).translated(0, -scroll);
            emit ContextMenuRequested(IndexAt(current), mapToGlobal(tile.center()));
        }
        return;
    }
    // A right click on a tile already opened the menu on press; empty space gets the menu
    // without a game (add a games folder).
    if (RowAt(event->pos()) < 0) {
        emit ContextMenuRequested(QModelIndex{}, event->globalPos());
    }
}

void CanvasStyle::wheelEvent(QWheelEvent* event) {
    int delta = 0;
    if (!event->pixelDelta().isNull()) {
        delta = -event->pixelDelta().y();
    } else {
        delta = -event->angleDelta().y() * layout.tile * 6 / 10 / 120;
    }
    SetScroll(scroll + delta);
    event->accept();
}

void CanvasStyle::keyPressEvent(QKeyEvent* event) {
    const int count = static_cast<int>(entries.size());
    if (count == 0 ||
        (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        LibraryStyleView::keyPressEvent(event);
        return;
    }
    const int cols = layout.columns;
    int row = current < 0 ? 0 : current;
    const int page_rows = std::max(1, height() / (layout.tile + layout.gap) - 1);
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_A:
        row = std::max(0, row - 1);
        break;
    case Qt::Key_Right:
    case Qt::Key_D:
        row = std::min(count - 1, row + 1);
        break;
    case Qt::Key_Up:
    case Qt::Key_W:
        if (row - cols >= 0) {
            row -= cols;
        }
        break;
    case Qt::Key_Down:
    case Qt::Key_S:
        if (row + cols < count) {
            row += cols;
        } else if (row / cols < (count - 1) / cols) {
            row = count - 1;
        }
        break;
    case Qt::Key_Home:
        row = 0;
        break;
    case Qt::Key_End:
        row = count - 1;
        break;
    case Qt::Key_PageUp:
        row = std::max(0, row - cols * page_rows);
        break;
    case Qt::Key_PageDown:
        row = std::min(count - 1, row + cols * page_rows);
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (current >= 0) {
            emit LaunchRequested(IndexAt(current));
        }
        return;
    default:
        LibraryStyleView::keyPressEvent(event);
        return;
    }
    SelectRow(row);
}

void CanvasStyle::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    const int row = RowAt(event->position().toPoint());
    if (row < 0) {
        LibraryStyleView::mousePressEvent(event);
        return;
    }
    if (event->button() == Qt::LeftButton || event->button() == Qt::RightButton) {
        SelectRow(row);
    }
    if (event->button() == Qt::RightButton) {
        emit ContextMenuRequested(IndexAt(row), event->globalPosition().toPoint());
    }
    event->accept();
}

void CanvasStyle::mouseDoubleClickEvent(QMouseEvent* event) {
    const int row = RowAt(event->position().toPoint());
    if (row >= 0 && event->button() == Qt::LeftButton) {
        SelectRow(row);
        emit LaunchRequested(IndexAt(row));
        event->accept();
        return;
    }
    LibraryStyleView::mouseDoubleClickEvent(event);
}

QPixmap CanvasStyle::TileArt(const QModelIndex& index, int side, qreal radius, bool dark) {
    const qreal ratio = devicePixelRatioF();
    const QPixmap card = index.data(GameListItemPath::HasIconRole).toBool()
                             ? index.data(GameListItemPath::CardIconRole).value<QPixmap>()
                             : QPixmap{};
    const QString key = QStringLiteral("%1|%2|%3|%4|%5|%6")
                            .arg(index.data(GameListItemPath::FullPathRole).toString())
                            .arg(card.cacheKey())
                            .arg(side)
                            .arg(ratio)
                            .arg(radius)
                            .arg(dark ? 1 : 0);
    if (const QPixmap* cached = art_cache.object(key)) {
        return *cached;
    }
    const int device = std::max(1, static_cast<int>(std::ceil(side * ratio)));
    QPixmap pixmap(device, device);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QRectF rectf(0, 0, side, side);
    QPainterPath clip;
    clip.addRoundedRect(rectf, radius, radius);
    p.setClipPath(clip);
    const QString title = index.data(GameListItemPath::TitleRole).toString();
    QPixmap art = card;
    if (art.isNull() && index.data(GameListItemPath::HasIconRole).toBool()) {
        art = index.data(Qt::DecorationRole).value<QPixmap>();
    }
    if (!art.isNull()) {
        QSize scaled = art.size();
        scaled.scale(QSize(device, device), Qt::KeepAspectRatioByExpanding);
        const QPixmap sized = art.scaled(scaled, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        p.drawPixmap(QRectF(rectf.center().x() - scaled.width() / ratio / 2,
                            rectf.center().y() - scaled.height() / ratio / 2,
                            scaled.width() / ratio, scaled.height() / ratio),
                     sized, QRectF(sized.rect()));
    } else {
        const qulonglong program_id = index.data(GameListItemPath::ProgramIdRole).toULongLong();
        const uint seed = program_id != 0 ? static_cast<uint>(qHash(program_id))
                                          : static_cast<uint>(qHash(title));
        DrawPlaceholder(&p, rectf, title, seed, dark);
    }
    p.end();
    art_cache.insert(key, new QPixmap(pixmap),
                     std::max(1, device * device * 4 / 1024));
    return pixmap;
}

void CanvasStyle::PaintBackground(QPainter& painter, bool dark) {
    const QPalette pal = palette();
    const qreal ratio = devicePixelRatioF();
    const QRgb window = pal.window().color().rgb();
    if (background.isNull() || background_size != size() || background_ratio != ratio ||
        background_window != window || background_dark != dark) {
        const int w = width();
        const int h = height();
        QPixmap pixmap(QSize(std::max(1, static_cast<int>(w * ratio)),
                             std::max(1, static_cast<int>(h * ratio))));
        pixmap.setDevicePixelRatio(ratio);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing, true);
        // Day or night background and arcs follow the palette.
        const QColor bg = dark ? pal.window().color().darker(135) : QColor(246, 233, 212);
        p.fillRect(QRect(0, 0, w, h), bg);
        // Lichii icon tiled in staggered rows, the whole pattern slanted.
        LibraryStyleContext::PaintLogoTiles(&p, QRectF(0, 0, w, h),
                                            std::clamp(h / 6.5, 56.0, 150.0), dark ? 0.07 : 0.11);
        p.end();
        background = pixmap;
        background_size = size();
        background_ratio = ratio;
        background_window = window;
        background_dark = dark;
    }
    painter.drawPixmap(0, 0, background);
}

void CanvasStyle::paintEvent([[maybe_unused]] QPaintEvent* event) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QPalette pal = palette();
    const bool dark = pal.window().color().lightness() < 128;
    const QColor accent = LibraryStyleContext::Accent(pal);
    const int w = width();
    const int h = height();

    PaintBackground(p, dark);
    p.setPen(Qt::NoPen);

    // Tiles: only the visible rows are painted.
    const int count = static_cast<int>(entries.size());
    if (count > 0) {
        const int step = layout.tile + layout.gap;
        const int first_line = std::max(0, (scroll - layout.top) / step - 1);
        const int last_line = std::max(0, (scroll + h - layout.top) / step + 1);
        const int first = first_line * layout.columns;
        const int last = std::min(count - 1, (last_line + 1) * layout.columns - 1);
        const qreal radius = layout.tile * 0.14;

        for (int row = first; row <= last; ++row) {
            const QModelIndex index = IndexAt(row);
            if (!index.isValid()) {
                continue;
            }
            const bool selected = row == current;
            QRect rect = TileRect(row).translated(0, -scroll);
            if (rect.bottom() < -8 || rect.top() > h + 8) {
                continue;
            }
            if (selected) {
                // The selected tile lifts a little, like the concept.
                const int lift = std::max(2, layout.tile / 40);
                rect.adjust(-lift, -lift, lift, lift);
            }
            const QRectF rectf(rect);

            // Concept shadow: 0 8px 20px, 45 % black at night, 25 % by day; the selected tile
            // casts a deeper one (0 14px 30px).
            {
                const int blur = selected ? 30 : 20;
                const qreal offset = selected ? 14.0 : 8.0;
                const QPixmap shadow = LibraryStyleContext::SoftShadow(
                    rect.size(), radius, blur, dark ? 0.45 : 0.25, devicePixelRatioF());
                p.drawPixmap(rectf.topLeft() + QPointF(-blur, -blur + offset), shadow);
            }
            p.setPen(Qt::NoPen);

            // The art fills the (lifted) tile, so art, clip and ring always line up.
            {
                const QPixmap art = TileArt(index, layout.tile, radius, dark);
                p.drawPixmap(rectf, art, QRectF(QPointF(0, 0), QSizeF(art.size())));
            }
            p.save();
            if (selected && (show_names || show_info)) {
                QPainterPath clip;
                clip.addRoundedRect(rectf, radius, radius);
                p.setClipPath(clip);
                // Text and padding follow the tile size (the same for every tile of the view):
                // 7 px on the smallest tiles, up to 20 px.
                const int pad = std::clamp(layout.tile / 18, 3, 12);
                QFont title_font = font();
                title_font.setFamilies({QStringLiteral("Nunito"), title_font.family()});
                title_font.setWeight(QFont::Black);
                title_font.setPixelSize(std::clamp(4 + layout.tile / 16, 7, 20));
                QFont sub_font = title_font;
                sub_font.setWeight(QFont::ExtraBold);
                sub_font.setPixelSize(std::max(6, title_font.pixelSize() - 2));
                const QFontMetrics tfm(title_font);
                const QFontMetrics sfm(sub_font);
                const int text_w = rect.width() - 2 * pad;
                // Same title and Game Info rules as the cards (style_helpers).
                const QString title = LibraryStyleContext::Info(index).title;
                QStringList info_lines =
                    show_info ? LibraryStyleContext::InfoLines(
                                    LibraryStyleContext::Info(index),
                                    LibraryStyleContext::LayoutInfo(sub_font, text_w), sub_font)
                              : QStringList{};
                int title_lines = 0;
                if (show_names) {
                    title_lines = tfm.horizontalAdvance(title) <= text_w
                                      ? 1
                                      : LibraryStyleContext::TitleLines(2);
                }
                // Never taller than the tile: the title drops to one line first, then the info
                // keeps the lines that fit.
                const int room = rect.height() - 2 * pad - 4;
                const auto needed = [&] {
                    return title_lines * tfm.lineSpacing() +
                           static_cast<int>(info_lines.size()) * sfm.lineSpacing();
                };
                if (needed() > room && title_lines > 1) {
                    title_lines = 1;
                }
                while (needed() > room && !info_lines.isEmpty()) {
                    info_lines.removeLast();
                }
                const int info_h = static_cast<int>(info_lines.size()) * sfm.lineSpacing();
                const int text_h = title_lines * tfm.lineSpacing() + info_h + 4;
                const int grad_h = text_h + pad * 3;
                const QRectF grad_rect(rectf.left(), rectf.bottom() - grad_h, rectf.width(),
                                       grad_h);
                QLinearGradient g(grad_rect.topLeft(), grad_rect.bottomLeft());
                g.setColorAt(0.0, QColor(0, 0, 0, 0));
                g.setColorAt(0.55, QColor(0, 0, 0, 170));
                g.setColorAt(1.0, QColor(0, 0, 0, 225));
                p.fillRect(grad_rect, g);

                int y = rect.bottom() - pad - text_h + 4;
                if (title_lines > 0) {
                    LibraryStyleContext::PaintTitle(
                        &p, QRect(rect.left() + pad, y, text_w, title_lines * tfm.lineSpacing()),
                        title, Qt::white, title_font, Qt::AlignHCenter | Qt::AlignTop,
                        title_lines, this, pad - 2);
                    y += title_lines * tfm.lineSpacing();
                }
                p.setFont(sub_font);
                p.setPen(Qt::white);
                for (const QString& line : info_lines) {
                    p.drawText(QRect(rect.left() + pad, y, text_w, sfm.lineSpacing()),
                               Qt::AlignHCenter | Qt::AlignVCenter,
                               sfm.elidedText(line, Qt::ElideRight, text_w));
                    y += sfm.lineSpacing();
                }
            }
            p.restore();

            if (selected) {
                // Concept ring: 5 px wide, 4 px away from the tile (CSS outline-offset), scaled
                // down on small tiles; concentric corners.
                const qreal k = std::min(1.0, layout.tile / 150.0);
                const qreal pen = std::max(2.5, 5.0 * k);
                const qreal gap = std::max(2.0, 4.0 * k);
                const qreal grow = gap + pen / 2;
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(accent, pen));
                p.drawRoundedRect(rectf.adjusted(-grow, -grow, grow, grow), radius + grow,
                                  radius + grow);
            }
        }
    }

    // Button hints, bottom centre.
    {
        QFont font_hint = font();
        font_hint.setBold(true);
        font_hint.setPixelSize(13);
        p.setFont(font_hint);
        const QFontMetrics fm(font_hint);
        struct Hint {
            QStringList glyphs;
            QString label;
        };
        const Hint hints[] = {{{QStringLiteral("A")}, tr("Play")},
                              {{QStringLiteral("B")}, tr("Names")},
                              {{QStringLiteral("Y")}, tr("Info")},
                              {{QStringLiteral("L"), QStringLiteral("R")}, tr("Style")}};
        const qreal radius = 9.5;
        const qreal glyph_gap = 4;
        const qreal item_gap = 22;
        qreal total = 0;
        for (const Hint& hint : hints) {
            for (const QString& glyph : hint.glyphs) {
                total += LibraryStyleContext::GlyphWidth(glyph, radius) + glyph_gap;
            }
            total += fm.horizontalAdvance(hint.label);
        }
        total += item_gap * 3;
        const QRectF bar(w / 2.0 - total / 2 - 20, h - FooterBarHeight - 10, total + 40,
                         FooterBarHeight - 8);
        QColor bar_color = dark ? pal.window().color() : QColor(255, 255, 255);
        bar_color.setAlpha(200);
        p.setPen(Qt::NoPen);
        p.setBrush(bar_color);
        p.drawRoundedRect(bar, bar.height() / 2, bar.height() / 2);

        QColor ink = pal.text().color();
        ink.setAlpha(220);
        qreal x = bar.left() + 20;
        const qreal cy = bar.center().y();
        for (const Hint& hint : hints) {
            QFont glyph_font = font_hint;
            glyph_font.setPixelSize(std::max(8, static_cast<int>(radius * 1.2)));
            for (const QString& glyph : hint.glyphs) {
                LibraryStyleContext::PaintGlyph(&p, glyph, x, cy, radius, ink, 1.4, glyph_font);
                x += LibraryStyleContext::GlyphWidth(glyph, radius) + glyph_gap;
            }
            p.setFont(font_hint);
            p.setPen(ink);
            const int label_w = fm.horizontalAdvance(hint.label);
            p.drawText(QRectF(x, cy - 12, label_w + 2, 24), Qt::AlignLeft | Qt::AlignVCenter,
                       hint.label);
            x += label_w + item_gap;
        }
    }
}

} // namespace LibraryStyles
