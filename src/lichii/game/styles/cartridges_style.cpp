// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Concept by inxomnyaa.

#include <algorithm>
#include <mutex>
#include <cmath>
#include <utility>
#include <vector>

#include <QAbstractItemModel>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QImage>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPolygonF>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStringList>
#include <QTimer>
#include <QTransform>
#include <QWheelEvent>

#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/game_list_p.h"
#include "lichii/game/styles/cartridges_style.h"
#include "lichii/game/styles/style_helpers.h"

namespace LibraryStyles {

namespace {

constexpr qreal SideMargin = 18.0;
constexpr qreal TopMargin = 18.0;
constexpr qreal LiftAmount = 12.0;
constexpr qreal SelectedScale = 1.06;
constexpr qreal RingWidth = 3.0;
/// Name bubble metrics from the concept: 14 px below the cart's resting edge, 7 px arrow.
constexpr qreal BubbleGap = 14.0;
constexpr qreal BubbleArrow = 7.0;
constexpr int BubbleFontPx = 13;
/// P3: the stickers lean 8 degrees on the cart.
constexpr qreal StickerTilt = 8.0;

/// Bubbles shrink with small carts so names never take more room than the carts themselves.
qreal BubbleScale(qreal card_w) {
    return std::clamp(card_w / 160.0, 0.65, 1.0);
}

QFont BubbleFont(QFont font, qreal scale) {
    font.setPixelSize(std::max(9, static_cast<int>(std::lround(BubbleFontPx * scale))));
    font.setBold(true);
    return font;
}

/// How far the lifted, scaled selected cart and its ring reach above its resting top edge.
qreal SelectedHeadroom(qreal card_h) {
    return LiftAmount + card_h * (SelectedScale - 1.0) / 2 + RingWidth + 6.0;
}

quint64 Hash64(quint64 x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

bool IsGameType(GameListItemType type) {
    return type == GameListItemType::Game;
}

GameListItemType TypeOf(const QModelIndex& index) {
    return index.data(GameListItem::TypeRole).value<GameListItemType>();
}

bool IsDark(const QPalette& palette) {
    return palette.color(QPalette::Window).lightness() < 128;
}

/// Room around the card in its pixmap: the stickers stick out past the right edge.
qreal CardMargin(qreal card_w) {
    // At least 22 px: on small carts the stickers move further out past the right edge.
    return std::max(card_w * 0.2, 22.0);
}

QPainterPath BodyPath(qreal w, qreal h) {
    QPainterPath body;
    body.addRoundedRect(QRectF(0, 0, w, h), w * 0.085, w * 0.085);
    return body;
}

struct CardData {
    QPixmap cover;
    QString title;
    QString id_text;
    /// Short play time ("1.9 h") and version ("v1.3.0") stickers; empty = no sticker.
    QString play_time;
    QString version;
    /// A title of the newer generation: red body and a "2" next to the header glyph.
    bool is_nx2 = false;
    bool is_game = true;
};

/// A sticker: rounded tag with a soft shadow, drawn upright in the card's own coordinates, so it
/// tilts together with the card.
void DrawSticker(QPainter& p, const QRectF& rect, const QString& text, const QColor& fill,
                 const QFont& font) {
    const qreal r = rect.height() * 0.28;
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 70));
    p.drawRoundedRect(rect.translated(0, rect.height() * 0.12), r, r);
    p.setBrush(fill);
    p.drawRoundedRect(rect, r, r);
    p.setPen(QColor(0x3b, 0x2a, 0x12));
    p.setFont(font);
    p.drawText(rect, Qt::AlignCenter, text);
}

/// The cart, after the Cartridges concept: dark (or red) body with a slot on the top rim and an
/// arrow at the bottom, and a light label with a red header band, the square art and a dark strip
/// with the program id. Everything scales with the card width.
/// What every card of one size and generation shares: body, slot, arrow, and the empty label
/// with its header band and id strip. Drawn once per size instead of again for every card.
QPixmap CardTemplate(bool nx2, qreal w, qreal h, qreal dpr) {
    static std::mutex mutex;
    static QHash<QString, QPixmap> templates;
    const QString key = QStringLiteral("%1|%2|%3|%4").arg(nx2).arg(w).arg(h).arg(dpr);
    std::scoped_lock lock{mutex};
    if (const auto found = templates.constFind(key); found != templates.constEnd()) {
        return *found;
    }
    if (templates.size() >= 8) {
        templates.clear(); // sizes from an earlier icon size or screen
    }
    const qreal m = CardMargin(w);
    QPixmap pixmap(QSize(static_cast<int>(std::ceil((w + 2 * m) * dpr)),
                         static_cast<int>(std::ceil((h + 2 * m) * dpr))));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.translate(m, m);
    CardData d;
    d.is_nx2 = nx2;

    const QColor body_color = d.is_nx2 ? QColor(0xec, 0x2a, 0x35) : QColor(0x3a, 0x3a, 0x3f);
    const QColor ridge_color = d.is_nx2 ? QColor(0xc3, 0x14, 0x1f) : QColor(0x26, 0x26, 0x2a);

    // Body, with a darker lip along the bottom edge.
    const QPainterPath body = BodyPath(w, h);
    p.setPen(Qt::NoPen);
    p.setBrush(body_color.darker(135));
    p.drawPath(body);
    p.save();
    p.setClipPath(body);
    p.setBrush(body_color);
    p.drawRoundedRect(QRectF(0, 0, w, h - std::max(1.5, w * 0.014)), w * 0.085, w * 0.085);
    p.restore();

    // Label geometry: header band, square art, id strip.
    const qreal side_pad = w * 0.064;
    const qreal lw = w - 2 * side_pad;
    const qreal header_h = lw * 0.17;
    const qreal strip_h = lw * 0.15;
    const QRectF label(side_pad, w * 0.1, lw, header_h + lw + strip_h);
    const qreal label_r = std::max(2.0, w * 0.03);

    // Slot: the short darker ridge in the middle of the top rim, sitting on the label.
    p.setBrush(ridge_color);
    const QRectF slot(w * 0.34, w * 0.05, w * 0.32, label.top() - w * 0.05 + 1.0);
    p.drawRoundedRect(slot, w * 0.02, w * 0.02);
    p.fillRect(slot.adjusted(0, slot.height() / 2, 0, 0), ridge_color);

    // Arrow under the label.
    const qreal ax = w / 2;
    const qreal arrow_w = w * 0.114;
    const qreal arrow_h = w * 0.05;
    const qreal arrow_top = h - w * 0.03 - arrow_h;
    p.drawPolygon(QPolygonF{{ax - arrow_w / 2, arrow_top},
                            {ax + arrow_w / 2, arrow_top},
                            {ax, arrow_top + arrow_h}});

    QPainterPath label_path;
    label_path.addRoundedRect(label, label_r, label_r);
    p.save();
    p.setClipPath(label_path);
    p.fillRect(label, QColor(0xf2, 0xf2, 0xf2));

    // Header band with a neutral glyph (a rounded pill), "2" for the newer generation.
    const QRectF header(label.left(), label.top(), lw, header_h);
    p.fillRect(header, QColor(0xe6, 0x00, 0x12));
    {
        // The neutral glyph, as on the cases: a rounded square (no console logo).
        const qreal pill_h = header_h * 0.5;
        const qreal pill_w = pill_h;
        QFont two = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
        two.setBold(true);
        two.setPixelSize(std::max(6, static_cast<int>(header_h * 0.56)));
        const qreal two_w = d.is_nx2 ? QFontMetricsF(two).horizontalAdvance(QStringLiteral("2")) : 0;
        const qreal gap = d.is_nx2 ? header_h * 0.18 : 0;
        const qreal x0 = header.center().x() - (pill_w + gap + two_w) / 2;
        p.setPen(QPen(Qt::white, std::max(1.0, pill_h * 0.125)));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(x0, header.center().y() - pill_h / 2, pill_w, pill_h),
                          pill_h * 0.3, pill_h * 0.3);
        if (d.is_nx2) {
            p.setFont(two);
            p.setPen(Qt::white);
            p.drawText(QRectF(x0 + pill_w + gap, header.top(), two_w + 2, header_h),
                       Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("2"));
        }
    }

