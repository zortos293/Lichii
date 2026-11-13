// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QCache>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QTimer>
#include <QVariantAnimation>
#include <vector>

#include "lichii/game/styles/library_style_view.h"

class QAbstractItemModel;
class QPainter;

namespace LibraryStyles {

/// "Dual Pane": a large preview of the selected game above a compact grid of small icons.
/// Concept by inxomnyaa.
class DualPaneStyle : public LibraryStyleView {
    Q_OBJECT
public:
    explicit DualPaneStyle(QWidget* parent = nullptr);
    ~DualPaneStyle() override;

    void SetModel(QAbstractItemModel* model) override;
    QModelIndex CurrentIndex() const override;
    void SetCurrentIndex(const QModelIndex& index) override;
    void SetShowNames(bool show) override;
    void SetShowInfo(bool show) override;
    QString CycleOption() override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void ScheduleRebuild();
    void Rebuild();
    void ComputeLayout();
    void SetCurrentRow(int row, bool emit_signal);
    void EnsureVisible();
    void ClampScroll();
    int RowAt(const QPoint& pos) const;
    QRect CellRect(int row) const;
    /// The icon's shell inside its cell.
    QRectF ShellRect(int row) const;
    /// What the blinking selection brackets of `row` cover.
    QRect MarkerRect(int row) const;
    QString OptionLabel() const;

    QPixmap Art(const QModelIndex& index, int logical_px);
    void DrawPreview(QPainter& painter, const QModelIndex& index, qreal opacity);
    /// Only the cells inside `dirty` are drawn.
    void DrawGrid(QPainter& painter, const QRect& dirty);

    QAbstractItemModel* model_ = nullptr;
    std::vector<QPersistentModelIndex> items_;
    int current_ = -1;
    QString current_path_;
    QPersistentModelIndex shown_;
    QPersistentModelIndex previous_;

    bool show_names_ = true;
    bool show_info_ = true;
    int icon_level_ = 1; // 0 small, 1 medium, 2 large

    QRect top_rect_;
    QRect grid_pane_;
    QRect grid_area_;
    int icon_px_ = 64;
    int cols_ = 1;
    int cell_w_ = 80;
    int cell_h_ = 80;
    int scroll_ = 0;

    QVariantAnimation fade_;
    qreal fade_value_ = 1.0;
    QTimer clock_timer_;
    QTimer rebuild_timer_;
    QCache<QString, QPixmap> art_cache_;
    /// The shell, both screens and their Lichii pattern, rebuilt when the size or the palette changes.
    QPixmap panes_;
    QString panes_key_;
};

} // namespace LibraryStyles
