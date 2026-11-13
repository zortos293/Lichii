// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Showcase library style. Concept by inxomnyaa.

#include <algorithm>
#include <cmath>

#include <QAbstractItemModel>
#include <QApplication>
#include <QCoreApplication>
#include <QEasingCurve>
#include <QEvent>
#include <QFontMetrics>
#include <QImage>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QRadialGradient>
#include <QResizeEvent>
#include <QStringList>
#include <QTransform>
#include <QVariantAnimation>
#include <QWheelEvent>

#include "qt_common/game_list/game_list_p.h"
#include "lichii/game/styles/carousel_style.h"
#include "lichii/game/styles/style_helpers.h"

namespace LibraryStyles {

namespace {

/// Cover corner radius as a share of its side (the concept: 10 px on a 230 px cover).
constexpr qreal kCoverRadius = 10.0 / 230.0;
/// Reflection: the bottom 22 % of the art, mirrored 6 px (at 230 px) under the cover.
constexpr qreal kReflectionShare = 0.22;
constexpr qreal kReflectionGap = 6.0 / 230.0;

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

constexpr int kMaxTints = 64;

constexpr int kAnimationMs = 180;
constexpr int kMaxDots = 15;
constexpr int kVisibleRadius = 4;

QColor HashColor(const QString& text) {
    const size_t hash = static_cast<size_t>(qHash(text));
    return QColor::fromHsvF(static_cast<qreal>(hash % 360) / 360.0, 0.55, 0.55);
}

/// Average colour of the pixmap, weighted towards saturated and bright pixels.
QColor DominantColor(const QPixmap& pixmap) {
    const QImage image = pixmap.toImage()
                             .scaled(24, 24, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_ARGB32);
    qreal r = 0, g = 0, b = 0, total = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor c = image.pixelColor(x, y);
            if (c.alphaF() < 0.5) {
                continue;
            }
            const qreal weight = 0.05 + c.hsvSaturationF() * c.valueF();
            r += c.redF() * weight;
            g += c.greenF() * weight;
            b += c.blueF() * weight;
            total += weight;
        }
    }
    if (total <= 0.0) {
        return QColor(60, 60, 70);
    }
    return QColor::fromRgbF(r / total, g / total, b / total);
}

/// A neighbour at rest (80 % of the selected size, 9 % tilt) with its cover at the origin.
struct RestingTilt {
    bool valid = false;
    /// Maps the cover square (0, 0, side, side) onto the tilted cover.
    QTransform transform;
    /// Shadow rect (0 18px 30px of the concept) and reflection rect in the cover's space.
    QRectF shadow;
    QRectF reflection;
    /// Everything above together; the tilted cover itself is (0, 0, 0.8 side, 0.8 side).
    QRectF bounds;
};

RestingTilt RestingTiltFor(bool right, qreal side) {
    RestingTilt tilt;
    constexpr qreal scale = 0.8;
    const qreal w = side * scale;
    const qreal far_h = w * (1.0 - 2.0 * 0.09);
    QPolygonF quad;
    // Same turn as PaintCover: the inner edge (towards the selection) is the shorter one.
    if (!right) {
        quad << QPointF(0, 0) << QPointF(w, (w - far_h) / 2) << QPointF(w, (w + far_h) / 2)
             << QPointF(0, w);
    } else {
        quad << QPointF(0, (w - far_h) / 2) << QPointF(w, 0) << QPointF(w, w)
             << QPointF(0, (w + far_h) / 2);
    }
    QPolygonF source;
    source << QPointF(0, 0) << QPointF(side, 0) << QPointF(side, side) << QPointF(0, side);
    tilt.valid = QTransform::quadToQuad(source, quad, tilt.transform);
    const qreal blur = 30.0 * side / 230.0 * scale;
    const qreal drop = 18.0 * side / 230.0 * scale;
    tilt.shadow = QRectF(-blur, -blur + drop, w + 2 * blur, w + 2 * blur);
    tilt.reflection = QRectF(0, side + kReflectionGap * side, side, side * kReflectionShare);
    tilt.bounds = QRectF(0, 0, w, w).united(tilt.shadow);
    if (tilt.valid) {
        tilt.bounds = tilt.bounds.united(tilt.transform.mapRect(tilt.reflection));
    }
    return tilt;
}

} // namespace

CarouselStyle::CarouselStyle(QWidget* parent) : LibraryStyleView(parent) {
    setObjectName(QStringLiteral("libraryStyleCarousel"));
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);

    animation_ = new QVariantAnimation(this);
    animation_->setDuration(kAnimationMs);
    animation_->setEasingCurve(QEasingCurve::OutCubic);
    animation_->setStartValue(0.0);
    animation_->setEndValue(1.0);
    connect(animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        progress_ = value.toReal();
        position_ = start_position_ + (target_position_ - start_position_) * progress_;
        update();
    });

    // Between frames: one cover per pass, so a frame is never held up for long.
    prefetch_timer_.setSingleShot(true);
    prefetch_timer_.setInterval(0);
    connect(&prefetch_timer_, &QTimer::timeout, this, &CarouselStyle::Prefetch);

    rebuild_timer_.setSingleShot(true);
    rebuild_timer_.setInterval(30);
    connect(&rebuild_timer_, &QTimer::timeout, this, [this] {
        EnsureBuilt();
        if (change_pending_) {
            change_pending_ = false;
            emit CurrentChanged(CurrentIndex());
        }
        update();
    });
}

