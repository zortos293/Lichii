// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Dual Pane library style. Concept by inxomnyaa.

#include <algorithm>
#include <cmath>

#include <QAbstractItemModel>
#include <QColor>
#include <QDateTime>
#include <QTimer>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QResizeEvent>
#include <QStringList>
#include <QWheelEvent>

#include "qt_common/game_list/game_list_p.h"
#include "qt_common/config/uisettings.h"
#include "lichii/game/styles/dual_pane_style.h"
#include "lichii/game/styles/style_helpers.h"

namespace LibraryStyles {

namespace {

constexpr int Margin = 12;
constexpr int PanePad = 14;
constexpr int StatusHeight = 26;
/// Button hint line along the bottom of the top screen (concept).
constexpr int HintHeight = 24;
constexpr int ScreenRadius = 16;
constexpr int FadeMs = 150;

bool IsDark(const QPalette& palette) {
    return palette.window().color().lightness() < 128;
}

bool IsGameType(int type) {
    return type == static_cast<int>(GameListItemType::Game);
}

bool IsFolderType(int type) {
    return type == static_cast<int>(GameListItemType::CustomDir) ||
           type == static_cast<int>(GameListItemType::SdmcDir) ||
           type == static_cast<int>(GameListItemType::UserNandDir) ||
           type == static_cast<int>(GameListItemType::SysNandDir);
}

QString TitleOf(const QModelIndex& index) {
    QString title = index.data(GameListItemPath::TitleRole).toString();
    if (title.isEmpty()) {
        title = index.data(Qt::DisplayRole).toString();
    }
    return title;
}

QPixmap SourceArt(const QModelIndex& index) {
    QPixmap art = index.data(GameListItemPath::CardIconRole).value<QPixmap>();
    if (art.isNull() && index.data(GameListItemPath::HasIconRole).toBool()) {
        art = index.data(Qt::DecorationRole).value<QPixmap>();
    }
    return art;
}

} // namespace

DualPaneStyle::DualPaneStyle(QWidget* parent)
    : LibraryStyleView(parent), art_cache_(48 * 1024 * 1024) {
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 280);

    // The selection brackets blink like the concept: repaint only the brackets.
    auto* blink_timer = new QTimer(this);
    blink_timer->setInterval(40);
    connect(blink_timer, &QTimer::timeout, this, [this] {
        if (isVisible() && current_ >= 0 && current_ < static_cast<int>(items_.size()) &&
            cols_ > 0) {
            const QRect marker = MarkerRect(current_) & grid_area_;
            if (!marker.isEmpty()) {
                update(marker);
            }
        }
    });
    blink_timer->start();

    fade_.setDuration(FadeMs);
    fade_.setStartValue(0.0);
    fade_.setEndValue(1.0);
    fade_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&fade_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        fade_value_ = value.toReal();
        update(top_rect_);
    });
    connect(&fade_, &QVariantAnimation::finished, this, [this] {
        fade_value_ = 1.0;
        previous_ = QPersistentModelIndex();
        update(top_rect_);
    });

    clock_timer_.setInterval(30 * 1000);
    connect(&clock_timer_, &QTimer::timeout, this, [this] { update(top_rect_); });
    clock_timer_.start();

    rebuild_timer_.setSingleShot(true);
    rebuild_timer_.setInterval(0);
    connect(&rebuild_timer_, &QTimer::timeout, this, [this] { Rebuild(); });
}

DualPaneStyle::~DualPaneStyle() = default;