    // Id strip, without the id. It starts 1 px up, under the art: where the art and the strip
    // meet at a fraction of a pixel, the light label showed through as a thin line (most on
    // tilted cartridges). The art is drawn over that pixel, so the strip doesn't move.
    const qreal strip_top = header.bottom() + lw;
    p.fillRect(QRectF(label.left(), strip_top - 1.0, lw, label.bottom() - strip_top + 1.0),
               QColor(0x16, 0x16, 0x18));
    p.restore();

    p.end();
    templates.insert(key, pixmap);
    return pixmap;
}

QPixmap RenderCard(const CardData& d, qreal w, qreal h, qreal dpr) {
    QPixmap pixmap = CardTemplate(d.is_nx2, w, h, dpr);
    // detach: the template stays as it is
    pixmap = pixmap.copy();
    pixmap.setDevicePixelRatio(dpr);
    const qreal m = CardMargin(w);

    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.translate(m, m);

    // Label geometry: header band, square art, id strip.
    const qreal side_pad = w * 0.064;
    const qreal lw = w - 2 * side_pad;
    const qreal header_h = lw * 0.17;
    const qreal strip_h = lw * 0.15;
    const QRectF label(side_pad, w * 0.1, lw, header_h + lw + strip_h);
    const qreal label_r = std::max(2.0, w * 0.03);

    // Stickers: play time (yellow) and version (paler) under it, one tilt, poking out on the
    // right; drawn last, on top.
    QFont tag_font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    tag_font.setWeight(QFont::Black);
    tag_font.setPixelSize(std::max(7, static_cast<int>(w * 0.085)));
    const QFontMetricsF tag_metrics(tag_font);
    const qreal tag_h = tag_font.pixelSize() * 1.75;
    struct Sticker {
        QString text;
        QColor fill;
        QRectF rect;
    };
    std::vector<Sticker> stickers;
    {
        const std::pair<QString, QColor> tags[] = {{d.play_time, QColor(0xff, 0xd3, 0x6b)},
                                                   {d.version, QColor(0xff, 0xf3, 0xc9)}};
        const auto tag_width = [&](const QString& text) {
            return tag_metrics.horizontalAdvance(text) + tag_font.pixelSize() * 1.4;
        };
        // Right edges 7 % past the cart like the concept. The tag font has a floor, so on small
        // carts the stickers would cover the art: shift them out until they cover at most 26 %
        // of the cart, keeping their right edges aligned.
        qreal widest = 0.0;
        for (const auto& [text, fill] : tags) {
            if (!text.isEmpty()) {
                widest = std::max(widest, tag_width(text));
            }
        }
        const qreal right = w * 1.07 + std::max<qreal>(0.0, widest - w * 0.07 - w * 0.26);
        qreal tag_y = h * 0.17;
        for (const auto& [text, fill] : tags) {
            if (text.isEmpty()) {
                continue;
            }
            const qreal tw = tag_width(text);
            const QRectF rect(right - tw, tag_y, tw, tag_h);
            stickers.push_back({text, fill, rect});
            tag_y += tag_h + w * 0.035;
        }
    }

    QPainterPath label_path;
    label_path.addRoundedRect(label, label_r, label_r);
    p.save();
    p.setClipPath(label_path);
    const QRectF header(label.left(), label.top(), lw, header_h);

    // Square art, cropped to fill.
    const QRectF art(label.left(), header.bottom(), lw, lw);
    if (!d.cover.isNull()) {
        QSizeF scaled = QSizeF(d.cover.size()) / std::max(1.0, d.cover.devicePixelRatio());
        scaled.scale(art.size(), Qt::KeepAspectRatioByExpanding);
        const QRectF target(art.center().x() - scaled.width() / 2,
                            art.center().y() - scaled.height() / 2, scaled.width(),
                            scaled.height());
        p.save();
        p.setClipRect(art);
        p.drawPixmap(target, d.cover, QRectF(d.cover.rect()));
        p.restore();
    } else {
        p.fillRect(art, QColor(0x23, 0x24, 0x2a));
        QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
        font.setBold(true);
        font.setPixelSize(std::max(7, static_cast<int>(lw * 0.1)));
        // The title printed on the label (S4: two lines, word wrap only). It stays where it is
        // when the cartridge is selected: the stickers sit on top of it like real stickers.
        const QRectF text_box = art.adjusted(lw * 0.08, lw * 0.08, -lw * 0.08, -lw * 0.08);
        LibraryStyleContext::PaintName(&p, text_box.toRect(), d.title, QColor(0xed, 0xed, 0xee),
                                       font, Qt::AlignCenter, 2, 0);
    }

    // Id on the strip.
    const QRectF strip(label.left(), art.bottom(), lw, label.bottom() - art.bottom());
    if (!d.id_text.isEmpty() && strip.height() >= 2) {
        // Laid out at a normal size and scaled to the strip: a font size floor let the id run
        // past the strip on small cartridges (and hid it below 6 px).
        QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        mono.setLetterSpacing(QFont::PercentageSpacing, 108);
        mono.setPixelSize(20);
        const QFontMetricsF fm(mono);
        const qreal advance = std::max(1.0, fm.horizontalAdvance(d.id_text));
        const qreal k = std::min(strip.width() * 0.86 / advance, strip.height() * 0.5 / 20.0);
        p.save();
        p.translate(strip.center());
        p.scale(k, k);
        p.setFont(mono);
        p.setPen(QColor(0xed, 0xed, 0xee));
        p.drawText(QRectF(-advance / 2 - 4, -fm.height() / 2, advance + 8, fm.height()),
                   Qt::AlignCenter, d.id_text);
        p.restore();
    }
    p.restore();

    // The stickers, tilted with the cart (they are drawn in its own coordinates).
    for (const Sticker& sticker : stickers) {
        p.save();
        p.translate(sticker.rect.center());
        p.rotate(StickerTilt);
        p.translate(-sticker.rect.center());
        DrawSticker(p, sticker.rect, sticker.text, sticker.fill, tag_font);
        p.restore();
    }

    p.end();
    return pixmap;
}