CarouselStyle::~CarouselStyle() = default;

void CarouselStyle::SetModel(QAbstractItemModel* model) {
    if (model_) {
        QObject::disconnect(model_, nullptr, this, nullptr);
    }
    model_ = model;
    items_.clear();
    current_ = 0;
    position_ = 0.0;
    target_position_ = 0.0;
    progress_ = 1.0;
    previous_row_ = -1;
    animation_->stop();
    if (model_) {
        const auto schedule = [this] { ScheduleRebuild(); };
        connect(model_, &QAbstractItemModel::modelReset, this, schedule);
        connect(model_, &QAbstractItemModel::layoutChanged, this, schedule);
        connect(model_, &QAbstractItemModel::rowsInserted, this, schedule);
        connect(model_, &QAbstractItemModel::rowsRemoved, this, schedule);
        connect(model_, &QAbstractItemModel::rowsMoved, this, schedule);
        // Data changes (icons loading in) only invalidate the cached art, not the row list. Play
        // time updates leave the art alone.
        connect(model_, &QAbstractItemModel::dataChanged, this,
                [this](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
                    if (ArtRolesChanged(roles)) {
                        cover_cache_.clear();
                        tilted_cache_.clear();
                        tint_cache_.clear();
                        background_cache_.clear();
                    }
                    update();
                });
        connect(model_, &QObject::destroyed, this, [this] {
            items_.clear();
            update();
        });
    }
    dirty_ = true;
    EnsureBuilt();
    change_pending_ = false;
    update();
}

QModelIndex CarouselStyle::CurrentIndex() const {
    const_cast<CarouselStyle*>(this)->EnsureBuilt();
    return IndexAt(current_);
}

void CarouselStyle::SetCurrentIndex(const QModelIndex& index) {
    EnsureBuilt();
    if (!index.isValid()) {
        return;
    }
    const auto it = std::find_if(items_.begin(), items_.end(),
                                 [&](const QPersistentModelIndex& i) { return i == index; });
    if (it == items_.end()) {
        return;
    }
    const bool was_blocked = blockSignals(true);
    SelectRow(static_cast<int>(it - items_.begin()), false);
    blockSignals(was_blocked);
    update();
}

void CarouselStyle::SetShowNames(bool show) {
    show_names_ = show;
    update();
}

void CarouselStyle::SetShowInfo(bool show) {
    show_info_ = show;
    update();
}

QString CarouselStyle::CycleOption() {
    flat_ = !flat_;
    update();
    return flat_ ? tr("Covers: flat") : tr("Covers: tiles");
}

void CarouselStyle::ScheduleRebuild() {
    dirty_ = true;
    if (!rebuild_timer_.isActive()) {
        rebuild_timer_.start();
    }
}

void CarouselStyle::EnsureBuilt() {
    if (dirty_) {
        Rebuild();
    }
}

void CarouselStyle::Rebuild() {
    dirty_ = false;

    const bool had_items = !items_.empty();
    const QPersistentModelIndex previous =
        current_ >= 0 && current_ < static_cast<int>(items_.size())
            ? items_[static_cast<size_t>(current_)]
            : QPersistentModelIndex();

    items_.clear();
    if (model_) {
        // Games are shown flat: top level games plus the games inside each folder row.
        // Favorites and "add folder" rows are not covers.
        const int top_rows = model_->rowCount();
        for (int r = 0; r < top_rows; ++r) {
            const QModelIndex index = model_->index(r, 0);
            const GameListItemType type = TypeOf(index);
            if (IsGameType(type)) {
                items_.emplace_back(index);
            } else if (IsFolderType(type)) {
                const int children = model_->rowCount(index);
                for (int c = 0; c < children; ++c) {
                    const QModelIndex child = model_->index(c, 0, index);
                    if (IsGameType(TypeOf(child))) {
                        items_.emplace_back(child);
                    }
                }
            }
        }
    }

    int new_current = std::clamp(current_, 0, std::max(0, static_cast<int>(items_.size()) - 1));
    if (previous.isValid()) {
        const auto it = std::find_if(items_.begin(), items_.end(),
                                     [&](const QPersistentModelIndex& i) { return i == previous; });
        if (it != items_.end()) {
            new_current = static_cast<int>(it - items_.begin());
        } else {
            change_pending_ = true;
        }
    } else if (had_items) {
        // The selected item was removed.
        change_pending_ = true;
    }
    current_ = new_current;
    animation_->stop();
    position_ = current_;
    target_position_ = current_;
    progress_ = 1.0;
    previous_row_ = -1;
    cover_cache_.clear();
    tilted_cache_.clear();
    tint_cache_.clear();
    background_cache_.clear();
}

QModelIndex CarouselStyle::IndexAt(int row) const {
    if (row < 0 || row >= static_cast<int>(items_.size())) {
        return {};
    }
    return QModelIndex(items_[static_cast<size_t>(row)]);
}

void CarouselStyle::SelectRow(int row, bool animate) {
    if (items_.empty()) {
        return;
    }
    const int count = static_cast<int>(items_.size());
    row = ((row % count) + count) % count;
    if (row == current_) {
        return;
    }
    // The ring: go the short way round, so the last game sits next to the first.
    int step = row - current_;
    if (step > count / 2) {
        step -= count;
    } else if (step < -count / 2) {
        step += count;
    }
    MoveSteps(step, animate, kAnimationMs);
}