void DualPaneStyle::SetModel(QAbstractItemModel* model) {
    if (model_) {
        disconnect(model_, nullptr, this, nullptr);
    }
    model_ = model;
    if (model_) {
        connect(model_, &QAbstractItemModel::modelReset, this, [this] { ScheduleRebuild(); });
        connect(model_, &QAbstractItemModel::layoutChanged, this, [this] { ScheduleRebuild(); });
        connect(model_, &QAbstractItemModel::rowsInserted, this, [this] { ScheduleRebuild(); });
        connect(model_, &QAbstractItemModel::rowsRemoved, this, [this] { ScheduleRebuild(); });
        connect(model_, &QAbstractItemModel::rowsMoved, this, [this] { ScheduleRebuild(); });
        // Data changes (play time, icons) keep the row list: repaint only.
        connect(model_, &QAbstractItemModel::dataChanged, this, [this] { update(); });
        connect(model_, &QObject::destroyed, this, [this] {
            model_ = nullptr;
            items_.clear();
            current_ = -1;
            shown_ = QPersistentModelIndex();
            previous_ = QPersistentModelIndex();
            update();
        });
    }
    art_cache_.clear();
    current_ = -1;
    shown_ = QPersistentModelIndex();
    previous_ = QPersistentModelIndex();
    Rebuild();
}

QModelIndex DualPaneStyle::CurrentIndex() const {
    if (current_ < 0 || current_ >= static_cast<int>(items_.size())) {
        return {};
    }
    return QModelIndex(items_[static_cast<size_t>(current_)]);
}

void DualPaneStyle::SetCurrentIndex(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }
    const QModelIndex first = index.siblingAtColumn(0);
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i] == first) {
            SetCurrentRow(static_cast<int>(i), true);
            return;
        }
    }
}

void DualPaneStyle::SetShowNames(bool show) {
    if (show_names_ == show) {
        return;
    }
    show_names_ = show;
    update(top_rect_);
}

void DualPaneStyle::SetShowInfo(bool show) {
    if (show_info_ == show) {
        return;
    }
    show_info_ = show;
    update(top_rect_);
}

QString DualPaneStyle::OptionLabel() const {
    switch (icon_level_) {
    case 0:
        return tr("Icon size: small");
    case 2:
        return tr("Icon size: large");
    default:
        return tr("Icon size: medium");
    }
}

QString DualPaneStyle::CycleOption() {
    icon_level_ = (icon_level_ + 1) % 3;
    art_cache_.clear();
    ComputeLayout();
    ClampScroll();
    EnsureVisible();
    update();
    return OptionLabel();
}

void DualPaneStyle::ScheduleRebuild() {
    if (!rebuild_timer_.isActive()) {
        rebuild_timer_.start();
    }
}

void DualPaneStyle::Rebuild() {
    const QModelIndex old_current = CurrentIndex();
    const bool had_current = current_ >= 0;
    items_.clear();

    if (model_) {
        const int rows = model_->rowCount();
        items_.reserve(static_cast<size_t>(rows));
        for (int r = 0; r < rows; ++r) {
            const QModelIndex top = model_->index(r, 0);
            const int type = top.data(GameListItem::TypeRole).toInt();
            if (IsGameType(type)) {
                items_.emplace_back(top);
            } else if (IsFolderType(type)) {
                const int children = model_->rowCount(top);
                for (int c = 0; c < children; ++c) {
                    const QModelIndex child = model_->index(c, 0, top);
                    if (IsGameType(child.data(GameListItem::TypeRole).toInt())) {
                        items_.emplace_back(child);
                    }
                }
            }
        }
    }

    int row = -1;
    if (old_current.isValid()) {
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i] == old_current) {
                row = static_cast<int>(i);
                break;
            }
        }
    }
    if (row < 0 && !current_path_.isEmpty()) {
        // The old index died in a model reset: find the same game again by its path.
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].data(GameListItemPath::FullPathRole).toString() == current_path_) {
                row = static_cast<int>(i);
                break;
            }
        }
    }
    if (row < 0 && !items_.empty()) {
        row = 0;
    }

    ComputeLayout();
    current_ = row;
    const QModelIndex now = CurrentIndex();
    shown_ = QPersistentModelIndex(now);
    previous_ = QPersistentModelIndex();
    current_path_ = now.isValid() ? now.data(GameListItemPath::FullPathRole).toString() : QString();
    ClampScroll();
    EnsureVisible();
    update();
    if (now != old_current || (had_current && !now.isValid())) {
        emit CurrentChanged(now);
    }
}