/// The cartridge's data for a row: art (or the shared folder art), title, id, generation and,
/// with `stickers`, the play time and version the View > Game Info settings switch on.
CardData CartridgeData(const QModelIndex& index, const QPalette& pal, bool stickers) {
    const bool is_game = IsGameType(TypeOf(index));
    CardData data;
    data.is_game = is_game;
    data.title = is_game ? index.data(GameListItemPath::TitleRole).toString() : QString{};
    if (data.title.isEmpty()) {
        data.title = index.data(Qt::DisplayRole).toString().section(QLatin1Char('\n'), 0, 0);
    }
    if (!is_game) {
        // Add Folder and the like: the shared folder-with-plus art on the label.
        data.cover = LibraryStyleContext::Cover(index, QSize(256, 256), pal);
        return data;
    }
    data.cover = index.data(GameListItemPath::CardIconRole).value<QPixmap>();
    if (data.cover.isNull() && index.data(GameListItemPath::HasIconRole).toBool()) {
        data.cover = index.data(Qt::DecorationRole).value<QPixmap>();
    }
    const GameInfo game = LibraryStyleContext::Info(index);
    const quint64 program_id = index.data(GameListItemPath::ProgramIdRole).toULongLong();
    data.id_text = game.program_id_text;
    data.is_nx2 = game.is_nx2 || (program_id >> 56) == 0x04;
    if (stickers) {
        if (game.is_game && UISettings::values.show_play_time.GetValue()) { // "New" if unplayed
            data.play_time = game.play_time_text;
        }
        if (UISettings::values.show_add_ons.GetValue() && !game.update_version.isEmpty()) {
            data.version = QStringLiteral("v") + game.update_version;
        }
    }
    return data;
}

} // namespace

