// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QPixmap>
#include <QPointer>
#include <QPolygonF>
#include <QString>
#include <QTimer>

#include "lichii/game/styles/library_style_view.h"

class QAbstractItemModel;
class QPainter;
class QVariantAnimation;

namespace LibraryStyles {

/// "Showcase" library style: one large selected cover with the neighbours stacked behind it.
/// Concept by inxomnyaa.
class CarouselStyle final : public LibraryStyleView {
    Q_OBJECT
public:
    explicit CarouselStyle(QWidget* parent = nullptr);
    ~CarouselStyle() override;

    void SetModel(QAbstractItemModel* model) override;
    QModelIndex CurrentIndex() const override;
    void SetCurrentIndex(const QModelIndex& index) override;
    void SetShowNames(bool show) override;
    void SetShowInfo(bool show) override;
    QString CycleOption() override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    struct Tint {
        QColor color;
        QPixmap blur;
    };
    struct Hit {
        int row;
        QPolygonF polygon;
    };
    struct Metrics {
        qreal side = 0;
        qreal center_x = 0;
        qreal center_y = 0;
        qreal title_y = 0;
        qreal title_px = 0;
        qreal info_y = 0;
        qreal info_px = 0;
        qreal dots_y = 0;
    };

    void ScheduleRebuild();
    void Rebuild();
    void EnsureBuilt();
    void SelectRow(int row, bool animate = true);
    void MoveBy(int delta);
    /// One step of a held key: lands on its game before the next repeat comes.
    void RepeatBy(int delta);
    void MoveSteps(int step, bool animate, int duration_ms);
    void Launch(int row);
    int HitRow(const QPoint& point) const;

    Metrics ComputeMetrics() const;
    QModelIndex IndexAt(int row) const;
    QPixmap SourceArt(int row, bool* has_art) const;
    QPixmap CoverPixmap(int row, int side);
    const Tint& TintFor(int row);
    /// The full tinted background for a selected cover, cached for the current size.
    QPixmap BackgroundFor(int row);
    /// A neighbour cover at rest, already tilted towards the selection, with its shadow and
    /// reflection, cached.
    QPixmap TiltedPixmap(int row, bool right, qreal side);
    QString TitleFor(int row) const;
    /// Drops cached covers that are more than two slots outside the covers of this frame.
    void TrimCaches(int lo, int hi);
    /// Builds one cover the next step will need (the covers just outside the visible ones and
    /// the selection turned either way), then runs again until none is missing. Moving to the
    /// next game then finds its covers ready instead of building them in that frame.
    void Prefetch();

    void PaintBackground(QPainter& painter);
    QPixmap Reflection(const QPixmap& cover);
    const QPixmap& ShadowFor(qreal side, bool selected);
    void PaintCover(QPainter& painter, const Metrics& metrics, int row, qreal offset);
    void PaintTexts(QPainter& painter, const Metrics& metrics);
    void PaintDots(QPainter& painter, const Metrics& metrics);

    QPointer<QAbstractItemModel> model_;
    std::vector<QPersistentModelIndex> items_;
    int current_ = 0;
    /// Unwrapped: the ring keeps counting past the last game; current_ is target_position_
    /// wrapped into the game list.
    qreal position_ = 0.0;
    qreal start_position_ = 0.0;
    qreal target_position_ = 0.0;
    qreal progress_ = 1.0;
    int previous_row_ = -1;

    bool show_names_ = true;
    bool show_info_ = true;
    bool flat_ = false;

    bool dirty_ = false;
    bool change_pending_ = false;
    QTimer rebuild_timer_;
    QTimer prefetch_timer_;
    QVariantAnimation* animation_ = nullptr;

    QHash<int, QPixmap> cover_cache_;
    QHash<int, Tint> tint_cache_;
    /// Finished background per selected row at the current widget size: blurred art, wash, glow
    /// and fade composed once, so a frame only blits one or two pixmaps.
    QHash<int, QPixmap> background_cache_;
    QHash<qint64, QPixmap> reflection_cache_;
    QPixmap shadow_;
    QPixmap shadow_selected_;
    int shadow_side_[2] = {0, 0};
    int cover_cache_side_ = 0;
    QHash<qint64, QPixmap> tilted_cache_;
    int tilted_side_ = 0;

    std::vector<Hit> hits_;
    int press_row_ = -1;
    bool press_was_selected_ = false;
    int last_launch_row_ = -1;
    QElapsedTimer launch_timer_;
    int wheel_accum_ = 0;
    /// Time since the last step of the arrow keys, to fit the steps of a held key.
    QElapsedTimer key_step_clock_;
};

} // namespace LibraryStyles
