// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>
#include <QCache>
#include <QChar>
#include <QColor>
#include <QHash>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QPointer>
#include <QRectF>
#include <QString>

#include "lichii/game/styles/library_style_view.h"

class QAbstractItemModel;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QPainter;
class QResizeEvent;
class QWheelEvent;

namespace LibraryStyles {

/// "Box Shelf": games stand as cases on a wooden table, front or spine out. Concept by inxomnyaa.
class BoxShelfStyle final : public LibraryStyleView {
    Q_OBJECT

public:
    explicit BoxShelfStyle(QWidget* parent = nullptr);
    ~BoxShelfStyle() override;

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

private:
    struct Metrics {
        int cols = 1;
        double left = 0.0;
        double cell_w = 0.0;
        double case_w = 0.0;
        double case_h = 0.0;
        double row_h = 0.0;
        double top_pad = 0.0;
        double face_w = 0.0; // front width of a case at this size (spines: as if facing you)
        double bubble_h = 0.0;
    };

    Metrics ComputeMetrics() const;
    int ItemCount() const {
        return static_cast<int>(m_entries.size());
    }
    int CurrentPos() const;
    QModelIndex IndexAtPos(int pos) const;
    QRectF ItemRect(const Metrics& m, int pos) const;
    int HitTest(const QPointF& point) const;
    double MaxScroll(const Metrics& m) const;
    void ClampScroll();
    void EnsureVisible(int pos);
    void SetCurrentPos(int pos);
    void NotifyCurrent();
    void InvalidateRange(const QModelIndex& top_left, const QModelIndex& bottom_right);

    void ScheduleRebuild();
    void EnsureRows();
    void RebuildRows();

    bool IsDark() const;
    void DrawItem(QPainter& p, const Metrics& m, int pos, bool selected, bool dark);
    /// Front mode: the case turned to show its spine, the selected one facing the viewer.
    void DrawFront(QPainter& p, const Metrics& m, int pos, bool selected);
    /// White name bubble under a case, the same one the Cartridges style uses.
    void DrawNameBubble(QPainter& p, const Metrics& m, const QRectF& base, const QString& title,
                        bool selected);

    QString PixmapKey(QChar kind, const QModelIndex& index, const Metrics& m, bool dark,
                      double dpr) const;
    QPixmap CasePixmap(const QModelIndex& index, const Metrics& m, bool dark);
    QPixmap SpinePixmap(const QModelIndex& index, const Metrics& m, bool dark);

    QPointer<QAbstractItemModel> m_model;
    QPersistentModelIndex m_current;
    QString m_current_key; // identity of the current game, to find it again after a model reset
    std::vector<QPersistentModelIndex> m_entries; // shown games: top level and inside folders
    bool m_rebuild_pending = false;
    bool m_notifying = false;

    bool m_show_names = true;
    bool m_show_info = true;
    bool m_spines = false;
    double m_scroll = 0.0;

    QCache<QString, QPixmap> m_pixmaps; // cost in KiB
    QHash<QString, int> m_gen; // bumped when a game's data changes, part of the pixmap cache key
};

} // namespace LibraryStyles