QPixmap RenderCartridge(const QModelIndex& index, qreal w, qreal dpr, const QPalette& palette) {
    return RenderCard(CartridgeData(index, palette, false), w, w * CartridgeAspect, dpr);
}

qreal CartridgeMargin(qreal w) {
    return CardMargin(w);
}

CartridgesStyle::CartridgesStyle(QWidget* parent)
    : LibraryStyleView{parent}, card_cache_{64 * 1024 * 1024}, tilted_cache_{64 * 1024 * 1024} {
    setObjectName(QStringLiteral("cartridgesStyleView"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent, true);

    bar_ = new QScrollBar(Qt::Vertical, this);
    connect(bar_, &QScrollBar::valueChanged, this, [this] { update(); });
    bar_->hide();
}

CartridgesStyle::~CartridgesStyle() = default;

void CartridgesStyle::SetModel(QAbstractItemModel* model) {
    for (const QMetaObject::Connection& connection : connections_) {
        disconnect(connection);
    }
    connections_.clear();
    model_ = model;
    current_ = QPersistentModelIndex{};
    current_row_ = -1;
    entries_.clear();
    card_cache_.clear();
    tilted_cache_.clear();
    if (model_) {
        // Structural changes rebuild the flat list (deferred, so a burst of inserts costs one
        // rebuild). Data changes only repaint: the card cache key covers cover, title and info.
        const auto structural = [this] { ScheduleRebuild(); };
        const auto repaint = [this] { update(); };
        connections_.push_back(connect(model_, &QAbstractItemModel::dataChanged, this, repaint));
        connections_.push_back(
            connect(model_, &QAbstractItemModel::rowsInserted, this, structural));
        connections_.push_back(connect(model_, &QAbstractItemModel::rowsRemoved, this, structural));
        connections_.push_back(connect(model_, &QAbstractItemModel::rowsMoved, this, structural));
        connections_.push_back(connect(model_, &QAbstractItemModel::modelReset, this, structural));
        connections_.push_back(
            connect(model_, &QAbstractItemModel::layoutChanged, this, structural));
    }
    Rebuild();
}

QModelIndex CartridgesStyle::CurrentIndex() const {
    return current_.isValid() ? QModelIndex(current_) : QModelIndex{};
}

QModelIndex CartridgesStyle::IndexAt(int row) const {
    if (row < 0 || row >= static_cast<int>(entries_.size())) {
        return {};
    }
    return QModelIndex(entries_[static_cast<size_t>(row)]);
}

void CartridgesStyle::ScheduleRebuild() {
    if (rebuild_pending_) {
        return;
    }
    rebuild_pending_ = true;
    QTimer::singleShot(0, this, [this] {
        if (rebuild_pending_) {
            Rebuild();
        }
    });
}

void CartridgesStyle::Rebuild() {
    rebuild_pending_ = false;
    const QPersistentModelIndex previous = current_;
    entries_.clear();
    // The games, flat and favorites first (shared by every style).
    for (const QModelIndex& game : LibraryStyleContext::Games(model_)) {
        entries_.emplace_back(game);
    }

    int new_row = -1;
    if (previous.isValid()) {
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i] == previous) {
                new_row = static_cast<int>(i);
                break;
            }
        }
    }
    if (new_row < 0 && !entries_.empty()) {
        // Keep the position after a removal; with nothing selected yet, start on the first cart
        // like the other styles (otherwise nothing is selected until a click).
        new_row = std::clamp(current_row_, 0, static_cast<int>(entries_.size()) - 1);
    }
    const QModelIndex before = previous;
    current_row_ = new_row;
    current_ = new_row >= 0 ? entries_[static_cast<size_t>(new_row)] : QPersistentModelIndex{};
    UpdateScrollRange();
    if (IndexAt(new_row) != before) {
        emit CurrentChanged(IndexAt(new_row));
    }
    update();
}

void CartridgesStyle::SetCurrentIndex(const QModelIndex& index) {
    if (!model_ || !index.isValid() || index.model() != model_) {
        return;
    }
    if (rebuild_pending_) {
        Rebuild();
    }
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i] == index) {
            SetCurrentRow(static_cast<int>(i), false);
            return;
        }
    }
}

void CartridgesStyle::SetShowNames(bool show) {
    show_names_ = show;
    UpdateScrollRange();
    update();
}

void CartridgesStyle::SetShowInfo(bool show) {
    show_info_ = show;
    update();
}

QString CartridgesStyle::CycleOption() {
    scattered_ = !scattered_;
    update();
    return scattered_ ? tr("Layout: scattered") : tr("Layout: tidy");
}

int CartridgesStyle::RowCount() const {
    return static_cast<int>(entries_.size());
}