void DualPaneStyle::ComputeLayout() {
    const int w = width();
    const int h = height();
    // Two screens across the whole view, the top one a bit shorter (no console frame).
    const int total_h = std::max(0, h - 3 * Margin);
    const int top_h = static_cast<int>(total_h * 0.45);
    top_rect_ = QRect(Margin, Margin, std::max(0, w - 2 * Margin), top_h);
    grid_pane_ = QRect(Margin, top_rect_.bottom() + 1 + Margin, top_rect_.width(),
                       std::max(0, total_h - top_h));
    grid_area_ = grid_pane_.adjusted(PanePad - 4, PanePad - 4, -(PanePad - 4), -(PanePad - 4));

    // The bottom screen's small icons are 3/5 of View > Game Icon Size, so every size option
    // changes them; the style's own option (X) still steps them smaller or larger.
    static constexpr qreal level_scale[3] = {0.72, 1.0, 1.39};
    const int base = LibraryStyleContext::ArtSize() * 3 / 5;
    icon_px_ = std::clamp(static_cast<int>(base * level_scale[std::clamp(icon_level_, 0, 2)]),
                          16, 256);
    // Concept tile: the art is 80 % of a padded shell; the selection brackets sit 7 % of the
    // shell outside it, so neighbours keep a gap of a fifth of the shell.
    const int shell = static_cast<int>(std::lround(icon_px_ / 0.8));
    const int gap_w = std::max(6, shell / 5);
    cols_ = std::max(1, (grid_area_.width() + gap_w) / (shell + gap_w));
    cell_w_ = std::max(1, grid_area_.width() / cols_);
    // Icons only on the bottom screen (the top screen names the selection).
    cell_h_ = shell + gap_w;
}

QRect DualPaneStyle::CellRect(int row) const {
    const int col = row % cols_;
    const int line = row / cols_;
    return QRect(grid_area_.left() + col * cell_w_, grid_area_.top() + line * cell_h_ - scroll_,
                 cell_w_, cell_h_);
}

QRectF DualPaneStyle::ShellRect(int row) const {
    const QRect cell = CellRect(row);
    const qreal shell_side = icon_px_ / 0.8;
    return QRectF(cell.left() + (cell.width() - shell_side) / 2.0,
                  cell.top() + (cell.height() - shell_side) / 2.0, shell_side, shell_side);
}

QRect DualPaneStyle::MarkerRect(int row) const {
    // The brackets sit 7 % outside the shell (see DrawGrid); one more pixel for the anti-aliasing.
    const QRectF shell = ShellRect(row);
    const qreal o = shell.width() * 0.07 + 1.0;
    return shell.adjusted(-o, -o, o, o).toAlignedRect();
}

int DualPaneStyle::RowAt(const QPoint& pos) const {
    if (!grid_area_.contains(pos) || items_.empty()) {
        return -1;
    }
    const int col = (pos.x() - grid_area_.left()) / cell_w_;
    const int line = (pos.y() - grid_area_.top() + scroll_) / cell_h_;
    if (col < 0 || col >= cols_ || line < 0) {
        return -1;
    }
    const int row = line * cols_ + col;
    return row < static_cast<int>(items_.size()) ? row : -1;
}

void DualPaneStyle::ClampScroll() {
    const int lines = (static_cast<int>(items_.size()) + cols_ - 1) / std::max(1, cols_);
    const int content = lines * cell_h_;
    const int max_scroll = std::max(0, content - grid_area_.height());
    scroll_ = std::clamp(scroll_, 0, max_scroll);
}

void DualPaneStyle::EnsureVisible() {
    if (current_ < 0 || grid_area_.height() <= 0) {
        return;
    }
    const int top = (current_ / cols_) * cell_h_;
    const int bottom = top + cell_h_;
    if (top < scroll_) {
        scroll_ = top;
    } else if (bottom > scroll_ + grid_area_.height()) {
        scroll_ = bottom - grid_area_.height();
    }
    ClampScroll();
}