void CarouselStyle::MoveSteps(int step, bool animate, int duration_ms) {
    const int count = static_cast<int>(items_.size());
    if (count == 0 || step == 0) {
        return;
    }
    previous_row_ = current_;
    start_position_ = position_;
    target_position_ = std::round(target_position_) + step;
    current_ = ((static_cast<int>(target_position_) % count) + count) % count;
    animation_->stop();
    if (animate) {
        progress_ = 0.0;
        animation_->setDuration(duration_ms);
        animation_->start();
    } else {
        position_ = target_position_;
        progress_ = 1.0;
        previous_row_ = -1;
    }
    update();
    emit CurrentChanged(IndexAt(current_));
}

void CarouselStyle::MoveBy(int delta) {
    // Steps around the ring: past the last game comes the first again.
    MoveSteps(delta, true, kAnimationMs);
}

void CarouselStyle::RepeatBy(int delta) {
    // A held key repeats faster than the full animation: restarting it on every repeat left the
    // covers a game or two behind the selection the whole time. Each step gets the time until
    // the next repeat instead (a jump when that is too short to see), so the centre cover is the
    // selected game on every step.
    const qint64 since_last = key_step_clock_.isValid() ? key_step_clock_.restart() : kAnimationMs;
    if (!key_step_clock_.isValid()) {
        key_step_clock_.start();
    }
    const int duration = static_cast<int>(std::min<qint64>(kAnimationMs, since_last * 3 / 4));
    MoveSteps(delta, duration >= 16, std::max(duration, 1));
    // No background cross fade while held: it would restart on every step and flicker between
    // two games.
    previous_row_ = -1;
}

void CarouselStyle::Launch(int row) {
    const QModelIndex index = IndexAt(row);
    if (index.isValid()) {
        last_launch_row_ = row;
        launch_timer_.start();
        emit LaunchRequested(index);
    }
}

int CarouselStyle::HitRow(const QPoint& point) const {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (it->polygon.containsPoint(QPointF(point), Qt::OddEvenFill)) {
            return it->row;
        }
    }
    return -1;
}

CarouselStyle::Metrics CarouselStyle::ComputeMetrics() const {
    Metrics m;
    const qreal w = width();
    // The hint pill takes the bottom; the covers, texts and dots centre above it.
    const qreal h = std::max(1, height() - LibraryStyleContext::HintPillReserve);
    // The centre cover is the featured size (1.6x the icon size), shrunk only to fit the window.
    // Floor at the smallest icon option (32 px), so every size step still counts.
    m.side = std::max(32.0, std::min<qreal>({static_cast<qreal>(LibraryStyleContext::FeaturedArtSize()),
                                             w * 0.45, h * 0.46}));
    m.title_px = std::clamp(m.side * 0.1, 15.0, 32.0);
    m.info_px = std::clamp(m.title_px * 0.5, 11.0, 15.0);

    // The texts start under the visible part of the reflection (6 px gap, then 22 % of the side
    // that has faded out by 80 %), so the reflection never shows behind them.
    const qreal under_cover =
        std::max(22.0, m.side * (kReflectionGap + kReflectionShare * 0.8) + 10.0);
    qreal block = m.side + under_cover + 22.0;
    if (show_names_) {
        block += m.title_px * 1.5;
    }
    if (show_info_) {
        block += m.info_px * 1.6 + 6.0;
    }
    const qreal top = std::max(12.0, (h - block) * 0.45);
    m.center_x = w * 0.5;
    m.center_y = top + m.side * 0.5;
    qreal y = top + m.side + under_cover;
    m.title_y = y;
    if (show_names_) {
        y += m.title_px * 1.5;
    }
    m.info_y = y;
    if (show_info_) {
        y += m.info_px * 1.6 + 6.0;
    }
    m.dots_y = y + 8.0;
    return m;
}

QPixmap CarouselStyle::SourceArt(int row, bool* has_art) const {
    const QModelIndex index = IndexAt(row);
    QPixmap pixmap = index.data(GameListItemPath::CardIconRole).value<QPixmap>();
    bool has = index.data(GameListItemPath::HasIconRole).toBool();
    if (pixmap.isNull()) {
        pixmap = index.data(Qt::DecorationRole).value<QPixmap>();
    }
    has = has && !pixmap.isNull();
    if (has_art) {
        *has_art = has;
    }
    return pixmap;
}

QString CarouselStyle::TitleFor(int row) const {
    const QModelIndex index = IndexAt(row);
    QString title = index.data(GameListItemPath::TitleRole).toString();
    if (title.isEmpty()) {
        title = index.data(Qt::DisplayRole).toString().section(QLatin1Char('\n'), 0, 0);
    }
    return title;
}