CartridgesStyle::Metrics CartridgesStyle::ComputeMetrics() const {
    Metrics m;
    const qreal avail = std::max<qreal>(1.0, width() - 2 * SideMargin);
    // The square label art is View > Game Icon Size; the cart is sized around it. Columns get
    // the concept's 26 px gap plus room for the stickers on the right edge.
    m.card_w = LibraryStyleContext::ArtSize() / 0.872;
    m.card_w = std::min(m.card_w, (avail - 26.0) / 1.12);
    m.cols = std::max(1, static_cast<int>(avail / (m.card_w * 1.12 + 26.0)));
    m.cell_w = avail / m.cols;
    // Label (header + square art + id strip) plus the rims lands at the real ~21:30 cart shape.
    m.card_h = m.card_w * CartridgeAspect;
    // Concept row gap of 34 px. With names on, the gap holds a two-line bubble plus the lifted,
    // scaled selected cart of the next row, so neither covers the other.
    m.bubble_scale = BubbleScale(m.card_w);
    const qreal bs = m.bubble_scale;
    const qreal bubble_h =
        BubbleGap * bs + QFontMetrics(BubbleFont(font(), bs)).lineSpacing() * 2 + 16.0 * bs;
    m.cell_h = m.card_h + (show_names_ ? std::max(34.0, bubble_h + SelectedHeadroom(m.card_h) + 6.0)
                                       : 34.0);
    m.margin_x = SideMargin;
    m.margin_top = TopMargin + SelectedHeadroom(m.card_h);
    const int n = RowCount();
    m.rows = (n + m.cols - 1) / m.cols;
    return m;
}

CartridgesStyle::Placement CartridgesStyle::Place(int row, const Metrics& m, bool selected) const {
    Placement place;
    const int col = row % m.cols;
    const int r = row / m.cols;
    place.center = QPointF(m.margin_x + (col + 0.5) * m.cell_w,
                           m.margin_top + r * m.cell_h + m.card_h / 2);
    if (scattered_) {
        const QModelIndex index = IndexAt(row);
        quint64 key = index.data(GameListItemPath::ProgramIdRole).toULongLong();
        if (key == 0) {
            key = qHash(index.data(Qt::DisplayRole).toString());
        }
        const quint64 h = Hash64(key);
        place.angle = static_cast<qreal>(h % 1201) / 100.0 - 6.0;
        const qreal ux = static_cast<qreal>((h >> 16) % 1001) / 1000.0 - 0.5;
        const qreal uy = static_cast<qreal>((h >> 32) % 1001) / 1000.0 - 0.5;
        place.center += QPointF(ux * 2.0 * m.cell_w * 0.05, uy * 2.0 * m.cell_h * 0.03);
    }
    if (selected) {
        place.center.ry() -= LiftAmount;
    }
    place.transform.translate(place.center.x(), place.center.y());
    place.transform.rotate(place.angle);
    if (selected) {
        place.transform.scale(SelectedScale, SelectedScale);
    }
    place.transform.translate(-m.card_w / 2, -m.card_h / 2);
    return place;
}

int CartridgesStyle::RowAt(const QPoint& pos) const {
    const int n = RowCount();
    if (n == 0) {
        return -1;
    }
    const Metrics m = ComputeMetrics();
    const QPointF content(pos.x(), pos.y() + bar_->value());
    const auto hits = [&](int row, bool selected) {
        bool ok = false;
        const QTransform inverse = Place(row, m, selected).transform.inverted(&ok);
        return ok && QRectF(0, 0, m.card_w, m.card_h).contains(inverse.map(content));
    };
    if (current_row_ >= 0 && hits(current_row_, true)) {
        return current_row_;
    }
    const int col0 = static_cast<int>(std::floor((content.x() - m.margin_x) / m.cell_w));
    const int row0 = static_cast<int>(std::floor((content.y() - m.margin_top) / m.cell_h));
    int best = -1;
    for (int dr = -1; dr <= 1; ++dr) {
        for (int dc = -1; dc <= 1; ++dc) {
            const int c = col0 + dc;
            const int r = row0 + dr;
            if (c < 0 || c >= m.cols || r < 0) {
                continue;
            }
            const int row = r * m.cols + c;
            if (row < n && hits(row, false)) {
                // Later cards are painted on top, so prefer the last hit.
                best = row;
            }
        }
    }
    return best;
}

void CartridgesStyle::UpdateScrollRange() {
    if (!bar_) {
        return;
    }
    const Metrics m = ComputeMetrics();
    // The hint pill covers the bottom; the last row scrolls clear of it.
    const qreal content_h = m.margin_top * 2 + m.rows * m.cell_h + (show_names_ ? 70.0 : 0.0) +
                            LibraryStyleContext::HintPillReserve;
    const int range = std::max(0, static_cast<int>(std::ceil(content_h)) - height());
    bar_->setRange(0, range);
    bar_->setPageStep(std::max(1, height()));
    bar_->setSingleStep(std::max(1, static_cast<int>(m.cell_h / 3)));
    bar_->setVisible(range > 0);
    const int bar_w = bar_->sizeHint().width();
    bar_->setGeometry(width() - bar_w, 0, bar_w, height());
}

void CartridgesStyle::EnsureVisible(int row) {
    if (row < 0 || row >= RowCount()) {
        return;
    }
    const Metrics m = ComputeMetrics();
    // Keep the lifted cart's ring and its name bubble on screen.
    const qreal card_top = m.margin_top + (row / m.cols) * m.cell_h;
    const int top = static_cast<int>(card_top - SelectedHeadroom(m.card_h) - 6.0);
    const int bottom = static_cast<int>(card_top + m.card_h + BubbleGap +
                                        QFontMetrics(font()).lineSpacing() * 2 + 30.0);
    if (top < bar_->value()) {
        bar_->setValue(top);
    } else if (bottom > bar_->value() + height() - LibraryStyleContext::HintPillReserve) {
        bar_->setValue(bottom - height() + LibraryStyleContext::HintPillReserve);
    }
}