void DualPaneStyle::SetCurrentRow(int row, bool emit_signal) {
    if (items_.empty()) {
        return;
    }
    row = std::clamp(row, 0, static_cast<int>(items_.size()) - 1);
    if (row == current_) {
        EnsureVisible();
        update();
        return;
    }
    fade_.stop();
    const QPersistentModelIndex old_shown = shown_;
    current_ = row;
    shown_ = items_[static_cast<size_t>(row)];
    current_path_ = shown_.data(GameListItemPath::FullPathRole).toString();
    previous_ = old_shown;
    fade_value_ = previous_.isValid() ? 0.0 : 1.0;
    if (previous_.isValid()) {
        fade_.start();
    }
    EnsureVisible();
    update();
    if (emit_signal) {
        emit CurrentChanged(CurrentIndex());
    }
}

QPixmap DualPaneStyle::Art(const QModelIndex& index, int logical_px) {
    const qreal dpr = devicePixelRatioF();
    const int px = std::max(1, static_cast<int>(std::lround(logical_px * dpr)));
    const QPixmap source = SourceArt(index);
    const QString title = TitleOf(index);
    const QString key = source.isNull()
                            ? QStringLiteral("n|%1|%2").arg(title).arg(px)
                            : QStringLiteral("%1|%2").arg(source.cacheKey()).arg(px);
    if (QPixmap* cached = art_cache_.object(key)) {
        return *cached;
    }

    QImage image(px, px, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        QPainterPath path;
        const qreal radius = px * 0.16;
        path.addRoundedRect(QRectF(0, 0, px, px), radius, radius);
        p.setClipPath(path);
        if (source.isNull()) {
            const int hue = static_cast<int>(qHash(title) % 360);
            p.fillRect(QRect(0, 0, px, px), QColor::fromHsl(hue, 90, 90));
            QFont f = font();
            f.setPixelSize(std::max(8, px / 2));
            f.setBold(true);
            p.setFont(f);
            p.setPen(QColor(255, 255, 255, 230));
            const QString initial = title.isEmpty() ? QStringLiteral("?") : title.left(1).toUpper();
            p.drawText(QRect(0, 0, px, px), Qt::AlignCenter, initial);
        } else {
            const QPixmap scaled =
                source.scaled(px, px, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            p.drawPixmap((px - scaled.width()) / 2, (px - scaled.height()) / 2, scaled);
        }
    }
    QPixmap result = QPixmap::fromImage(image);
    result.setDevicePixelRatio(dpr);
    art_cache_.insert(key, new QPixmap(result),
                      static_cast<int>(std::min<qint64>(qint64(px) * px * 4, 1 << 30)));
    return result;
}

void DualPaneStyle::DrawPreview(QPainter& painter, const QModelIndex& index, qreal opacity) {
    if (!index.isValid() || opacity <= 0.0) {
        return;
    }
    const QPalette pal = palette();
    painter.save();
    painter.setOpacity(opacity);

    // Concept top screen: the art centred, the name under it, play time and version below.
    const QRect content =
        top_rect_.adjusted(PanePad, StatusHeight + 4, -PanePad, -PanePad - HintHeight);
    if (content.width() < 40 || content.height() < 40) {
        painter.restore();
        return;
    }
    const int text_w = content.width() * 9 / 10;
    const int gap = std::max(6, content.height() / 30);
    const QString title = TitleOf(index);

    QFont title_font = font();
    title_font.setPixelSize(std::clamp(content.height() / 9, 14, 30));
    title_font.setBold(true);
    const QFontMetrics tfm(title_font);
    const int max_title_lines = LibraryStyleContext::TitleLines(2);
    const QStringList title_lines =
        // Names (B) shows or hides the title; the bottom screen stays icons only.
        show_names_ ? LibraryStyleContext::NameLines(title, title_font, text_w, max_title_lines,
                                                     PanePad / 2)
                    : QStringList{};
    const int title_h = static_cast<int>(title_lines.size()) * tfm.lineSpacing();

    QFont info_font = font();
    info_font.setPixelSize(std::clamp(content.height() / 16, 11, 15));
    info_font.setBold(true);
    const QFontMetrics ifm(info_font);
    QStringList info;
    if (show_info_) {
        const GameInfo game = LibraryStyleContext::Info(index);
        if (game.is_game) {
            info = LibraryStyleContext::InfoLines(
                game, LibraryStyleContext::LayoutInfo(info_font, text_w), info_font);
        }
    }
    const int info_h = static_cast<int>(info.size()) * ifm.lineSpacing();

    // The art keeps a full gap to whatever comes under it; the info sits closer to the title.
    const int info_gap = title_h > 0 ? gap / 2 : gap;
    const int text_h = (title_h > 0 ? gap + title_h : 0) + (info_h > 0 ? info_gap + info_h : 0);
    const int side = std::clamp(content.height() - text_h, 0, content.height() * 62 / 100);
    const int total = side + text_h;
    int y = content.top() + std::max(0, (content.height() - total) / 2);

    if (side > 8) {
        const QRect cover(content.center().x() + 1 - side / 2, y, side, side);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 50));
        painter.drawRoundedRect(QRectF(cover).translated(0, side * 0.03), side * 0.16,
                                side * 0.16);
        painter.drawPixmap(cover.topLeft(), Art(index, side));
    }
    y += side;

    const QRect text_area(content.center().x() + 1 - text_w / 2, content.top(), text_w,
                          content.height());
    if (title_h > 0) {
        y += gap;
        painter.save();
        painter.setClipRect(text_area.adjusted(-PanePad / 2, 0, PanePad / 2, 0));
        LibraryStyleContext::PaintTitle(
            &painter, QRect(text_area.left(), y, text_area.width(), title_h), title,
            pal.text().color(), title_font, Qt::AlignHCenter | Qt::AlignVCenter, max_title_lines,
            index == QModelIndex(shown_) ? this : nullptr, PanePad / 2);
        painter.restore();
        y += title_h;
    }
    if (info_h > 0) {
        y += info_gap;
        painter.setFont(info_font);
        painter.setPen(LibraryStyleContext::MutedText(pal));
        for (const QString& line : info) {
            painter.drawText(QRect(text_area.left(), y, text_area.width(), ifm.lineSpacing()),
                             Qt::AlignHCenter | Qt::AlignVCenter,
                             ifm.elidedText(line, Qt::ElideRight, text_area.width()));
            y += ifm.lineSpacing();
        }
    }
    painter.restore();
}