void CarouselStyle::TrimCaches(int lo, int hi) {
    const int count = static_cast<int>(items_.size());
    // Two slots of margin on each side: the next step and the prefetched covers stay.
    const int keep_slots = hi - lo + 5;
    if (count <= keep_slots) {
        return;
    }
    if (cover_cache_.size() <= keep_slots && tilted_cache_.size() <= 2 * keep_slots) {
        return;
    }
    const auto kept = [&](int row) {
        for (int slot = lo - 2; slot <= hi + 2; ++slot) {
            if (((slot % count) + count) % count == row) {
                return true;
            }
        }
        return false;
    };
    for (auto it = cover_cache_.begin(); it != cover_cache_.end();) {
        it = kept(it.key()) ? std::next(it) : cover_cache_.erase(it);
    }
    for (auto it = tilted_cache_.begin(); it != tilted_cache_.end();) {
        it = kept(static_cast<int>(it.key() / 2)) ? std::next(it) : tilted_cache_.erase(it);
    }
    if (reflection_cache_.size() > 2 * keep_slots) {
        reflection_cache_.clear();
    }
}

void CarouselStyle::Prefetch() {
    if (items_.empty() || dirty_ || !isVisible()) {
        return;
    }
    const Metrics m = ComputeMetrics();
    const qreal dpr = devicePixelRatioF();
    const int count = static_cast<int>(items_.size());
    const int radius = std::min(kVisibleRadius, (count - 1) / 2);
    const int cover_side = static_cast<int>(std::ceil(m.side));
    const int cover_px = std::max(1, static_cast<int>(std::ceil(cover_side * dpr)));
    const int tilt_px = std::max(1, static_cast<int>(std::ceil(m.side * dpr)));
    const auto has_cover = [&](int row) {
        return cover_cache_side_ == cover_px && cover_cache_.contains(row);
    };
    const auto has_tilt = [&](int row, bool right) {
        return tilted_side_ == tilt_px &&
               tilted_cache_.contains(static_cast<qint64>(row) * 2 + (right ? 1 : 0));
    };
    // Builds the first missing cover and comes back for the next one.
    const auto build = [&](int row, bool right) {
        if (!has_cover(row)) {
            CoverPixmap(row, cover_side);
        } else if (!flat_ && !has_tilt(row, right)) {
            TiltedPixmap(row, right, m.side);
        } else {
            return false;
        }
        prefetch_timer_.start();
        return true;
    };
    // The selection rests turned to either side once the next step moves it away.
    if (build(current_, true) || build(current_, false)) {
        return;
    }
    for (int d = 1; d <= radius + 1; ++d) {
        for (const int sign : {1, -1}) {
            const int row = ((current_ + sign * d) % count + count) % count;
            if (build(row, sign > 0)) {
                return;
            }
        }
    }
}

