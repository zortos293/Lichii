// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

#include <QModelIndex>
#include <QPoint>
#include <QString>
#include <QWidget>
#include <QCoreApplication>
#include <QResizeEvent>

class QAbstractItemModel;

namespace LibraryStyles {

/**
 * One library layout ("style"). GameList owns the instance, hands it a flat, filtered model of the
 * library and handles the shared keys (Enter, WASD, I/Y, B, R/X, [ and ]) before the view sees
 * them, so a view only paints, moves its selection with the arrow keys and reports mouse input.
 */
class LibraryStyleView : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;
    ~LibraryStyleView() override = default;

    // The game list model the Modern grid uses (filtered); rows are games/folders with the roles
    // from qt_common/game_list/game_list_p.h.
    virtual void SetModel(QAbstractItemModel* model) = 0;
    virtual QModelIndex CurrentIndex() const = 0;
    virtual void SetCurrentIndex(const QModelIndex& index) = 0;
    virtual void SetShowNames(bool show) = 0;
    virtual void SetShowInfo(bool show) = 0;
    // The global icon size changed (View > Game Icon Size). The default replays a resize, which
    // every style already answers with a relayout and fresh caches.
    virtual void UpdateIconSize() {
        QResizeEvent event(size(), size());
        QCoreApplication::sendEvent(this, &event);
        update();
    }
    // The style's own option (R / X), e.g. scattered on/off. Returns a short label for the current
    // option or empty.
    virtual QString CycleOption() {
        return {};
    }

signals:
    void LaunchRequested(const QModelIndex& index);
    void ContextMenuRequested(const QModelIndex& index, const QPoint& global_pos);
    void CurrentChanged(const QModelIndex& index);
};

using Factory = LibraryStyleView* (*)(QWidget* parent);

struct StyleInfo {
    const char* id;
    const char* display_name;
    Factory factory;
};

// Self-registration: each style's .cpp has
// `static const bool registered = LibraryStyles::Register(
//      {"canvas", QT_TRANSLATE_NOOP("LibraryStyles", "Canvas"), &Make});`
// The display name is translated in the "LibraryStyles" context (see DisplayName); QT_TR_NOOP
// outside a class gives lupdate no such context.
bool Register(const StyleInfo& info);
const std::vector<StyleInfo>& All();

} // namespace LibraryStyles
