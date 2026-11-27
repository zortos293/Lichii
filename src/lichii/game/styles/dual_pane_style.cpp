// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Dual Pane library style. Concept by inxomnyaa.

#include <algorithm>
#include <vector>
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

    page_anim_.setDuration(220);
    page_anim_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&page_anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        page_scroll_ = value.toReal();
        update(grid_pane_);
    });
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
            // Set from outside (the game list restoring the selection after a switch): no fade,
            // or the top screen showed the first game for a moment before fading to this one.
            SetCurrentRow(static_cast<int>(i), true, false);
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
    return side_by_side_ ? tr("Layout: side by side") : tr("Layout: stacked");
}

QString DualPaneStyle::CycleOption() {
    side_by_side_ = !side_by_side_;
    page_anim_.stop();
    art_cache_.clear();
    ComputeLayout();
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

    // The games, flat and favorites first (shared by every style).
    for (const QModelIndex& game : LibraryStyleContext::Games(model_)) {
        items_.emplace_back(game);
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
    if (side_by_side_) {
        // X: the screens side by side (a phone held sideways), the info screen on the left.
        const int total_w = std::max(0, w - 3 * Margin);
        const int top_w = static_cast<int>(total_w * 0.45);
        top_rect_ = QRect(Margin, Margin, top_w, std::max(0, h - 2 * Margin));
        grid_pane_ = QRect(top_rect_.right() + 1 + Margin, Margin, std::max(0, total_w - top_w),
                           top_rect_.height());
    } else {
        // Two screens across the whole view, the top one a bit shorter (no console frame).
        const int total_h = std::max(0, h - 3 * Margin);
        const int top_h = static_cast<int>(total_h * 0.45);
        top_rect_ = QRect(Margin, Margin, std::max(0, w - 2 * Margin), top_h);
        grid_pane_ = QRect(Margin, top_rect_.bottom() + 1 + Margin, top_rect_.width(),
                           std::max(0, total_h - top_h));
    }
    // One row more per size step: Full Size 1, Cover 2, Large 3, Standard 4, Medium 5, Small 6.
    const int art = LibraryStyleContext::ArtSize();
    lines_ = art >= 256 ? 1 : art >= 160 ? 2 : art >= 128 ? 3 : art >= 64 ? 4 : art >= 44 ? 5 : 6;
    // The rows fill the screen's height. Concept tile: the art is 80 % of a padded shell, and
    // shells keep a fifth of a shell between them, across and down alike. The top and bottom
    // margin match that gap (at least clear of the page arrows); the width the columns leave
    // over is split between the left and right margin, like Channels centres its pages.
    const qreal pane_w = grid_pane_.width();
    const qreal pane_h = grid_pane_.height();
    const qreal arrow_room = ArrowRect(false).width() + 6.0;
    qreal shell = pane_h / (lines_ + (lines_ + 1) / 5.0);
    qreal margin = shell / 5.0;
    if (margin < arrow_room) {
        margin = arrow_room;
        shell = std::max(8.0, (pane_h - 2 * margin) / (lines_ + (lines_ - 1) / 5.0));
    }
    const qreal gap = shell / 5.0;
    cols_ = std::max(1, static_cast<int>((pane_w - 2 * margin + gap) / (shell + gap)));
    cell_w_ = std::max(1, static_cast<int>(std::lround(shell + gap)));
    cell_h_ = cell_w_;
    icon_px_ = std::max(16, static_cast<int>(shell * 0.8));
    const qreal left = (pane_w - cols_ * cell_w_) / 2.0;
    grid_area_ = QRect(grid_pane_.left() + static_cast<int>(std::lround(left)),
                       grid_pane_.top() + static_cast<int>(std::lround(margin - gap / 2)),
                       cols_ * cell_w_, lines_ * cell_h_);
    per_page_ = lines_ * cols_;
    ClampScroll();
}

int DualPaneStyle::PageCount() const {
    return std::max(1, (static_cast<int>(items_.size()) + per_page_ - 1) / per_page_);
}

int DualPaneStyle::ShownPage() const {
    const int w = std::max(1, grid_area_.width());
    return std::clamp(static_cast<int>(std::lround(page_scroll_ / w)), 0, PageCount() - 1);
}

QRect DualPaneStyle::ArrowRect(bool right) const {
    // A slim tab at the screen's side, scaled a little with the screen.
    const qreal s = std::clamp(grid_pane_.height() / 420.0, 0.7, 1.4);
    const int aw = static_cast<int>(std::lround(24 * s));
    const int ah = static_cast<int>(std::lround(84 * s));
    const int x = right ? grid_pane_.right() + 1 - aw : grid_pane_.left();
    return QRect(x, grid_pane_.center().y() - ah / 2, aw, ah);
}

void DualPaneStyle::TurnPage(int step) {
    if (items_.empty()) {
        return;
    }
    const int page = std::clamp(ShownPage() + step, 0, PageCount() - 1);
    if (page == ShownPage()) {
        return;
    }
    // The same place on the other page, or its last game.
    const int k = current_ < 0 ? 0 : current_ % per_page_;
    const int row = std::min(static_cast<int>(items_.size()) - 1, page * per_page_ + k);
    SetCurrentRow(row, true, true);
}

QRect DualPaneStyle::CellRect(int row) const {
    // Pages side by side like Channels, each filled row by row, the rows centred in the screen.
    const int page = row / per_page_;
    const int k = row % per_page_;
    return QRect(grid_area_.left() + page * grid_area_.width() + (k % cols_) * cell_w_ -
                     static_cast<int>(std::lround(page_scroll_)),
                 PageTop(page) + (k / cols_) * cell_h_, cell_w_, cell_h_);
}

int DualPaneStyle::PageTop([[maybe_unused]] int page) const {
    // A fixed grid from the top left, like Channels and Canvas: a short page keeps its rows at
    // the top.
    return grid_area_.top();
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
    const int y = pos.y() - PageTop(ShownPage());
    const int line = y < 0 ? -1 : y / cell_h_;
    if (col < 0 || col >= cols_ || line < 0 || line >= lines_) {
        return -1;
    }
    const int row = ShownPage() * per_page_ + line * cols_ + col;
    return row < static_cast<int>(items_.size()) ? row : -1;
}

void DualPaneStyle::ClampScroll() {
    page_scroll_ = std::clamp(page_scroll_, 0.0,
                              static_cast<qreal>((PageCount() - 1) * grid_area_.width()));
}

void DualPaneStyle::EnsureVisible(bool animate) {
    if (current_ < 0 || grid_area_.width() <= 0) {
        return;
    }
    const qreal target = static_cast<qreal>((current_ / per_page_) * grid_area_.width());
    const qreal end =
        page_anim_.state() == QAbstractAnimation::Running ? page_anim_.endValue().toReal()
                                                           : page_scroll_;
    if (std::abs(end - target) < 0.5) {
        return;
    }
    page_anim_.stop();
    if (!animate || !isVisible()) {
        page_scroll_ = target;
        return;
    }
    page_anim_.setStartValue(page_scroll_);
    page_anim_.setEndValue(target);
    StartAnimation(page_anim_);
}

void DualPaneStyle::SetCurrentRow(int row, bool emit_signal, bool animate) {
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
    previous_ = animate ? old_shown : QPersistentModelIndex{};
    fade_value_ = previous_.isValid() ? 0.0 : 1.0;
    if (previous_.isValid()) {
        StartAnimation(fade_);
    }
    EnsureVisible(animate);
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
                            ? QStringLiteral("n|%1|%2|%3")
                                  .arg(title)
                                  .arg(px)
                                  .arg(palette().window().color().rgb())
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
            // No art: the coloured placeholder with initials every other style uses.
            LibraryStyleContext::PaintPlaceholderArt(&p, QRect(0, 0, px, px), title, palette());
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
    // The games in view: the pages a slide shows.
    std::vector<int> rows;
    {
        const qreal w = std::max(1, grid_area_.width());
        const int first = std::max(0, static_cast<int>(std::floor(page_scroll_ / w)));
        const int last = std::min(PageCount() - 1, static_cast<int>(std::ceil(page_scroll_ / w)));
        for (int row = first * per_page_; row < std::min(total, (last + 1) * per_page_); ++row) {
            rows.push_back(row);
        }
    }
    for (const int row : rows) {
        {
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
            if (LibraryStyleContext::IsFavorite(index)) {
                LibraryStyleContext::PaintFavoriteHeart(&painter, QRectF(tile));
            }

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

    {
        // Page arrows while there is a page to that side.
        const int page = ShownPage();
        for (const bool right : {false, true}) {
            if (right ? page >= PageCount() - 1 : page <= 0) {
                continue;
            }
            const QRectF a = ArrowRect(right);
            if (!a.toAlignedRect().adjusted(-6, -6, 6, 6).intersects(dirty)) {
                continue;
            }
            // Concept: a tab on the screen's side with rounded inner corners and an arrow in
            // it. Here in the icon shells' material (their fill and hairline edge), so it
            // belongs to the screen instead of standing out.
            QPainterPath tab;
            const qreal radius = a.width() * 0.95;
            tab.addRoundedRect(right ? a.adjusted(0, 0, a.width(), 0)
                                     : a.adjusted(-a.width(), 0, 0, 0),
                               radius, radius);
            QPainterPath screen;
            screen.addRoundedRect(QRectF(grid_pane_), ScreenRadius, ScreenRadius);
            tab = tab.intersected(screen);
            QColor fill = dark ? QColor(255, 255, 255, 18) : QColor(255, 255, 255, 230);
            QColor edge = pal.text().color();
            edge.setAlpha(dark ? 40 : 28);
            painter.save();
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, dark ? 60 : 40));
            painter.drawPath(tab.translated(0, a.height() * 0.015 + 1));
            painter.setPen(QPen(edge, 1));
            painter.setBrush(fill);
            painter.drawPath(tab);
            // A chevron pointing out of the screen.
            const qreal cw = a.width() * 0.3;
            const qreal ch = a.width() * 0.32;
            const QPointF c(a.center().x() + (right ? a.width() * 0.06 : -a.width() * 0.06),
                            a.center().y());
            const qreal dir = right ? 1.0 : -1.0;
            QPainterPath arrow;
            arrow.moveTo(c.x() - dir * cw / 2, c.y() - ch);
            arrow.lineTo(c.x() + dir * cw / 2, c.y());
            arrow.lineTo(c.x() - dir * cw / 2, c.y() + ch);
            QColor ink = LibraryStyleContext::MutedText(pal);
            ink.setAlpha(230);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(ink, std::max(2.0, a.width() * 0.1), Qt::SolidLine, Qt::RoundCap,
                                Qt::RoundJoin));
            painter.drawPath(arrow);
            painter.restore();
        }
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
    const QString pane_key = QStringLiteral("%1x%2|%3|%4|%5|%6")
                                 .arg(side_by_side_ ? 1 : 0)
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
        const auto hints = LibraryStyleContext::StandardHints(tr("Layout"));
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
    {
        // Pages side by side: Left/Right run on to the next page, Up/Down stay on the page.
        const int k = row % per_page_;
        const int line = k / cols_;
        const int col = k % cols_;
        const int page_start = row - k;
        switch (event->key()) {
        case Qt::Key_Left:
            if (col > 0) {
                row -= 1;
            } else if (page_start > 0) {
                row = page_start - per_page_ + line * cols_ + cols_ - 1;
            } else if (row > 0) {
                row -= 1;
            }
            break;
        case Qt::Key_Right:
            if (col < cols_ - 1 && row + 1 < count) {
                row += 1;
            } else if (page_start + per_page_ < count) {
                row = std::min(count - 1, page_start + per_page_ + line * cols_);
            } else if (row + 1 < count) {
                row += 1;
            }
            break;
        case Qt::Key_Up:
            row = line > 0 ? row - cols_ : row;
            break;
        case Qt::Key_Down:
            row = line < lines_ - 1 && row + cols_ < count ? row + cols_ : row;
            break;
        case Qt::Key_PageUp:
            TurnPage(-1);
            return;
        case Qt::Key_PageDown:
            TurnPage(1);
            return;
        default:
            row = -1;
            break;
        }
        if (row >= 0) {
            SetCurrentRow(row, true, !event->isAutoRepeat());
            return;
        }
        row = current_ < 0 ? 0 : current_;
    }
    switch (event->key()) {
    case Qt::Key_Left:
        row = std::max(0, row - 1);
        break;
    case Qt::Key_Right:
        row = std::min(count - 1, row + 1);
        break;
    case Qt::Key_Up:
        row = row - cols_ >= 0 ? row - cols_ : row;
        break;
    case Qt::Key_Down:
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
    SetCurrentRow(row, true, !event->isAutoRepeat());
}

void DualPaneStyle::mousePressEvent(QMouseEvent* event) {
    setFocus();
    if (event->button() == Qt::LeftButton) {
        const int page = ShownPage();
        if (page > 0 &&
            ArrowRect(false).adjusted(-6, -6, 6, 6).contains(event->position().toPoint())) {
            TurnPage(-1);
            return;
        }
        if (page < PageCount() - 1 &&
            ArrowRect(true).adjusted(-6, -6, 6, 6).contains(event->position().toPoint())) {
            TurnPage(1);
            return;
        }
    }
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
    // A page per notch (either wheel axis).
    wheel_pages_ += event->angleDelta().y() + event->angleDelta().x();
    while (std::abs(wheel_pages_) >= 120) {
        const int step = wheel_pages_ > 0 ? -1 : 1;
        wheel_pages_ += step * 120;
        TurnPage(step);
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