const CarouselStyle::Tint& CarouselStyle::TintFor(int row) {
    const auto found = tint_cache_.constFind(row);
    if (found != tint_cache_.constEnd()) {
        return *found;
    }
    // One small entry per game, but never let the cache grow with the library.
    if (tint_cache_.size() >= kMaxTints) {
        tint_cache_.clear();
    }
    Tint tint;
    bool has_art = false;
    const QPixmap art = SourceArt(row, &has_art);
    if (has_art) {
        tint.color = DominantColor(art);
        tint.blur = art.scaled(32, 32, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    } else {
        tint.color = HashColor(TitleFor(row));
    }
    return *tint_cache_.insert(row, tint);
}

QPixmap CarouselStyle::CoverPixmap(int row, int side) {
    const qreal dpr = devicePixelRatioF();
    const int px = std::max(1, static_cast<int>(std::ceil(side * dpr)));
    if (cover_cache_side_ != px) {
        cover_cache_.clear();
        tilted_cache_.clear();
        cover_cache_side_ = px;
    }
    const auto found = cover_cache_.constFind(row);
    if (found != cover_cache_.constEnd()) {
        return *found;
    }
    // TrimCaches keeps the cache to the covers around the selection.
    bool has_art = false;
    const QPixmap art = SourceArt(row, &has_art);

    QImage image(px, px, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        QPainterPath path;
        const qreal radius = px * kCoverRadius;
        path.addRoundedRect(QRectF(0, 0, px, px), radius, radius);
        p.setClipPath(path);
        if (has_art) {
            // Fill the square (crop, never letterbox) so the ring always meets art.
            const QPixmap scaled =
                art.scaled(px, px, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            p.drawPixmap((px - scaled.width()) / 2, (px - scaled.height()) / 2, scaled);
        } else {
            const QColor base = HashColor(TitleFor(row));
            QLinearGradient gradient(0, 0, 0, px);
            gradient.setColorAt(0.0, base.lighter(130));
            gradient.setColorAt(1.0, base.darker(150));
            p.fillRect(QRectF(0, 0, px, px), gradient);
            const QString title = TitleFor(row).trimmed();
            if (!title.isEmpty()) {
                QFont font = this->font();
                font.setPixelSize(std::max(8, static_cast<int>(px * 0.45)));
                font.setBold(true);
                p.setFont(font);
                p.setPen(QColor(255, 255, 255, 220));
                p.drawText(QRectF(0, 0, px, px), Qt::AlignCenter, title.left(1).toUpper());
            }
        }
    }
    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(dpr);
    cover_cache_.insert(row, pixmap);
    return pixmap;
}

QPixmap CarouselStyle::BackgroundFor(int row) {
    const auto found = background_cache_.constFind(row);
    if (found != background_cache_.constEnd()) {
        return *found;
    }
    // Entries are window sized: keep only the selection and the one it fades from.
    for (auto it = background_cache_.begin(); it != background_cache_.end();) {
        if (it.key() != current_ && it.key() != previous_row_) {
            it = background_cache_.erase(it);
        } else {
            ++it;
        }
    }
    // Blur, wash and gradients have no fine detail: render at half the logical size and scale it
    // up once below. A full resolution render cost tens of ms per selection at 200 %.
    QPixmap pixmap(QSize(std::max(1, (width() + 1) / 2), std::max(1, (height() + 1) / 2)));
    const QPalette& pal = palette();
    const QRect area(0, 0, width(), height());
    QPainter painter(&pixmap);
    painter.scale(0.5, 0.5);
    painter.fillRect(area, pal.color(QPalette::Window));
    const Tint tint = TintFor(row);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    if (!tint.blur.isNull()) {
        painter.setOpacity(0.5);
        painter.drawPixmap(area, tint.blur);
        painter.setOpacity(1.0);
    }

    // Wash the blurred cover with its dominant colour, then fade into the palette at the bottom.
    QColor wash = tint.color;
    wash.setAlpha(110);
    painter.fillRect(area, wash);

    QRadialGradient glow(QPointF(width() * 0.4, height() * 0.4), std::max(width(), height()) * 0.6);
    QColor glow_color = tint.color.lighter(150);
    glow_color.setAlpha(90);
    glow.setColorAt(0.0, glow_color);
    glow_color.setAlpha(0);
    glow.setColorAt(1.0, glow_color);
    painter.fillRect(area, glow);

    QColor base = pal.color(QPalette::Window);
    QLinearGradient fade(0, 0, 0, height());
    base.setAlpha(60);
    fade.setColorAt(0.0, base);
    base.setAlpha(190);
    fade.setColorAt(1.0, base);
    painter.fillRect(area, fade);
    painter.end();
    // Scale it up once to the device size, so a frame copies it 1:1 instead of scaling the whole
    // window again.
    const qreal dpr = devicePixelRatioF();
    QPixmap full = pixmap.scaled(QSize(std::max(1, static_cast<int>(std::ceil(width() * dpr))),
                                       std::max(1, static_cast<int>(std::ceil(height() * dpr)))),
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    full.setDevicePixelRatio(dpr);
    background_cache_.insert(row, full);
    return full;
}

QPixmap CarouselStyle::TiltedPixmap(int row, bool right, qreal side) {
    const qreal dpr = devicePixelRatioF();
    const int px = std::max(1, static_cast<int>(std::ceil(side * dpr)));
    if (tilted_side_ != px) {
        tilted_cache_.clear();
        tilted_side_ = px;
    }
    const qint64 key = static_cast<qint64>(row) * 2 + (right ? 1 : 0);
    const auto found = tilted_cache_.constFind(key);
    if (found != tilted_cache_.constEnd()) {
        return *found;
    }
    // Shadow, tilted cover and reflection in one pixmap, drawn the same way PaintCover draws a
    // moving cover.
    const RestingTilt tilt = RestingTiltFor(right, side);
    const QRectF& bounds = tilt.bounds;
    QPixmap tilted(QSize(std::max(1, static_cast<int>(std::ceil(bounds.width() * dpr))),
                         std::max(1, static_cast<int>(std::ceil(bounds.height() * dpr)))));
    tilted.setDevicePixelRatio(dpr);
    tilted.fill(Qt::transparent);
    if (tilt.valid) {
        const QPixmap cover = CoverPixmap(row, static_cast<int>(std::ceil(side)));
        const QPixmap& shadow = ShadowFor(side, false);
        const QPixmap mirror = Reflection(cover);
        QPainter painter(&tilted);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.translate(-bounds.topLeft());
        painter.drawPixmap(tilt.shadow, shadow, QRectF(shadow.rect()));
        painter.setWorldTransform(tilt.transform, true);
        painter.drawPixmap(QRectF(0, 0, side, side), cover, QRectF(cover.rect()));
        painter.drawPixmap(tilt.reflection, mirror, QRectF(mirror.rect()));
    }
    tilted_cache_.insert(key, tilted);
    return tilted;
}

void CarouselStyle::PaintBackground(QPainter& painter) {
    const QPalette& pal = palette();
    if (items_.empty()) {
        painter.fillRect(rect(), pal.color(QPalette::Window));
        return;
    }
    // The tinted background of each cover is built once, then cross faded while moving.
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const int cur = std::clamp(current_, 0, static_cast<int>(items_.size()) - 1);
    // Drawn at the origin, not into rect(): the pixmap is the widget in device pixels (rounded
    // up), so this is a plain copy. Fitting it into rect() scales it by a fraction of a pixel at
    // 125 % or 150 %, which takes the slow filtered path for the whole window.
    if (progress_ < 1.0 && previous_row_ >= 0 && previous_row_ < static_cast<int>(items_.size())) {
        painter.drawPixmap(QPointF(0, 0), BackgroundFor(previous_row_));
        painter.setOpacity(progress_);
        painter.drawPixmap(QPointF(0, 0), BackgroundFor(cur));
        painter.setOpacity(1.0);
        return;
    }
    painter.drawPixmap(QPointF(0, 0), BackgroundFor(cur));
}

const QPixmap& CarouselStyle::ShadowFor(qreal side, bool selected) {
    const int px = static_cast<int>(std::ceil(side));
    QPixmap& slot = selected ? shadow_selected_ : shadow_;
    if (slot.isNull() || shadow_side_[selected ? 1 : 0] != px) {
        const int blur = std::max(4, static_cast<int>(std::lround((selected ? 40.0 : 30.0) * side /
                                                                 230.0)));
        slot = LibraryStyleContext::SoftShadow(QSize(px, px), side * kCoverRadius, blur,
                                               selected ? 0.5 : 0.4, devicePixelRatioF());
        shadow_side_[selected ? 1 : 0] = px;
    }
    return slot;
}

QPixmap CarouselStyle::Reflection(const QPixmap& cover) {
    const qint64 key = cover.cacheKey();
    if (const auto it = reflection_cache_.constFind(key); it != reflection_cache_.constEnd()) {
        return *it;
    }
    if (reflection_cache_.size() > 48) {
        reflection_cache_.clear();
    }
    const QImage source = cover.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int h = std::max(1, static_cast<int>(source.height() * kReflectionShare));
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    QImage part = source.copy(0, source.height() - h, source.width(), h).flipped(Qt::Vertical);
#else
    QImage part = source.copy(0, source.height() - h, source.width(), h).mirrored(false, true);
#endif
    // The fade below works in device pixels: with the cover's pixel ratio still set, the painter
    // would stretch it over ratio times the height and the reflection would end in a hard edge.
    part.setDevicePixelRatio(1.0);
    {
        // Strongest next to the cover (28 %), gone by 80 % of the way down.
        QPainter p(&part);
        p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        QLinearGradient fade(0, 0, 0, h);
        fade.setColorAt(0.0, QColor(0, 0, 0, 72));
        fade.setColorAt(0.8, QColor(0, 0, 0, 0));
        p.fillRect(part.rect(), fade);
    }
    QPixmap out = QPixmap::fromImage(part);
    out.setDevicePixelRatio(cover.devicePixelRatio());
    reflection_cache_.insert(key, out);
    return out;
}

void CarouselStyle::PaintCover(QPainter& painter, const Metrics& m, int row, qreal d) {
    const qreal a = std::abs(d);
    const qreal sign = d < 0 ? -1.0 : 1.0;
    const qreal side = m.side;

    qreal scale;
    qreal offset;
    qreal tilt;
    qreal opacity;
    if (!flat_) {
        scale = a <= 1.0 ? 1.0 - 0.2 * a : std::max(0.5, 0.8 - 0.07 * (a - 1.0));
        offset = a <= 1.0 ? 0.45 * side * a : 0.45 * side + (a - 1.0) * 0.18 * side;
        tilt = std::min(a, 1.0) * 0.09;
        opacity = std::max(0.35, 1.0 - 0.1 * a);
    } else {
        constexpr qreal gap = 0.04;
        scale = a <= 1.0 ? 1.0 - 0.25 * a : 0.75;
        offset = a <= 1.0 ? a * (0.5 + gap + 0.375) * side
                          : (0.5 + gap + 0.375) * side + (a - 1.0) * (0.75 + gap) * side;
        tilt = 0.0;
        opacity = std::max(0.4, 1.0 - 0.1 * a);
    }
    opacity *= std::clamp(kVisibleRadius + 1.0 - a, 0.0, 1.0);
    if (opacity <= 0.01) {
        return;
    }

    const qreal x = m.center_x + sign * offset;
    const qreal w = side * scale;
    const qreal h = side * scale;
    const qreal far_h = h * (1.0 - 2.0 * tilt);

    QPolygonF quad;
    // Like the concept: side covers turn their inner edge (towards the selection) away, so that
    // edge is the shorter one and the outer edge the taller one.
    if (sign < 0 || a < 1e-6) {
        quad << QPointF(x - w / 2, m.center_y - h / 2) << QPointF(x + w / 2, m.center_y - far_h / 2)
             << QPointF(x + w / 2, m.center_y + far_h / 2) << QPointF(x - w / 2, m.center_y + h / 2);
    } else {
        quad << QPointF(x - w / 2, m.center_y - far_h / 2) << QPointF(x + w / 2, m.center_y - h / 2)
             << QPointF(x + w / 2, m.center_y + h / 2) << QPointF(x - w / 2, m.center_y + far_h / 2);
    }
    hits_.push_back({row, quad});

    const QPixmap pixmap = CoverPixmap(row, static_cast<int>(std::ceil(side)));
    QPolygonF source;
    source << QPointF(0, 0) << QPointF(side, 0) << QPointF(side, side) << QPointF(0, side);
    QTransform transform;
    if (!QTransform::quadToQuad(source, quad, transform)) {
        return;
    }

    painter.save();
    painter.setOpacity(opacity);

    // Covers at rest beside the selection all have the same tilt: draw one cached pixmap with
    // the shadow, the tilted cover and its reflection, with a plain scale. Transforming every
    // cover and reflection in perspective and scaling every shadow on every frame is slow.
    if (!flat_ && a >= 1.0) {
        const QPixmap tilted = TiltedPixmap(row, sign > 0, side);
        const QRectF bounds = RestingTiltFor(sign > 0, side).bounds;
        const QRectF box = quad.boundingRect();
        const qreal f = box.width() / (side * 0.8);
        painter.drawPixmap(QRectF(box.left() + bounds.left() * f, box.top() + bounds.top() * f,
                                  bounds.width() * f, bounds.height() * f),
                           tilted, QRectF(tilted.rect()));
        painter.restore();
        return;
    }

    // Concept shadow: 0 18px 30px at 40 %, the selected cover 0 24px 40px at 50 % (the concept
    // cover is 230 px). Blurred once per cover size, then scaled to each cover.
    {
        const bool sel = a < 0.5;
        const QPixmap& shadow = sel ? ShadowFor(side, true) : ShadowFor(side, false);
        const qreal k = scale;
        const qreal blur = (sel ? 40.0 : 30.0) * side / 230.0;
        const qreal drop = (sel ? 24.0 : 18.0) * side / 230.0;
        const QRectF box = quad.boundingRect();
        const QRectF target(box.left() - blur * k, box.top() - blur * k + drop * k,
                            box.width() + 2 * blur * k, box.height() + 2 * blur * k);
        painter.drawPixmap(target, shadow, QRectF(shadow.rect()));
    }

    painter.setWorldTransform(transform);
    painter.drawPixmap(QRectF(0, 0, side, side), pixmap, QRectF(pixmap.rect()));

    // Reflection: the bottom 22 % of the art, mirrored 6 px under the cover, fading out.
    {
        const QPixmap mirror = Reflection(pixmap);
        painter.drawPixmap(QRectF(0, side + kReflectionGap * side, side, side * kReflectionShare),
                           mirror, QRectF(mirror.rect()));
    }

    if (a < 0.5) {
        QColor accent = LibraryStyleContext::Accent(palette());
        accent.setAlphaF(1.0 - a * 2.0);
        // 4 px ring (at the concept's 230 px cover) hugging the cover, concentric corners.
        QPen pen(accent, std::max(2.5, 4.0 * side / 230.0));
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        // 1 px over the cover's anti-aliased edge, so no background shows in the corners.
        const qreal grow = pen.widthF() / 2.0 - 1.0;
        painter.drawRoundedRect(QRectF(-grow, -grow, side + 2 * grow, side + 2 * grow),
                                side * kCoverRadius + grow, side * kCoverRadius + grow);
    }
    painter.restore();
}

void CarouselStyle::PaintTexts(QPainter& painter, const Metrics& m) {
    if (items_.empty()) {
        return;
    }
    const int row = std::clamp(current_, 0, static_cast<int>(items_.size()) - 1);
    const QColor text = palette().color(QPalette::WindowText);
    const qreal text_width = std::max(40.0, width() * 0.9);
    const qreal left = (width() - text_width) / 2.0;

    if (show_names_) {
        QFont font = this->font();
        font.setPixelSize(static_cast<int>(m.title_px));
        font.setBold(true);
        // One line (S4); with Scroll Long Names a long name glides.
        LibraryStyleContext::PaintTitle(
            &painter,
            QRectF(left, m.title_y, text_width, m.title_px * 1.5).toAlignedRect(),
            TitleFor(row), text, font, Qt::AlignHCenter | Qt::AlignVCenter, 1, this);
    }
    if (show_info_) {
        QFont font = this->font();
        font.setPixelSize(static_cast<int>(m.info_px));
        QColor muted = text;
        muted.setAlpha(170);
        // One line of fixed parts (S3): whole parts drop out when the window is narrow.
        const QStringList info = LibraryStyleContext::InfoLines(
            LibraryStyleContext::Info(IndexAt(row)),
            LibraryStyleContext::LayoutInfo(font, static_cast<int>(text_width), 1), font);
        LibraryStyleContext::PaintInfoLines(
            &painter, QRectF(left, m.info_y, text_width, m.info_px * 1.6).toAlignedRect(), info,
            muted, font, Qt::AlignHCenter | Qt::AlignVCenter);
    }
}

void CarouselStyle::PaintDots(QPainter& painter, const Metrics& m) {
    const int count = static_cast<int>(items_.size());
    if (count <= 1) {
        return;
    }
    const int dots = std::min(count, kMaxDots);
    const int active = count <= kMaxDots
                           ? current_
                           : static_cast<int>(std::lround(static_cast<double>(current_) *
                                                          (dots - 1) / (count - 1)));
    constexpr qreal spacing = 14.0;
    const qreal start = width() / 2.0 - spacing * (dots - 1) / 2.0;
    QColor muted = palette().color(QPalette::WindowText);
    muted.setAlpha(90);
    painter.setPen(Qt::NoPen);
    for (int i = 0; i < dots; ++i) {
        const bool on = i == active;
        painter.setBrush(on ? LibraryStyleContext::Accent(palette()) : muted);
        const qreal radius = on ? 4.5 : 3.0;
        painter.drawEllipse(QPointF(start + spacing * i, m.dots_y), radius, radius);
    }
}

void CarouselStyle::paintEvent([[maybe_unused]] QPaintEvent* event) {
    EnsureBuilt();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::TextAntialiasing);

    hits_.clear();
    PaintBackground(painter);

    if (items_.empty()) {
        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(rect(), Qt::AlignCenter, tr("No games to show"));
        return;
    }

    const Metrics m = ComputeMetrics();
    const int count = static_cast<int>(items_.size());
    // The covers form a ring: slot k (unwrapped) shows game k mod count. With few games the
    // radius shrinks so no game shows twice.
    const int radius = std::min(kVisibleRadius, (count - 1) / 2);
    const int lo = static_cast<int>(std::floor(position_)) - radius;
    const int hi = static_cast<int>(std::ceil(position_)) + radius;

    std::vector<int> order;
    order.reserve(static_cast<size_t>(std::max(0, hi - lo + 1)));
    for (int slot = lo; slot <= hi; ++slot) {
        order.push_back(slot);
    }
    // Far covers first so the nearer ones overlap them.
    std::stable_sort(order.begin(), order.end(), [this](int l, int r) {
        return std::abs(l - position_) > std::abs(r - position_);
    });
    for (const int slot : order) {
        const int row = ((slot % count) + count) % count;
        PaintCover(painter, m, row, slot - position_);
    }
    TrimCaches(lo, hi);
    if (!prefetch_timer_.isActive()) {
        prefetch_timer_.start();
    }

    PaintTexts(painter, m);
    PaintDots(painter, m);
    LibraryStyleContext::PaintHintPill(
        &painter, rect(), LibraryStyleContext::StandardHints(tr("Covers")),
        LibraryStyleContext::IsDark(palette()));
}

void CarouselStyle::resizeEvent(QResizeEvent* event) {
    LibraryStyleView::resizeEvent(event);
    cover_cache_.clear();
    tilted_cache_.clear();
    background_cache_.clear();
}

void CarouselStyle::changeEvent(QEvent* event) {
    LibraryStyleView::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        cover_cache_.clear();
        tilted_cache_.clear();
        background_cache_.clear();
        update();
    }
}

void CarouselStyle::keyPressEvent(QKeyEvent* event) {
    if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
        LibraryStyleView::keyPressEvent(event);
        return;
    }
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_A:
    case Qt::Key_Right:
    case Qt::Key_D: {
        const int delta = event->key() == Qt::Key_Left || event->key() == Qt::Key_A ? -1 : 1;
        if (event->isAutoRepeat()) {
            RepeatBy(delta);
        } else {
            key_step_clock_.start();
            MoveBy(delta);
        }
        break;
    }
    case Qt::Key_PageUp:
        MoveBy(-5);
        break;
    case Qt::Key_PageDown:
        MoveBy(5);
        break;
    case Qt::Key_Home:
        SelectRow(0);
        break;
    case Qt::Key_End:
        SelectRow(static_cast<int>(items_.size()) - 1);
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        Launch(current_);
        break;
    case Qt::Key_Menu:
        if (const QModelIndex index = IndexAt(current_); index.isValid()) {
            emit ContextMenuRequested(index, mapToGlobal(rect().center()));
        }
        break;
    default:
        LibraryStyleView::keyPressEvent(event);
        return;
    }
    event->accept();
}

