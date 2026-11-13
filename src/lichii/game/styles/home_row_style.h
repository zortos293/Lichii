// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Home Row library style. Concept by inxomnyaa.

#pragma once

#include <vector>

#include <QCache>
#include <QModelIndex>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QPointer>
#include <QString>

#include "lichii/game/styles/library_style_view.h"

class QAbstractItemModel;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QPainter;
class QResizeEvent;
class QTimer;
class QVariantAnimation;
class QWheelEvent;

namespace LibraryStyles {

/// One horizontal row of large square covers with an animated scroll offset.
class HomeRowStyle final : public LibraryStyleView {
    Q_OBJECT
public:
    explicit HomeRowStyle(QWidget* parent = nullptr);
    ~HomeRowStyle() override;

    void SetModel(QAbstractItemModel* model) override;
    QModelIndex CurrentIndex() const override;
    void SetCurrentIndex(const QModelIndex& index) override;
    void SetShowNames(bool show) override;
    void SetShowInfo(bool show) override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct Metrics {
        int side;
        int step;
        int center_y;
    };

    Metrics ComputeMetrics() const;
    int RowCount() const;
    int RowAt(const QPoint& pos) const;
    QRectF CoverRect(int row, const Metrics& m) const;
    QString TitleAt(int row) const;
    void MoveTo(int row);
    void AnimateToCurrent(bool animate);
    void ScheduleRebuild();
    void Rebuild();
    QModelIndex IndexAt(int row) const;
    QPixmap CoverPixmap(int row, int side);

    QPointer<QAbstractItemModel> m_model;
    /// Shown games: top level games and the games inside folder rows.
    std::vector<QPersistentModelIndex> m_entries;
    bool m_rebuild_pending = false;
    int m_current = -1;
    double m_offset = 0.0; // in cover steps
    bool m_show_names = true;
    bool m_show_info = true;
    int m_wheel_remainder = 0;
    QVariantAnimation* m_animation = nullptr;
    QTimer* m_clock = nullptr;
    QCache<QString, QPixmap> m_cache{96};
};

} // namespace LibraryStyles