void CartridgesStyle::SetCurrentRow(int row, bool notify) {
    if (!model_ || row < 0 || row >= RowCount()) {
        return;
    }
    const QModelIndex index = IndexAt(row);
    if (!index.isValid()) {
        return;
    }
    const bool changed = current_row_ != row;
    current_ = QPersistentModelIndex(index);
    current_row_ = row;
    EnsureVisible(row);
    update();
    if (changed && notify) {
        emit CurrentChanged(index);
    }
}

void CartridgesStyle::Launch(const QModelIndex& index) {
    if (index.isValid()) {
        emit LaunchRequested(index);
    }
}

QPixmap CartridgesStyle::CardPixmap(const QModelIndex& index, const Metrics& m, qreal dpr,
                                    bool selected) const {
    const auto type = TypeOf(index);
    const bool is_game = IsGameType(type);
    // Like Dense Grid: Info shows the stickers on the selected cart only.
    const bool info = show_info_ && selected && is_game;
    const CardData card_data = CartridgeData(index, palette(), info);
    const QString key =
        QStringList{index.data(GameListItemPath::FullPathRole).toString(),
                    card_data.title,
                    QString::number(index.data(GameListItemPath::ProgramIdRole).toULongLong()),
                    QString::number(card_data.cover.cacheKey()),
                    QString::number(static_cast<int>(type)),
                    card_data.play_time + QLatin1Char('|') + card_data.version,
                    QString::number(static_cast<int>(m.card_w * 100)),
                    QString::number(static_cast<int>(dpr * 100))}
            .join(QLatin1Char('|'));
    if (QPixmap* cached = card_cache_.object(key)) {
        return *cached;
    }
    auto* pixmap = new QPixmap(RenderCard(card_data, m.card_w, m.card_h, dpr));
    const int cost = std::max(
        1, static_cast<int>(static_cast<qint64>(pixmap->width()) * pixmap->height() * 4));
    const QPixmap result = *pixmap;
    card_cache_.insert(key, pixmap, cost);
    return result;
}

void CartridgesStyle::DrawNameBubble(QPainter& painter, int row, const Metrics& m,
                                     const Placement& place) const {
    const QModelIndex index = IndexAt(row);
    if (!index.isValid()) {
        return;
    }
    const auto type = index.data(GameListItem::TypeRole).value<GameListItemType>();
    QString title = type == GameListItemType::Game
                        ? index.data(GameListItemPath::TitleRole).toString()
                        : QString{};
    if (title.isEmpty()) {
        title = index.data(Qt::DisplayRole).toString().section(QLatin1Char('\n'), 0, 0);
    }
    if (title.isEmpty()) {
        return;
    }
    const qreal bs = m.bubble_scale;
    const QFont bubble_font = BubbleFont(font(), bs);
    const QFontMetrics fm(bubble_font);
    const qreal pad_x = 12.0 * bs;
    const qreal pad_y = 8.0 * bs;
    const qreal corner = 12.0 * bs;
    // With every name showing, each bubble keeps to its own column so neighbours never overlap;
    // at most two lines, the rest is cut with "...", so the row gap always holds it.
    const int max_w = static_cast<int>(
        std::max(30.0, (show_names_ ? m.cell_w - 12.0 : std::max(190.0 * bs, m.cell_w)) -
                           2 * pad_x));
    // The bubble fits the S4 lines of the name: word wrap only, then "...".
    const int title_lines = LibraryStyleContext::TitleLines(2);
    const QStringList lines =
        LibraryStyleContext::NameLines(title, bubble_font, max_w, title_lines, pad_x);
    const bool glides = row == current_row_ && title_lines == 1 &&
                        fm.horizontalAdvance(title) > max_w + pad_x;
    int text_w = 0;
    for (const QString& line : lines) {
        text_w = std::max(text_w, fm.horizontalAdvance(line));
    }
    const qreal bw = (glides ? max_w : std::min(text_w, max_w)) + 2 * pad_x;
    const qreal bh = fm.lineSpacing() * std::max<qsizetype>(1, lines.size()) + 2 * pad_y;
    const qreal arrow = BubbleArrow * bs;
    // Anchored to the cart's resting bottom edge, so lifting the selected cart opens a gap.
    const qreal rest_bottom = m.margin_top + (row / m.cols) * m.cell_h + m.card_h;
    const qreal top = std::max(rest_bottom, place.center.y() + m.card_h * SelectedScale / 2) +
                      BubbleGap * bs - arrow;
    qreal left = place.center.x() - bw / 2;
    left = std::clamp(left, 6.0, std::max(6.0, width() - 6.0 - bw));
    const QRectF rect(left, top + arrow, bw, bh);

    painter.save();
    painter.setPen(Qt::NoPen);
    for (int i = 1; i <= 4; ++i) {
        painter.setBrush(QColor(0, 0, 0, 18));
        painter.drawRoundedRect(rect.adjusted(-i, -i + 5 * bs, i, i + 5 * bs), corner + i,
                                corner + i);
    }
    const QColor bubble(255, 255, 255, 235);
    painter.setBrush(bubble);
    painter.drawRoundedRect(rect, corner, corner);
    const qreal cx =
        std::clamp(place.center.x(), rect.left() + corner + 2, rect.right() - corner - 2);
    QPolygonF tip;
    tip << QPointF(cx - arrow, rect.top() + 0.75) << QPointF(cx, rect.top() - arrow)
        << QPointF(cx + arrow, rect.top() + 0.75);
    painter.setPen(Qt::NoPen);
    painter.setBrush(bubble);
    painter.drawPolygon(tip);
    // The selected cart's long name glides with Scroll Long Names.
    LibraryStyleContext::PaintTitle(
        &painter, rect.adjusted(pad_x, pad_y, -pad_x, -pad_y).toAlignedRect(), title,
        QColor(0x16, 0x14, 0x1c), bubble_font, Qt::AlignHCenter | Qt::AlignVCenter, title_lines,
        row == current_row_ ? this : nullptr, static_cast<int>(pad_x));
    painter.restore();
}