void CarouselStyle::mousePressEvent(QMouseEvent* event) {
    setFocus();
    const int row = HitRow(event->position().toPoint());
    if (event->button() == Qt::LeftButton) {
        press_row_ = row;
        press_was_selected_ = row >= 0 && row == current_;
        if (row >= 0 && row != current_) {
            SelectRow(row);
        }
        event->accept();
    } else if (event->button() == Qt::RightButton) {
        if (row >= 0) {
            if (row != current_) {
                SelectRow(row);
            }
            emit ContextMenuRequested(IndexAt(row), event->globalPosition().toPoint());
        }
        event->accept();
    } else {
        LibraryStyleView::mousePressEvent(event);
    }
}

void CarouselStyle::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const int row = HitRow(event->position().toPoint());
        if (press_row_ >= 0 && press_was_selected_ && row == press_row_) {
            Launch(row);
        }
        press_row_ = -1;
        press_was_selected_ = false;
        event->accept();
        return;
    }
    LibraryStyleView::mouseReleaseEvent(event);
}

void CarouselStyle::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        press_row_ = -1;
        press_was_selected_ = false;
        const int row = HitRow(event->position().toPoint());
        if (row >= 0) {
            const bool just_launched =
                launch_timer_.isValid() && last_launch_row_ == row &&
                launch_timer_.elapsed() <= QApplication::doubleClickInterval() + 100;
            if (row != current_) {
                SelectRow(row);
            }
            if (!just_launched) {
                Launch(row);
            }
        }
        event->accept();
        return;
    }
    LibraryStyleView::mouseDoubleClickEvent(event);
}

void CarouselStyle::wheelEvent(QWheelEvent* event) {
    int delta = event->angleDelta().y();
    if (delta == 0) {
        delta = event->angleDelta().x();
    }
    if ((delta > 0 && wheel_accum_ < 0) || (delta < 0 && wheel_accum_ > 0)) {
        wheel_accum_ = 0;
    }
    wheel_accum_ += delta;
    while (wheel_accum_ >= 120) {
        MoveBy(-1);
        wheel_accum_ -= 120;
    }
    while (wheel_accum_ <= -120) {
        MoveBy(1);
        wheel_accum_ += 120;
    }
    event->accept();
}

namespace {

LibraryStyleView* Make(QWidget* parent) {
    return new CarouselStyle(parent);
}

[[maybe_unused]] const bool registered =
    LibraryStyles::Register({"carousel", QT_TRANSLATE_NOOP("LibraryStyles", "Showcase"), &Make});

} // namespace

} // namespace LibraryStyles
