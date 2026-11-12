// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#if defined(__linux__) && !defined(__ANDROID__)
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWidget>
#include <QWheelEvent>

namespace ConfigurationShared {

// Route wheel input over closed selectors to the nearest scrollable settings page.
class SettingsWheelFilter final : public QObject {
public:
    explicit SettingsWheelFilter(QWidget* window_) : QObject(window_), window{window_} {}

    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() != QEvent::Wheel || QApplication::activePopupWidget()) {
            return false;
        }

        auto* target = qobject_cast<QWidget*>(object);
        if (!target || target->window() != window) {
            return false;
        }
        bool over_selector = false;
        for (QWidget* widget = target; widget; widget = widget->parentWidget()) {
            over_selector |= qobject_cast<QComboBox*>(widget) ||
                             qobject_cast<QAbstractSpinBox*>(widget);
            auto* scroll = qobject_cast<QAbstractScrollArea*>(widget);
            if (!scroll) {
                continue;
            }
            if (!over_selector || !scroll->property("lichiiSettingsPage").toBool()) {
                return false;
            }
            const auto* wheel = static_cast<QWheelEvent*>(event);
            // QAbstractScrollArea::wheelEvent sends the event to one bar only, chosen by the
            // dominant angle delta, so leave the event alone when that bar cannot move.
            const QPoint angle = wheel->angleDelta();
            const QScrollBar* bar = qAbs(angle.x()) > qAbs(angle.y())
                                        ? scroll->horizontalScrollBar()
                                        : scroll->verticalScrollBar();
            if (bar->minimum() == bar->maximum()) {
                return false;
            }
            const QPointF position = target->mapTo(scroll->viewport(), wheel->position());
            QWheelEvent forwarded(position, wheel->globalPosition(), wheel->pixelDelta(),
                                  wheel->angleDelta(), wheel->buttons(), wheel->modifiers(),
                                  wheel->phase(), wheel->inverted(), wheel->source(),
                                  wheel->pointingDevice());
            QApplication::sendEvent(scroll->viewport(), &forwarded);
            return true;
        }
        return false;
    }

private:
    QWidget* window;
};

// Keep settings at their minimum size and scroll when the viewport is smaller.
inline QScrollArea* CreateScrollArea(QWidget* content, QWidget* parent) {
    if (content->layout()) {
        content->layout()->setSizeConstraint(QLayout::SetMinimumSize);
    }
    auto* scroll = new QScrollArea(parent);
    scroll->setProperty("lichiiSettingsPage", true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);
    QWidget* window = parent->window();
    if (!window->property("lichiiSettingsWheelFilter").toBool()) {
        window->setProperty("lichiiSettingsWheelFilter", true);
        qApp->installEventFilter(new SettingsWheelFilter(window));
    }
    return scroll;
}

// Wrap a settings page without changing pages that are already scroll areas.
inline void MakePageScrollable(QWidget* page) {
    if (!page || !page->layout() || qobject_cast<QScrollArea*>(page)) {
        return;
    }
    auto* content = new QWidget;
    content->setLayout(page->layout());
    auto* scroll = CreateScrollArea(content, page);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);
}

} // namespace ConfigurationShared
#endif