void CartridgesStyle::paintEvent([[maybe_unused]] QPaintEvent* event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const qreal dpr = devicePixelRatioF();
    const int scroll = bar_->value();

    painter.drawPixmap(QPointF(0, 0),
                       LibraryStyleContext::WoodTable(size(), dpr, IsDark(palette())));

    const int n = RowCount();
    if (n == 0) {
        return;
    }
    const Metrics m = ComputeMetrics();
    const qreal margin = CardMargin(m.card_w);
    const int first_row =
        std::max(0, static_cast<int>(std::floor((scroll - m.margin_top) / m.cell_h)) - 1);
    const int last_row = std::min(
        m.rows - 1, static_cast<int>(std::floor((scroll + height() - m.margin_top) / m.cell_h)) + 1);
    const int selected = current_row_;

    painter.translate(0, -scroll);
    const auto draw_card = [&](int row, bool is_selected) {
        const QModelIndex index = IndexAt(row);
        if (!index.isValid()) {
            return;
        }
        const QPixmap pixmap = CardPixmap(index, m, dpr, is_selected);
        const Placement place = Place(row, m, is_selected);
        if (!is_selected && place.angle != 0.0) {
            // Around the cartridge's centre: its shadow and image, turned by its angle.
            QTransform local;
            local.rotate(place.angle);
            local.translate(-m.card_w / 2, -m.card_h / 2);
            const QRectF source(-margin, -margin, m.card_w + 2 * margin,
                                m.card_h + 2 * margin + 3);
            const QRectF bounds = local.mapRect(source).adjusted(-2, -2, 2, 2);
            const QString key = QStringLiteral("%1|%2|%3")
                                    .arg(pixmap.cacheKey())
                                    .arg(place.angle)
                                    .arg(dpr);
            QPixmap* tilted = tilted_cache_.object(key);
            if (tilted == nullptr) {
                QPixmap pm(QSize(static_cast<int>(std::ceil(bounds.width() * dpr)),
                                 static_cast<int>(std::ceil(bounds.height() * dpr))));
                pm.setDevicePixelRatio(dpr);
                pm.fill(Qt::transparent);
                QPainter tp(&pm);
                tp.setRenderHint(QPainter::Antialiasing, true);
                tp.setRenderHint(QPainter::SmoothPixmapTransform, true);
                tp.translate(-bounds.topLeft());
                tp.setTransform(local, true);
                tp.setPen(Qt::NoPen);
                tp.setBrush(QColor(0, 0, 0, 45));
                tp.drawRoundedRect(QRectF(0, 3, m.card_w, m.card_h), m.card_w * 0.085,
                                   m.card_w * 0.085);
                tp.drawPixmap(QPointF(-margin, -margin), pixmap);
                tp.end();
                tilted = new QPixmap(pm);
                tilted_cache_.insert(key, tilted,
                                     std::max(1, static_cast<int>(pm.width() * pm.height() * 4)));
            }
            painter.drawPixmap(QPointF(std::round(place.center.x() + bounds.left()),
                                       std::round(place.center.y() + bounds.top())),
                               *tilted);
            if (LibraryStyleContext::IsFavorite(index)) {
                // Over the cached tilted cartridge, turned the same way (not baked in it, so a
                // favorite toggle only repaints).
                painter.save();
                painter.setTransform(place.transform, true);
                LibraryStyleContext::PaintStickerHeart(&painter, QRectF(0, 0, m.card_w, m.card_h));
                painter.restore();
            }
            return;
        }
        painter.save();
        painter.setTransform(place.transform, true);
        painter.setPen(Qt::NoPen);
        if (is_selected) {
            for (int i = 0; i < 6; ++i) {
                painter.setBrush(QColor(0, 0, 0, 20));
                painter.drawRoundedRect(QRectF(-i, -i + 12, m.card_w + 2 * i, m.card_h + 2 * i),
                                        m.card_w * 0.085 + i, m.card_w * 0.085 + i);
            }
        } else {
            painter.setBrush(QColor(0, 0, 0, 45));
            painter.drawRoundedRect(QRectF(0, 3, m.card_w, m.card_h), m.card_w * 0.085,
                                    m.card_w * 0.085);
        }
        if (is_selected) {
            QColor ring = LibraryStyleContext::Accent(palette());
            if (!hasFocus()) {
                ring.setAlpha(170);
            }
            // Ring hugging the body from outside, concentric with its corners.
            const qreal grow = RingWidth / 2;
            painter.setPen(QPen(ring, RingWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(QRectF(-grow, -grow, m.card_w + 2 * grow, m.card_h + 2 * grow),
                                    m.card_w * 0.085 + grow, m.card_w * 0.085 + grow);
        }
        // Drawn after the ring: the stickers stick out past the body and sit on top of it.
        painter.drawPixmap(QPointF(-margin, -margin), pixmap);
        if (LibraryStyleContext::IsFavorite(index)) {
            LibraryStyleContext::PaintStickerHeart(&painter, QRectF(0, 0, m.card_w, m.card_h));
        }
        painter.restore();
    };

    const int end_row = std::min(n, (last_row + 1) * m.cols);
    for (int row = first_row * m.cols; row < end_row; ++row) {
        if (row != selected) {
            draw_card(row, false);
        }
    }
    if (show_names_) {
        for (int row = first_row * m.cols; row < end_row; ++row) {
            if (row != selected) {
                DrawNameBubble(painter, row, m, Place(row, m, false));
            }
        }
    }
    if (selected >= 0 && selected < n) {
        draw_card(selected, true);
        // The selected cart always names itself.
        DrawNameBubble(painter, selected, m, Place(selected, m, true));
    }
    // In view coordinates: the cartridges above are drawn scrolled.
    painter.translate(0, scroll);
    LibraryStyleContext::PaintHintPill(&painter, rect(),
                                       LibraryStyleContext::StandardHints(tr("Layout")),
                                       IsDark(palette()));
}

void CartridgesStyle::resizeEvent(QResizeEvent* event) {
    LibraryStyleView::resizeEvent(event);
    UpdateScrollRange();
}

void CartridgesStyle::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    if (event->button() == Qt::LeftButton) {
        const int row = RowAt(event->position().toPoint());
        if (row >= 0) {
            SetCurrentRow(row, true);
        }
        event->accept();
        return;
    }
    LibraryStyleView::mousePressEvent(event);
}

void CartridgesStyle::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const int row = RowAt(event->position().toPoint());
        if (row >= 0) {
            SetCurrentRow(row, true);
            Launch(IndexAt(row));
        }
        event->accept();
        return;
    }
    LibraryStyleView::mouseDoubleClickEvent(event);
}

