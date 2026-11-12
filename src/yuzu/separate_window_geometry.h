// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>

#include <QByteArray>
#include <QRect>
#include <QSize>
#include <QWidget>

namespace SeparateWindowGeometry {

inline void Backup(QWidget& window, QByteArray& saved) {
    if (window.parentWidget() == nullptr) {
        saved = window.saveGeometry();
    }
}

inline void Load(QWidget& window, QByteArray& saved, const QByteArray& stored) {
    if (!stored.isEmpty() && window.restoreGeometry(stored)) {
        saved = stored;
    } else {
        saved.clear();
    }
}

inline void Restore(QWidget& window, QByteArray& saved, const QRect& available) {
    if (!saved.isEmpty() && window.restoreGeometry(saved)) {
        return;
    }
    if (!available.isValid()) {
        return;
    }
    const QSize limit{(std::max)(1, available.width() * 4 / 5),
                      (std::max)(1, available.height() * 4 / 5)};
    QSize size{1280, 720};
    if (size.width() > limit.width() || size.height() > limit.height()) {
        size.scale(limit, Qt::KeepAspectRatio);
    }
    QRect initial{QPoint{}, size};
    initial.moveCenter(available.center());
    window.setGeometry(initial);
    Backup(window, saved);
}

} // namespace SeparateWindowGeometry