void DualPaneStyle::DrawGrid(QPainter& painter, const QRect& dirty) {
    const QPalette pal = palette();
    const QColor accent = LibraryStyleContext::Accent(pal);
    const bool dark = IsDark(pal);
    const QFontMetrics fm(font());

    painter.save();
    painter.setClipRect(grid_area_);
    const int total = static_cast<int>(items_.size());
    const int first_line = std::max(0, scroll_ / cell_h_);
    const int last_line = (scroll_ + grid_area_.height()) / cell_h_;
    for (int line = first_line; line <= last_line; ++line) {
        for (int col = 0; col < cols_; ++col) {
            const int row = line * cols_ + col;
            if (row >= total) {
                break;
            }
            const QModelIndex index(items_[static_cast<size_t>(row)]);
            if (!index.isValid()) {
                continue;
            }
            const bool selected = row == current_;
            if (!(selected ? CellRect(row).united(MarkerRect(row)) : CellRect(row)).intersects(dirty)) {
                continue;
            }
            const QRectF shell = ShellRect(row);
            const qreal shell_side = shell.width();
            const QRect tile(static_cast<int>(std::lround(shell.left() + shell_side * 0.1)),
                             static_cast<int>(std::lround(shell.top() + shell_side * 0.1)),
                             icon_px_, icon_px_);

            // Shell: rounded 16 %, padded 10 %, soft shadow and hairline edge.
            QColor card = dark ? QColor(255, 255, 255, 18) : QColor(255, 255, 255, 230);
            QColor card_border = pal.text().color();
            card_border.setAlpha(dark ? 40 : 28);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, dark ? 60 : 40));
            painter.drawRoundedRect(shell.translated(0, shell_side * 0.015), shell_side * 0.16,
                                    shell_side * 0.16);
            painter.setPen(QPen(card_border, 1));
            painter.setBrush(card);
            painter.drawRoundedRect(shell, shell_side * 0.16, shell_side * 0.16);
            painter.drawPixmap(tile.topLeft(), Art(index, icon_px_));

            if (selected) {
                // Four thick rounded L corners 7 % outside the shell, 32 % long (concept).
                const qreal t = std::max(3.0, shell_side * 0.075);
                const qreal o = shell_side * 0.07;
                const qreal len = shell_side * 0.32;
                const qreal r = shell_side * 0.14;
                const QRectF box = shell.adjusted(-o, -o, o, o);
                // Blink: fade to 55 % and back every 1.2 s, eased (concept dsBlink).
                const qreal phase =
                    static_cast<qreal>(QDateTime::currentMSecsSinceEpoch() % 1200) / 1200.0;
                QColor blink = accent;
                blink.setAlphaF(1.0 - 0.45 * (0.5 - 0.5 * std::cos(phase * 6.283185307)));
                painter.setPen(QPen(blink, t, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
                painter.setBrush(Qt::NoBrush);
                const qreal h = t / 2;      // stroke centre inside the box edge
                const qreal ri = std::max(0.0, r - h); // centre-line corner radius
                const auto corner = [&](QPointF c, qreal sx, qreal sy) {
                    // c: box corner; sx/sy: +1 or -1 towards the inside.
                    QPainterPath path;
                    path.moveTo(c.x() + sx * h, c.y() + sy * len);
                    path.lineTo(c.x() + sx * h, c.y() + sy * (h + ri));
                    path.quadTo(c.x() + sx * h, c.y() + sy * h, c.x() + sx * (h + ri),
                                c.y() + sy * h);
                    path.lineTo(c.x() + sx * len, c.y() + sy * h);
                    painter.drawPath(path);
                };
                corner(box.topLeft(), 1, 1);
                corner(box.topRight(), -1, 1);
                corner(box.bottomLeft(), 1, -1);
                corner(box.bottomRight(), -1, -1);
            }
        }
    }
    painter.restore();

    const int lines = (total + cols_ - 1) / std::max(1, cols_);
    const int content = lines * cell_h_;
    if (content > grid_area_.height() && grid_area_.height() > 0 &&
        dirty.right() >= grid_pane_.right() - 8) {
        const qreal ratio = static_cast<qreal>(grid_area_.height()) / content;
        const int bar_h = std::max(24, static_cast<int>(grid_area_.height() * ratio));
        const int range = grid_area_.height() - bar_h;
        const int max_scroll = content - grid_area_.height();
        const int y = grid_area_.top() + (max_scroll > 0 ? range * scroll_ / max_scroll : 0);
        QColor bar = pal.text().color();
        bar.setAlpha(70);
        painter.setPen(Qt::NoPen);
        painter.setBrush(bar);
        painter.drawRoundedRect(QRect(grid_pane_.right() - 8, y, 4, bar_h), 2, 2);
    }
}

