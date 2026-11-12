// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <deque>
#include <optional>
#include <vector>
#include <QColor>
#include <QWidget>

#include "common/dkb_cpu_trace.h"
#include "core/perf_stats.h"

namespace VideoCore {
class ShaderNotify;
}

class MainWindow;

class PerformanceOverlay : public QWidget {
    Q_OBJECT

    // The colours are properties so that a theme's stylesheet can set them:
    //   PerformanceOverlay { qproperty-accentColor: #82a88b; qproperty-cornerRadius: 8; }
    Q_PROPERTY(QColor backgroundColor MEMBER m_backgroundColor)
    Q_PROPERTY(QColor borderColor MEMBER m_borderColor)
    Q_PROPERTY(QColor textColor MEMBER m_textColor)
    Q_PROPERTY(QColor mutedColor MEMBER m_mutedColor)
    Q_PROPERTY(QColor dimColor MEMBER m_dimColor)
    Q_PROPERTY(QColor accentColor MEMBER m_accentColor)
    Q_PROPERTY(QColor spikeColor MEMBER m_spikeColor)
    Q_PROPERTY(QColor lineColor MEMBER m_lineColor)
    Q_PROPERTY(QColor axisColor MEMBER m_axisColor)
    Q_PROPERTY(QColor medianLineColor MEMBER m_medianLineColor)
    Q_PROPERTY(qreal cornerRadius MEMBER m_cornerRadius)

public:
    explicit PerformanceOverlay(MainWindow* parent = nullptr);

    /// Back to the built-in colours. Call before a new stylesheet is applied: a stylesheet only
    /// sets the properties it names.
    void ResetThemeColors();

protected:
    void paintEvent(QPaintEvent* event) override;
    void hideEvent(QHideEvent* event) override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void resetPosition(const QPoint& pos);
    void updateStats(const Core::PerfStatsResults& results, const VideoCore::ShaderNotify& shaders);
    void paintGraph(QPainter& painter, const QRectF& area);

    MainWindow* m_mainWindow = nullptr;

    QPoint m_offset{25, 75};

    // drag
    QPoint m_drag_start_pos;

    std::optional<double> m_fps;
    /// The last 5 s of 500 ms fps readings, for min/median/max
    std::deque<float> m_fpsSamples;
    std::optional<double> m_frameMs;
    std::optional<double> m_cpuPercent;
    std::optional<double> m_gpuPercent;
    std::optional<Common::DkbCpuTrace::Snapshot> m_cpuSnapshot;
    std::vector<Core::FrameTimeHistory::Entry> m_frames;
    QString m_tags;

    QColor m_backgroundColor;
    QColor m_borderColor;
    QColor m_textColor;
    QColor m_mutedColor;
    QColor m_dimColor;
    QColor m_accentColor;
    QColor m_spikeColor;
    QColor m_lineColor;
    QColor m_axisColor;
    QColor m_medianLineColor;
    qreal m_cornerRadius = 5.0;

signals:
    void closed();
};
