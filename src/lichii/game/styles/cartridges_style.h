// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Concept by inxomnyaa.

#pragma once

#include <QAbstractItemModel>
#include <QCache>
#include <QHash>
#include <QColor>
#include <QMetaObject>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QPointer>
#include <QString>
#include <QTransform>

#include <vector>

#include "lichii/game/styles/library_style_view.h"

class QScrollBar;

namespace LibraryStyles {

/// Each game is a generic cartridge lying on a wood table.
class CartridgesStyle final : public LibraryStyleView {
    Q_OBJECT

public:
    explicit CartridgesStyle(QWidget* parent = nullptr);
    ~CartridgesStyle() override;

    void SetModel(QAbstractItemModel* model) override;
    QModelIndex CurrentIndex() const override;
    void SetCurrentIndex(const QModelIndex& index) override;
    void SetShowNames(bool show) override;
    void SetShowInfo(bool show) override;
    QString CycleOption() override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    struct Metrics {
        int cols = 1;
        int rows = 0;
        qreal cell_w = 0;
        qreal cell_h = 0;
        qreal card_w = 0;
        qreal card_h = 0;
        qreal margin_x = 0;
        qreal margin_top = 0;
        /// Name bubble size relative to the concept (1.0 on a 160 px cart, down to 0.65).
        qreal bubble_scale = 1.0;
    };
    struct Placement {
        QPointF center;
        qreal angle = 0;
        QTransform transform;
    };

    Metrics ComputeMetrics() const;
    Placement Place(int row, const Metrics& m, bool selected) const;
    int RowCount() const;
    int RowAt(const QPoint& pos) const;
    void UpdateScrollRange();
    void EnsureVisible(int row);
    void SetCurrentRow(int row, bool notify);
    QModelIndex IndexAt(int row) const;
    void ScheduleRebuild();
    void Rebuild();
    void Launch(const QModelIndex& index);

    QPixmap CardPixmap(const QModelIndex& index, const Metrics& m, qreal dpr,
                       bool selected) const;
    void DrawNameBubble(QPainter& painter, int row, const Metrics& m,
                        const Placement& place) const;

    QPointer<QAbstractItemModel> model_;
    std::vector<QMetaObject::Connection> connections_;
    std::vector<QPersistentModelIndex> entries_;
    QPersistentModelIndex current_;
    int current_row_ = -1;
    bool rebuild_pending_ = false;
    double wheel_remainder_ = 0.0;
    QScrollBar* bar_ = nullptr;
    bool show_names_ = true;
    bool show_info_ = true;
    bool scattered_ = false;

    mutable QCache<QString, QPixmap> card_cache_;
};

} // namespace LibraryStyles