void DualPaneStyle::paintEvent(QPaintEvent* event) {
    // The blinking brackets repaint a small rect many times a second: draw only what is in it.
    const QRect dirty = event->rect();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QPalette pal = palette();
    const bool dark = IsDark(pal);

    const QColor pane = dark ? pal.base().color().lighter(112) : pal.base().color();

    // Both screens are the same every frame: draw them once per size and palette.
    const qreal dpr = devicePixelRatioF();
    const QString pane_key = QStringLiteral("%1x%2|%3|%4|%5")
                                 .arg(width())
                                 .arg(height())
                                 .arg(dpr)
                                 .arg(pane.rgba())
                                 .arg(pal.window().color().rgba());
    if (panes_key_ != pane_key) {
        panes_key_ = pane_key;
        panes_ = QPixmap(QSize(std::max(1, static_cast<int>(std::ceil(width() * dpr))),
                               std::max(1, static_cast<int>(std::ceil(height() * dpr)))));
        panes_.setDevicePixelRatio(dpr);
        panes_.fill(pal.window().color());
        QPainter p(&panes_);
        p.setRenderHint(QPainter::Antialiasing);
        for (const QRect& r : {top_rect_, grid_pane_}) {
            if (r.width() <= 0 || r.height() <= 0) {
                continue;
            }
            QPainterPath path;
            path.addRoundedRect(QRectF(r), ScreenRadius, ScreenRadius);
            QColor edge = pal.text().color();
            edge.setAlpha(dark ? 50 : 34);
            p.setPen(QPen(edge, 1));
            p.setBrush(pane);
            p.drawPath(path);
            p.save();
            p.setClipPath(path);
            // The Canvas background in small: the Lichii icon tiled and slanted.
            LibraryStyleContext::PaintLogoTiles(&p, QRectF(r), std::clamp(r.height() / 9.0, 18.0, 40.0),
                                                dark ? 0.07 : 0.10);
            p.restore();
        }
    }
    painter.drawPixmap(0, 0, panes_);

    const bool top_dirty = dirty.intersects(top_rect_);
    if (top_dirty) {
        QColor sub = LibraryStyleContext::MutedText(pal);
        // Status bar: the game count in the blue pill, the date and time on the right.
        QFont f = font();
        f.setPixelSize(std::clamp(top_rect_.height() / 22, 10, 13));
        f.setBold(true);
        painter.setFont(f);
        const QFontMetrics sfm(f);
        const QRect status(top_rect_.left() + 9, top_rect_.top() + 5, top_rect_.width() - 18,
                           StatusHeight - 8);
        const QString count = tr("%n game(s)", nullptr, static_cast<int>(items_.size()));
        const QRectF pill(status.left(), status.top() + 1,
                          std::max<qreal>(sfm.horizontalAdvance(count) + 20,
                                          std::min(status.width() * 0.3, 200.0)),
                          status.height() - 2);
        QLinearGradient blue(pill.topLeft(), pill.bottomLeft());
        blue.setColorAt(0.0, QColor(0x5a, 0xa9, 0xf0));
        blue.setColorAt(1.0, QColor(0x2f, 0x86, 0xdc));
        painter.setPen(Qt::NoPen);
        painter.setBrush(blue);
        painter.drawRoundedRect(pill, 3, 3);
        painter.setPen(Qt::white);
        painter.drawText(pill.adjusted(10, 0, -6, 0), Qt::AlignLeft | Qt::AlignVCenter, count);
        const QLocale locale;
        const QDateTime now = QDateTime::currentDateTime();
        const QString clock = QStringLiteral("%1 (%2) %3")
                                  .arg(locale.toString(now.date(), QLocale::ShortFormat),
                                       locale.dayName(now.date().dayOfWeek(), QLocale::ShortFormat),
                                       locale.toString(now.time(), QLocale::ShortFormat));
        painter.setPen(pal.text().color());
        painter.drawText(status, Qt::AlignRight | Qt::AlignVCenter, clock);

        // Button hints along the bottom of the top screen, centred.
        QFont hint_font = font();
        hint_font.setPixelSize(11);
        hint_font.setBold(true);
        const auto hints = LibraryStyleContext::StandardHints(tr("Size"));
        const qreal radius = 7.0;
        const qreal hints_w = LibraryStyleContext::HintsWidth(hints, hint_font, radius);
        if (hints_w <= top_rect_.width() - 2 * PanePad) {
            LibraryStyleContext::PaintHints(&painter, hints, top_rect_.center().x() + 1 - hints_w / 2,
                                            top_rect_.bottom() + 1 - PanePad / 2 - HintHeight / 2.0,
                                            hint_font, radius, sub);
        }
    }

    if (items_.empty()) {
        painter.setPen(pal.text().color());
        painter.setFont(font());
        painter.drawText(grid_pane_, Qt::AlignCenter, tr("No games found"));
        return;
    }

    if (top_dirty) {
        if (previous_.isValid() && fade_value_ < 1.0) {
            DrawPreview(painter, QModelIndex(previous_), 1.0 - fade_value_);
        }
        DrawPreview(painter, QModelIndex(shown_), previous_.isValid() ? fade_value_ : 1.0);
    }

    if (dirty.intersects(grid_pane_)) {
        DrawGrid(painter, dirty);
    }
}