void CartridgesStyle::keyPressEvent(QKeyEvent* event) {
    const int n = RowCount();
    if (n == 0 ||
        (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        LibraryStyleView::keyPressEvent(event);
        return;
    }
    const Metrics m = ComputeMetrics();
    const int row = current_row_;
    int target = row;
    switch (event->key()) {
    case Qt::Key_Left:
        target = row < 0 ? 0 : row - 1;
        break;
    case Qt::Key_Right:
        target = row < 0 ? 0 : row + 1;
        break;
    case Qt::Key_Up:
        target = row < 0 ? 0 : row - m.cols;
        break;
    case Qt::Key_Down:
        target = row < 0 ? 0 : row + m.cols;
        // Moving down from a full row into a short last row lands on its last game.
        if (row >= 0 && target >= n && row / m.cols < (n - 1) / m.cols) {
            target = n - 1;
        }
        break;
    case Qt::Key_Home:
        target = 0;
        break;
    case Qt::Key_End:
        target = n - 1;
        break;
    case Qt::Key_PageUp:
        target = std::max(0, std::max(row, 0) - m.cols * 2);
        break;
    case Qt::Key_PageDown:
        target = std::min(n - 1, std::max(row, 0) + m.cols * 2);
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (row >= 0) {
            Launch(IndexAt(row));
            event->accept();
        } else {
            event->ignore();
        }
        return;
    default:
        LibraryStyleView::keyPressEvent(event);
        return;
    }
    if (target >= 0 && target < n) {
        SetCurrentRow(target, true);
    }
    event->accept();
}

void CartridgesStyle::wheelEvent(QWheelEvent* event) {
    int delta = 0;
    if (!event->pixelDelta().isNull()) {
        delta = event->pixelDelta().y();
    } else {
        // Accumulate so small touchpad / high resolution wheel steps are not truncated to zero.
        wheel_remainder_ += event->angleDelta().y() / 120.0 * ComputeMetrics().cell_h * 0.5;
        delta = static_cast<int>(wheel_remainder_);
        wheel_remainder_ -= delta;
    }
    if (delta != 0) {
        bar_->setValue(bar_->value() - delta);
    }
    event->accept();
}

void CartridgesStyle::contextMenuEvent(QContextMenuEvent* event) {
    int row = -1;
    QPoint global = event->globalPos();
    if (event->reason() == QContextMenuEvent::Keyboard) {
        row = current_row_;
        if (row >= 0) {
            const QPointF c = Place(row, ComputeMetrics(), true).center;
            global = mapToGlobal(
                QPoint(static_cast<int>(c.x()), static_cast<int>(c.y()) - bar_->value()));
        }
    } else {
        row = RowAt(event->pos());
    }
    event->accept();
    if (row < 0) {
        if (event->reason() != QContextMenuEvent::Keyboard) {
            emit ContextMenuRequested(QModelIndex{}, global);
        }
        return;
    }
    SetCurrentRow(row, true);
    emit ContextMenuRequested(IndexAt(row), global);
    event->accept();
}

void CartridgesStyle::focusInEvent(QFocusEvent* event) {
    LibraryStyleView::focusInEvent(event);
    update();
}

void CartridgesStyle::focusOutEvent(QFocusEvent* event) {
    LibraryStyleView::focusOutEvent(event);
    update();
}

namespace {

LibraryStyleView* Make(QWidget* parent) {
    return new CartridgesStyle(parent);
}

[[maybe_unused]] const bool registered =
    LibraryStyles::Register(
        {"cartridges", QT_TRANSLATE_NOOP("LibraryStyles", "Cartridges"), &Make});

} // namespace

} // namespace LibraryStyles
