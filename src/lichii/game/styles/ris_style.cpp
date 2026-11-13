// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// RIS (Red Industries Society, PantACRO4life) branded showcase.

#include <algorithm>
#include <cmath>

#include <QAbstractItemModel>
#include <QContextMenuEvent>
#include <QEasingCurve>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QPolygon>
#include <QRadialGradient>
#include <QStringList>
#include <QTimer>
#include <QVariantAnimation>
#include <QWheelEvent>

#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/game_list_p.h"
#include "lichii/game/styles/ris_style.h"
#include "lichii/game/styles/style_helpers.h"

namespace LibraryStyles {

namespace {

constexpr int Margin = 20;
constexpr int Gap = 14;
constexpr int HeaderHeight = 68;
constexpr int FooterHeight = 34;
constexpr int Hazard = 4;
constexpr int Lift = 10;
constexpr int ShelfLabel = 26;
constexpr int Radius = 3;

bool IsFolder(GameListItemType type) {
    return type == GameListItemType::CustomDir || type == GameListItemType::SdmcDir ||
           type == GameListItemType::UserNandDir || type == GameListItemType::SysNandDir;
}

/// Chakra Petch ships with the RIS theme resources; the style uses it with any theme. It has
/// no CJK glyphs: every font set from it falls back to system CJK fonts (titles in Japanese).
QString LoadDisplayFamily() {
    static const QString family = [] {
        const int id =
            QFontDatabase::addApplicationFont(QStringLiteral(":/ris/fonts/ChakraPetch-Bold.ttf"));
        QFontDatabase::addApplicationFont(QStringLiteral(":/ris/fonts/ChakraPetch-SemiBold.ttf"));
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        return families.isEmpty() ? QString{} : families.first();
    }();
    return family;
}

QFont MonoFont(int pixel_size) {
    QFont font(QStringLiteral("Consolas"));
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(pixel_size);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
    return font;
}

/// Red and dark diagonal stripes, the hazard band of the RIS site and boot screens.
void PaintHazard(QPainter& painter, const QRect& band, const QColor& stripe, const QColor& base) {
    painter.save();
    painter.fillRect(band, base);
    painter.setClipRect(band);
    painter.setPen(Qt::NoPen);
    painter.setBrush(stripe);
    const int step = band.height() * 3;
    for (int x = band.left() - step; x < band.right() + step; x += step) {
        const QPolygon slant({QPoint(x, band.bottom() + 1), QPoint(x + band.height(), band.top()),
                              QPoint(x + band.height() * 2, band.top()),
                              QPoint(x + band.height(), band.bottom() + 1)});
        painter.drawPolygon(slant);
    }
    painter.restore();
}

/// HUD corner brackets just outside `rect`.
void PaintBrackets(QPainter& painter, const QRect& rect, const QColor& color, int length,
                   int distance) {
    painter.save();
    QPen pen(color, 2);
    pen.setCapStyle(Qt::SquareCap);
    painter.setPen(pen);
    const QRect r = rect.adjusted(-distance, -distance, distance, distance);
    const auto corner = [&](QPoint p, int dx, int dy) {
        painter.drawLine(p, p + QPoint(dx * length, 0));
        painter.drawLine(p, p + QPoint(0, dy * length));
    };
    corner(r.topLeft(), 1, 1);
    corner(r.topRight(), -1, 1);
    corner(r.bottomLeft(), 1, -1);
    corner(r.bottomRight(), -1, -1);
    painter.restore();
}

LibraryStyleView* Make(QWidget* parent) {
    return new RisStyle(parent);
}

} // namespace

[[maybe_unused]] static const bool registered =
    LibraryStyles::Register({"ris", QT_TRANSLATE_NOOP("LibraryStyles", "RIS"), &Make});

RisStyle::RisStyle(QWidget* parent) : LibraryStyleView{parent} {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    display_family = LoadDisplayFamily();

    scroll_animation = new QVariantAnimation(this);
    scroll_animation->setDuration(200);
    scroll_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(scroll_animation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                offset = value.toReal();
                update();
            });
}