void DualPaneStyle::resizeEvent(QResizeEvent* event) {
    LibraryStyleView::resizeEvent(event);
    ComputeLayout();
    ClampScroll();
    EnsureVisible();
}

void DualPaneStyle::changeEvent(QEvent* event) {
    LibraryStyleView::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange ||
        event->type() == QEvent::StyleChange) {
        art_cache_.clear();
        ComputeLayout();
        ClampScroll();
        EnsureVisible();
        update();
    }
}

void DualPaneStyle::keyPressEvent(QKeyEvent* event) {
    if (items_.empty()) {
        LibraryStyleView::keyPressEvent(event);
        return;
    }
    const int count = static_cast<int>(items_.size());
    const int page = std::max(1, grid_area_.height() / std::max(1, cell_h_)) * cols_;
    int row = current_ < 0 ? 0 : current_;
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
        row = row - cols_ >= 0 ? row - cols_ : row;
        break;
    case Qt::Key_Down:
    case Qt::Key_S:
        if (row + cols_ < count) {
            row += cols_;
        } else if (row / cols_ < (count - 1) / cols_) {
            row = count - 1;
        }
        break;
    case Qt::Key_PageUp:
        row = std::max(0, row - page);
        break;
    case Qt::Key_PageDown:
        row = std::min(count - 1, row + page);
        break;
    case Qt::Key_Home:
        row = 0;
        break;
    case Qt::Key_End:
        row = count - 1;
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (current_ >= 0) {
            emit LaunchRequested(CurrentIndex());
        }
        return;
    default:
        LibraryStyleView::keyPressEvent(event);
        return;
    }
    SetCurrentRow(row, true);
}

