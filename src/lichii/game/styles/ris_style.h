// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QList>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QPointer>
#include <QRect>

#include "lichii/game/styles/library_style_view.h"

class QPainter;
class QVariantAnimation;

namespace LibraryStyles {

/**
 * RIS: the branded showcase of RIS (Red Industries Society, PantACRO4life). A header with the RIS
 * name in text and the PantACRO4life tagline, a hero area with the selected game (large cover,
 * title, play time and version, a Play plate), and a horizontal shelf of covers under it. Uses the
 * RIS colors with the RIS theme and the palette of any other theme. The option (R / X) switches
 * the shelf between compact and large covers.
 */
class RisStyle final : public LibraryStyleView {
    Q_OBJECT
public:
    explicit RisStyle(QWidget* parent = nullptr);

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
    void contextMenuEvent(QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    struct Colors {
        QColor background;
        QColor header;
        QColor panel;
        QColor text;
        QColor muted;
        QColor faint;
        QColor accent;
        QColor accent_hot;
    };

    Colors Scheme() const;
    void Rebuild();
    void Relayout();
    void Select(int position);
    /// Moves the shelf so `position` is centered (as far as the ends allow).
    void ScrollShelfTo(int position, bool animate);
    qreal MaxOffset() const;
    int VisibleTiles() const;
    int PositionAt(const QPoint& point) const;

    int TileSize() const;
    int NameHeight() const;
    QRect HeaderRect() const;
    QRect FooterRect() const;
    QRect ShelfRect() const;
    QRect HeroRect() const;
    QRect TileRect(int position) const;
    QRect HeroCoverRect() const;
    QPixmap CachedCover(const QModelIndex& index, const QSize& size) const;

    void PaintHeader(QPainter& painter, const Colors& colors);
    void PaintHero(QPainter& painter, const Colors& colors);
    void PaintShelf(QPainter& painter, const Colors& colors);
    void PaintFooter(QPainter& painter, const Colors& colors);

    QPointer<QAbstractItemModel> model;
    QList<QPersistentModelIndex> entries;
    int current = -1;
    bool rebuild_pending = false;
    bool show_names = true;
    bool show_info = false;
    bool large_shelf = false;
    /// Horizontal scroll of the shelf, in pixels.
    qreal offset = 0;
    QVariantAnimation* scroll_animation = nullptr;
    QString display_family;
    /// Where the Play plate was painted last, for mouse clicks.
    QRect play_rect;
    /// Hero background (grid and glow), rendered once per size and colours.
    QPixmap hero_background;
    QString hero_key;
};

} // namespace LibraryStyles