void RisStyle::SetModel(QAbstractItemModel* new_model) {
    if (model) {
        disconnect(model, nullptr, this, nullptr);
    }
    model = new_model;
    if (model) {
        // Population adds rows one by one: rebuild once per event loop pass.
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

QModelIndex RisStyle::CurrentIndex() const {
    if (current < 0 || current >= entries.size()) {
        return {};
    }
    return entries[current];
}

void RisStyle::SetCurrentIndex(const QModelIndex& index) {
    // A rebuild may still be queued after the model changed: run it first.
    if (rebuild_pending) {
        Rebuild();
    }
    const QPersistentModelIndex wanted{index.siblingAtColumn(0)};
    for (int i = 0; i < entries.size(); ++i) {
        if (entries[i] == wanted) {
            Select(i);
            ScrollShelfTo(i, false);
            return;
        }
    }
}

void RisStyle::SetShowNames(bool show) {
    show_names = show;
    Relayout();
}

void RisStyle::SetShowInfo(bool show) {
    show_info = show;
    update();
}

QString RisStyle::CycleOption() {
    large_shelf = !large_shelf;
    Relayout();
    return large_shelf ? tr("Shelf: large covers") : tr("Shelf: compact covers");
}

void RisStyle::Rebuild() {
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

RisStyle::Colors RisStyle::Scheme() const {
    Colors colors;
    if (UISettings::IsRisTheme()) {
        colors.background = QColor(0x0f, 0x11, 0x14);
        colors.header = QColor(0x0a, 0x0b, 0x0d);
        colors.panel = QColor(0x14, 0x17, 0x1b);
        colors.text = QColor(0xf2, 0xf4, 0xf7);
        colors.muted = QColor(0x8b, 0x93, 0x9f);
        colors.faint = QColor(0x2a, 0x2f, 0x37);
        colors.accent = QColor(0xe0, 0x1b, 0x31);
        colors.accent_hot = QColor(0xff, 0x33, 0x47);
        return colors;
    }
    const QPalette& pal = palette();
    const bool dark = LibraryStyleContext::IsDark(pal);
    colors.background = pal.window().color();
    colors.header = dark ? colors.background.darker(135) : colors.background.darker(108);
    colors.panel = LibraryStyleContext::Surface(pal);
    colors.text = pal.text().color();
    colors.muted = LibraryStyleContext::MutedText(pal);
    colors.faint = colors.text;
    colors.faint.setAlpha(dark ? 46 : 38);
    colors.accent = LibraryStyleContext::Accent(pal);
    colors.accent_hot = dark ? colors.accent.lighter(125) : colors.accent.darker(115);
    return colors;
}

int RisStyle::TileSize() const {
    // Large shelf: the icon size every style uses; small shelf: three quarters of it.
    const int art = LibraryStyleContext::ArtSize();
    return large_shelf ? art : art * 3 / 4;
}

int RisStyle::NameHeight() const {
    return show_names ? QFontMetrics(font()).lineSpacing() + 8 : 0;
}

QRect RisStyle::HeaderRect() const {
    return QRect(0, 0, width(), HeaderHeight);
}

QRect RisStyle::FooterRect() const {
    return QRect(0, height() - FooterHeight, width(), FooterHeight);
}

QRect RisStyle::ShelfRect() const {
    const int shelf_height = ShelfLabel + Lift + TileSize() + NameHeight() + 14;
    return QRect(0, FooterRect().top() - shelf_height, width(), shelf_height);
}

QRect RisStyle::HeroRect() const {
    const int top = HeaderRect().bottom() + 1;
    return QRect(0, top, width(), std::max(0, ShelfRect().top() - top));
}

QRect RisStyle::HeroCoverRect() const {
    const QRect hero = HeroRect();
    const int side =
        std::max(0, std::min(hero.height() - Margin * 2, static_cast<int>(hero.width() * 0.38)));
    return QRect(hero.left() + Margin * 2, hero.top() + (hero.height() - side) / 2, side, side);
}

QRect RisStyle::TileRect(int position) const {
    const QRect shelf = ShelfRect();
    const int tile = TileSize();
    const int x = Margin + position * (tile + Gap) - static_cast<int>(std::lround(offset));
    const int y = shelf.top() + ShelfLabel + Lift - (position == current ? Lift : 0);
    return QRect(x, y, tile, tile);
}

qreal RisStyle::MaxOffset() const {
    const int tile = TileSize();
    const int content = Margin * 2 + static_cast<int>(entries.size()) * (tile + Gap) - Gap;
    return std::max(0, content - width());
}

int RisStyle::VisibleTiles() const {
    return std::max(1, (width() - Margin * 2 + Gap) / (TileSize() + Gap));
}

void RisStyle::ScrollShelfTo(int position, bool animate) {
    if (position < 0 || position >= entries.size()) {
        return;
    }
    const int tile = TileSize();
    const qreal center = Margin + position * (tile + Gap) + tile / 2.0;
    const qreal target = std::clamp(center - width() / 2.0, 0.0, MaxOffset());
    scroll_animation->stop();
    if (!animate || !isVisible()) {
        offset = target;
        update();
        return;
    }
    scroll_animation->setStartValue(offset);
    scroll_animation->setEndValue(target);
    scroll_animation->start();
}

void RisStyle::Relayout() {
    offset = std::clamp(offset, 0.0, MaxOffset());
    ScrollShelfTo(current, false);
    update();
}

int RisStyle::PositionAt(const QPoint& point) const {
    const QRect shelf = ShelfRect();
    if (!shelf.contains(point)) {
        return -1;
    }
    for (int i = 0; i < entries.size(); ++i) {
        QRect cell = TileRect(i);
        cell.setTop(shelf.top() + ShelfLabel);
        cell.setHeight(Lift + TileSize() + NameHeight());
        if (cell.contains(point)) {
            return i;
        }
    }
    return -1;
}

void RisStyle::Select(int position) {
    if (entries.isEmpty()) {
        return;
    }
    position = std::clamp(position, 0, static_cast<int>(entries.size()) - 1);
    if (position == current) {
        return;
    }
    current = position;
    ScrollShelfTo(current, true);
    update();
    emit CurrentChanged(CurrentIndex());
}

QPixmap RisStyle::CachedCover(const QModelIndex& index, const QSize& size) const {
    // Keyed by the path and the art itself: Info() reads a dozen roles, too much per tile and frame.
    const qint64 art =
        index.data(GameListItemPath::CardIconRole).value<QPixmap>().cacheKey();
    const QString key = QStringLiteral("ris_cover:%1:%2:%3x%4:%5:%6")
                            .arg(index.data(GameListItemPath::FullPathRole).toString())
                            .arg(art)
                            .arg(size.width())
                            .arg(size.height())
                            .arg(palette().window().color().rgb())
                            .arg(palette().highlight().color().rgb());
    QPixmap pixmap;
    if (!QPixmapCache::find(key, &pixmap)) {
        pixmap = LibraryStyleContext::Cover(index, size, palette());
        QPixmapCache::insert(key, pixmap);
    }
    return pixmap;
}

void RisStyle::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const Colors colors = Scheme();
    painter.fillRect(rect(), colors.background);
    PaintHero(painter, colors);
    PaintShelf(painter, colors);
    PaintHeader(painter, colors);
    PaintFooter(painter, colors);
}

void RisStyle::PaintHeader(QPainter& painter, const Colors& colors) {
    const QRect header = HeaderRect();
    painter.fillRect(header, colors.header);
    PaintHazard(painter, QRect(header.left(), header.bottom() - Hazard + 1, header.width(), Hazard),
                colors.accent, colors.header);

    const int x = Margin;
    const int content_height = header.height() - Hazard;

    QFont display = font();
    if (!display_family.isEmpty()) {
        LibraryStyleContext::SetFamilyWithFallbacks(display, display_family);
    }
    display.setPixelSize(17);
    display.setBold(true);
    display.setLetterSpacing(QFont::AbsoluteSpacing, 2.5);
    const QFontMetrics display_metrics(display);
    const QString red = QStringLiteral("RED INDUSTRIES ");
    const QString society = QStringLiteral("SOCIETY");
    const int baseline = content_height / 2 + 2;
    painter.setFont(display);
    painter.setPen(colors.text);
    painter.drawText(QPoint(x, baseline), red);
    painter.setPen(colors.accent);
    painter.drawText(QPoint(x + display_metrics.horizontalAdvance(red), baseline), society);

    const QFont tagline = MonoFont(12);
    painter.setFont(tagline);
    painter.setPen(colors.muted);
    painter.drawText(QPoint(x + 1, baseline + QFontMetrics(tagline).lineSpacing() + 2),
                     QStringLiteral("// PantACRO4life"));

    const int games = static_cast<int>(entries.size());
    const QFont count_font = MonoFont(12);
    painter.setFont(count_font);
    painter.setPen(colors.muted);
    painter.drawText(QRect(0, 0, header.width() - Margin, content_height),
                     Qt::AlignRight | Qt::AlignVCenter, tr("%n game(s)", "", games).toUpper());
}

void RisStyle::PaintHero(QPainter& painter, const Colors& colors) {
    const QRect hero = HeroRect();
    play_rect = {};
    if (hero.height() < 60) {
        return;
    }
    painter.save();
    painter.setClipRect(hero);

    // Blueprint grid and a red glow behind the cover: the same for every selection, so they are
    // rendered once per size and colours.
    const QRect cover = HeroCoverRect();
    const qreal dpr = devicePixelRatioF();
    const QString key = QStringLiteral("%1x%2|%3|%4|%5|%6")
                            .arg(hero.width())
                            .arg(hero.height())
                            .arg(dpr)
                            .arg(colors.background.rgba())
                            .arg(colors.text.rgba())
                            .arg(colors.accent.rgba());
    if (hero_key != key) {
        hero_key = key;
        hero_background = QPixmap(QSize(std::max(1, static_cast<int>(std::ceil(hero.width() * dpr))),
                                        std::max(1, static_cast<int>(std::ceil(hero.height() * dpr)))));
        hero_background.setDevicePixelRatio(dpr);
        hero_background.fill(colors.background);
        QPainter p(&hero_background);
        p.translate(0, -hero.top());
        QColor grid = colors.text;
        grid.setAlpha(LibraryStyleContext::IsDark(palette()) || UISettings::IsRisTheme() ? 9 : 14);
        p.setPen(QPen(grid, 1));
        for (int gx = 0; gx < hero.width(); gx += 32) {
            p.drawLine(gx, hero.top(), gx, hero.bottom());
        }
        for (int gy = hero.top(); gy < hero.bottom(); gy += 32) {
            p.drawLine(0, gy, hero.width(), gy);
        }
        QColor glow = colors.accent;
        glow.setAlpha(70);
        QRadialGradient radial(cover.center(), std::max(1, cover.width()));
        radial.setColorAt(0.0, glow);
        glow.setAlpha(0);
        radial.setColorAt(1.0, glow);
        p.fillRect(hero, radial);
    }
    painter.drawPixmap(hero.topLeft(), hero_background);

    const QModelIndex selected = CurrentIndex();
    if (!selected.isValid()) {
        QFont empty = font();
        if (!display_family.isEmpty()) {
            LibraryStyleContext::SetFamilyWithFallbacks(empty, display_family);
        }
        empty.setPixelSize(22);
        empty.setBold(true);
        painter.setFont(empty);
        painter.setPen(colors.muted);
        painter.drawText(hero, Qt::AlignCenter, tr("No games yet"));
        painter.restore();
        return;
    }
    const GameInfo info = LibraryStyleContext::Info(selected);

    // Cover with a thin accent frame and HUD brackets.
    if (cover.width() > 0) {
        painter.save();
        QPainterPath clip;
        clip.addRoundedRect(cover, Radius, Radius);
        painter.setClipPath(clip, Qt::IntersectClip);
        painter.drawPixmap(cover,
                           CachedCover(selected, cover.size() * devicePixelRatioF()));
        painter.restore();
        painter.setPen(QPen(colors.accent, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(cover).adjusted(-1, -1, 1, 1), Radius, Radius);
        PaintBrackets(painter, cover, colors.accent_hot, std::max(10, cover.width() / 10), 8);
    }

    // Text column.
    const int left = cover.width() > 0 ? cover.right() + Margin * 2 : hero.left() + Margin * 2;
    const int right = hero.right() - Margin * 2;
    const int column = std::max(0, right - left);
    int y = hero.top() + std::max(Margin, (hero.height() - std::min(hero.height(), 300)) / 2);

    const QFont kicker = MonoFont(12);
    painter.setFont(kicker);
    painter.setPen(colors.accent_hot);
    const QString kicker_text = tr("SELECTED  %1 / %2")
                                    .arg(current + 1, 2, 10, QLatin1Char('0'))
                                    .arg(entries.size(), 2, 10, QLatin1Char('0'));
    painter.drawText(QRect(left, y, column, QFontMetrics(kicker).lineSpacing()),
                     Qt::AlignLeft | Qt::AlignVCenter, kicker_text);
    y += QFontMetrics(kicker).lineSpacing() + 8;

    QFont title = font();
    if (!display_family.isEmpty()) {
        LibraryStyleContext::SetFamilyWithFallbacks(title, display_family);
    }
    title.setPixelSize(std::clamp(hero.height() / 8, 20, 40));
    title.setBold(true);
    const QFontMetrics title_metrics(title);
    // S4: two lines at most, one gliding line with Scroll Long Names.
    const int title_lines = LibraryStyleContext::TitleLines(2);
    const int used_lines = static_cast<int>(
        LibraryStyleContext::NameLines(info.title, title, column, title_lines).size());
    const QRect title_rect(left, y, column, title_metrics.lineSpacing() * title_lines);
    LibraryStyleContext::PaintTitle(&painter, title_rect, info.title, colors.text, title,
                                    Qt::AlignLeft | Qt::AlignTop, title_lines, this);
    y += title_metrics.height() +
         title_metrics.lineSpacing() * (std::max(1, used_lines) - 1) + 12;

    painter.fillRect(QRect(left, y, 56, 3), colors.accent);
    y += 3 + 16;

    // Stats: label over value, side by side.
    struct Stat {
        QString label;
        QString value;
    };
    QList<Stat> stats;
    stats.append({tr("PLAY TIME"), info.play_time_text});
    stats.append({tr("VERSION"), info.update_version.isEmpty()
                                     ? tr("Base")
                                     : QStringLiteral("v") + info.update_version});
    if (show_info) {
        stats.append({tr("FORMAT"), info.file_type});
        stats.append({tr("SIZE"), info.size_text});
        stats.append({tr("TITLE ID"), info.program_id_text});
    }
    const QFont label_font = MonoFont(11);
    QFont value_font = font();
    if (!display_family.isEmpty()) {
        LibraryStyleContext::SetFamilyWithFallbacks(value_font, display_family);
    }
    value_font.setPixelSize(17);
    value_font.setWeight(QFont::DemiBold);
    const QFontMetrics label_metrics(label_font);
    const QFontMetrics value_metrics(value_font);
    int sx = left;
    int sy = y;
    for (const Stat& stat : stats) {
        const int w = std::max(label_metrics.horizontalAdvance(stat.label),
                               value_metrics.horizontalAdvance(stat.value)) +
                      28;
        if (sx > left && sx + w > right) {
            sx = left;
            sy += label_metrics.lineSpacing() + value_metrics.lineSpacing() + 12;
        }
        painter.fillRect(QRect(sx, sy, 2, label_metrics.lineSpacing() + value_metrics.lineSpacing()),
                         colors.faint);
        painter.setFont(label_font);
        painter.setPen(colors.muted);
        painter.drawText(QRect(sx + 10, sy, w, label_metrics.lineSpacing()),
                         Qt::AlignLeft | Qt::AlignVCenter, stat.label);
        painter.setFont(value_font);
        painter.setPen(colors.text);
        painter.drawText(
            QRect(sx + 10, sy + label_metrics.lineSpacing(), w, value_metrics.lineSpacing()),
            Qt::AlignLeft | Qt::AlignVCenter, stat.value);
        sx += w + 8;
    }
    y = sy + label_metrics.lineSpacing() + value_metrics.lineSpacing() + 22;

    // Play plate.
    if (y + 40 < hero.bottom()) {
        QFont plate_font = font();
        if (!display_family.isEmpty()) {
            LibraryStyleContext::SetFamilyWithFallbacks(plate_font, display_family);
        }
        plate_font.setPixelSize(15);
        plate_font.setBold(true);
        plate_font.setLetterSpacing(QFont::AbsoluteSpacing, 2);
        const QString plate_text = tr("A  PLAY");
        const int plate_width = QFontMetrics(plate_font).horizontalAdvance(plate_text) + 44;
        play_rect = QRect(left, y, plate_width, 38);
        QPainterPath plate;
        // Clipped corner, like the RIS site buttons.
        plate.moveTo(play_rect.left(), play_rect.top());
        plate.lineTo(play_rect.right() - 10, play_rect.top());
        plate.lineTo(play_rect.right() + 1, play_rect.top() + 11);
        plate.lineTo(play_rect.right() + 1, play_rect.bottom() + 1);
        plate.lineTo(play_rect.left(), play_rect.bottom() + 1);
        plate.closeSubpath();
        painter.setPen(Qt::NoPen);
        painter.setBrush(colors.accent);
        painter.drawPath(plate);
        painter.setFont(plate_font);
        painter.setPen(Qt::white);
        painter.drawText(play_rect, Qt::AlignCenter, plate_text);
    }
    painter.restore();
}

void RisStyle::PaintShelf(QPainter& painter, const Colors& colors) {
    const QRect shelf = ShelfRect();
    QLinearGradient fade(0, shelf.top(), 0, shelf.bottom());
    QColor top = colors.header;
    top.setAlpha(120);
    fade.setColorAt(0.0, top);
    fade.setColorAt(1.0, colors.header);
    painter.fillRect(shelf, fade);
    painter.fillRect(QRect(shelf.left(), shelf.top(), shelf.width(), 1), colors.faint);

    const QFont label = MonoFont(11);
    painter.setFont(label);
    painter.setPen(colors.muted);
    painter.drawText(QRect(Margin, shelf.top(), shelf.width() - Margin * 2, ShelfLabel),
                     Qt::AlignLeft | Qt::AlignVCenter, tr("// LIBRARY"));

    if (entries.isEmpty()) {
        return;
    }
    painter.save();
    painter.setClipRect(shelf);
    const int tile = TileSize();
    const QFontMetrics metrics(font());
    const bool dark = UISettings::IsRisTheme() || LibraryStyleContext::IsDark(palette());
    for (int i = 0; i < entries.size(); ++i) {
        const QRect rect = TileRect(i);
        if (rect.right() < 0 || rect.left() > width()) {
            continue;
        }
        const QModelIndex index = entries[i];
        const bool selected = i == current;
        painter.save();
        QPainterPath clip;
        clip.addRoundedRect(rect, Radius, Radius);
        painter.setClipPath(clip, Qt::IntersectClip);
        painter.drawPixmap(rect, CachedCover(index, rect.size() * devicePixelRatioF()));
        if (!selected) {
            painter.fillRect(rect, dark ? QColor(0, 0, 0, 90) : QColor(255, 255, 255, 70));
        }
        painter.restore();

        if (selected) {
            painter.setPen(QPen(colors.accent, 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(QRectF(rect).adjusted(-1, -1, 1, 1), Radius, Radius);
            painter.fillRect(QRect(rect.left(), rect.bottom() + Lift - 2, rect.width(), 3),
                             colors.accent);
        }
        if (show_names) {
            const QRect name(rect.left(), shelf.top() + ShelfLabel + Lift + tile + 6, tile,
                             metrics.lineSpacing());
            QFont name_font = font();
            name_font.setBold(selected);
            LibraryStyleContext::PaintTitle(&painter, name, LibraryStyleContext::Title(index),
                                            selected ? colors.text : colors.muted, name_font,
                                            Qt::AlignHCenter | Qt::AlignTop, 1,
                                            selected ? this : nullptr);
        }
    }
    painter.restore();

    // Edge fades when the shelf scrolls.
    const auto edge = [&](int x, bool left_side) {
        QLinearGradient gradient(x, 0, left_side ? x + 40 : x - 40, 0);
        gradient.setColorAt(0.0, colors.header);
        QColor clear = colors.header;
        clear.setAlpha(0);
        gradient.setColorAt(1.0, clear);
        const QRect band = left_side ? QRect(x, shelf.top() + ShelfLabel, 40, shelf.height())
                                     : QRect(x - 40, shelf.top() + ShelfLabel, 40, shelf.height());
        painter.fillRect(band, gradient);
    };
    if (offset > 1) {
        edge(0, true);
    }
    if (offset < MaxOffset() - 1) {
        edge(width(), false);
    }
}

void RisStyle::PaintFooter(QPainter& painter, const Colors& colors) {
    const QRect footer = FooterRect();
    painter.fillRect(footer, colors.header);
    painter.fillRect(QRect(footer.left(), footer.top(), footer.width(), 1), colors.faint);

    const QFont mono = MonoFont(11);
    painter.setFont(mono);
    if (current >= 0) {
        painter.setPen(colors.accent_hot);
        painter.drawText(footer.adjusted(Margin, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("%1 / %2")
                             .arg(current + 1, 2, 10, QLatin1Char('0'))
                             .arg(entries.size(), 2, 10, QLatin1Char('0')));
    }
    const QStringList hints{tr("A Play"), tr("B Names"), tr("Y Details"), tr("X Shelf size"),
                            tr("LB RB Style")};
    painter.setPen(colors.muted);
    painter.drawText(footer.adjusted(0, 0, -Margin, 0), Qt::AlignRight | Qt::AlignVCenter,
                     hints.join(QStringLiteral("    ")).toUpper());
}

void RisStyle::resizeEvent(QResizeEvent* event) {
    LibraryStyleView::resizeEvent(event);
    Relayout();
}

void RisStyle::keyPressEvent(QKeyEvent* event) {
    if (entries.isEmpty()) {
        LibraryStyleView::keyPressEvent(event);
        return;
    }
    const int page = VisibleTiles();
    switch (event->key()) {
    case Qt::Key_Left:
        Select(current - 1);
        break;
    case Qt::Key_Right:
        Select(current + 1);
        break;
    case Qt::Key_Up:
    case Qt::Key_PageUp:
        Select(current - page);
        break;
    case Qt::Key_Down:
    case Qt::Key_PageDown:
        Select(current + page);
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

void RisStyle::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    const QPoint point = event->position().toPoint();
    if (event->button() == Qt::LeftButton && play_rect.contains(point)) {
        emit LaunchRequested(CurrentIndex());
        return;
    }
    const int position = PositionAt(point);
    if (position >= 0) {
        Select(position);
    }
    LibraryStyleView::mousePressEvent(event);
}

void RisStyle::mouseDoubleClickEvent(QMouseEvent* event) {
    const QPoint point = event->position().toPoint();
    if (event->button() == Qt::LeftButton) {
        const int position = PositionAt(point);
        if (position >= 0) {
            Select(position);
            emit LaunchRequested(CurrentIndex());
            return;
        }
        if (HeroCoverRect().contains(point) && CurrentIndex().isValid()) {
            emit LaunchRequested(CurrentIndex());
            return;
        }
    }
    LibraryStyleView::mouseDoubleClickEvent(event);
}

void RisStyle::contextMenuEvent(QContextMenuEvent* event) {
    int position = PositionAt(event->pos());
    if (position < 0 && HeroRect().contains(event->pos())) {
        position = current;
    }
    if (position >= 0) {
        Select(position);
    }
    emit ContextMenuRequested(position >= 0 ? QModelIndex{entries[position]} : QModelIndex{},
                              event->globalPos());
}

void RisStyle::wheelEvent(QWheelEvent* event) {
    const QPoint delta = event->angleDelta();
    const int step = std::abs(delta.x()) > std::abs(delta.y()) ? delta.x() : delta.y();
    scroll_animation->stop();
    offset = std::clamp(offset - step, 0.0, MaxOffset());
    update();
    event->accept();
}

} // namespace LibraryStyles