void DualPaneStyle::mousePressEvent(QMouseEvent* event) {
    setFocus();
    const int row = RowAt(event->position().toPoint());
    if (row < 0) {
        LibraryStyleView::mousePressEvent(event);
        return;
    }
    SetCurrentRow(row, true);
    if (event->button() == Qt::RightButton) {
        emit ContextMenuRequested(CurrentIndex(), event->globalPosition().toPoint());
    }
}

void DualPaneStyle::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const int row = RowAt(event->position().toPoint());
    if (row < 0) {
        return;
    }
    SetCurrentRow(row, true);
    emit LaunchRequested(CurrentIndex());
}

void DualPaneStyle::wheelEvent(QWheelEvent* event) {
    int delta = 0;
    if (!event->pixelDelta().isNull()) {
        delta = -event->pixelDelta().y();
    } else {
        delta = -event->angleDelta().y() * cell_h_ / 240;
    }
    const int before = scroll_;
    scroll_ += delta;
    ClampScroll();
    if (scroll_ != before) {
        update(grid_pane_);
    }
    event->accept();
}

namespace {
LibraryStyleView* Make(QWidget* p) {
    return new DualPaneStyle(p);
}
[[maybe_unused]] const bool registered =
    LibraryStyles::Register({"dual_pane", QT_TRANSLATE_NOOP("LibraryStyles", "Dual Pane"), &Make});
} // namespace

} // namespace LibraryStyles
