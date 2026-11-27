// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

#include <QAbstractAnimation>
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
    /// Whether the style's option is on (see CycleOption); the game list keeps it per style.
    virtual bool OptionOn() const {
        return false;
    }
    /// The style's look option (+), e.g. Showcase's covers, cartridges or cases. Returns a short
    /// label for the new look, or empty when the style has none.
    virtual QString CycleLook() {
        return {};
    }
    virtual int Look() const {
        return 0;
    }
    void SetLook(int look) {
        for (int i = 0; i < 8 && Look() != look; ++i) {
            if (CycleLook().isEmpty()) {
                return;
            }
        }
    }
    void SetOptionOn(bool on) {
        if (OptionOn() != on) {
            CycleOption();
        }
    }

    /// While one lives, changes come from outside the view (the game list restoring the
    /// selection after a style switch, a search jump): every animation lands at its end at once.
    /// One rule for all styles, so none slides from the first game to the restored one.
    class Quiet {
    public:
        explicit Quiet(LibraryStyleView* view) : view_{view}, was_{view->quiet_} {
            view_->quiet_ = true;
        }
        ~Quiet() {
            view_->quiet_ = was_;
        }
        Quiet(const Quiet&) = delete;
        Quiet& operator=(const Quiet&) = delete;

    private:
        LibraryStyleView* view_;
        bool was_;
    };

protected:
    /// Every style starts its animations here. Quiet, or not shown yet: the animation runs to its
    /// end at once, so the view ends up in the same state (and the same signals fire) without
    /// the movement.
    void StartAnimation(QAbstractAnimation& animation) {
        animation.start();
        if ((quiet_ || !isVisible()) && animation.totalDuration() >= 0) { // not endless loops
            animation.setCurrentTime(animation.totalDuration());
        }
    }

public:
private:
    bool quiet_ = false;

signals:
    void LaunchRequested(const QModelIndex& index);
    void ContextMenuRequested(const QModelIndex& index, const QPoint& global_pos);
    void CurrentChanged(const QModelIndex& index);
    /// The profile picture was clicked (Home Row, Channels): pick the active profile.
    void ProfileSelectRequested();
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
