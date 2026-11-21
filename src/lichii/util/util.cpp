// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2015 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QStyleHints>

#include "applets/qt_profile_select.h"
#include "core/frontend/applets/profile_select.h"
#include "core/hle/service/acc/profile_manager.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"
#include "lichii/util/util.h"

#ifdef _WIN32
#include <windows.h>
#include "common/fs/file.h"
#endif

QFont GetMonospaceFont() {
    QFont font(QStringLiteral("monospace"));
    // Automatic fallback to a monospace font on on platforms without a font called "monospace"
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    return font;
}

namespace {

QPixmap TintedPixmap(const QPixmap& source, const QColor& color) {
    if (source.isNull()) {
        return {};
    }
    QImage image = source.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    // Paint in physical pixels; the DPR is restored on the pixmap below.
    image.setDevicePixelRatio(1.0);
    QPainter painter(&image);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(image.rect(), color);
    painter.end();
    QPixmap result = QPixmap::fromImage(image);
    result.setDevicePixelRatio(source.devicePixelRatio());
    return result;
}

QIcon BuildMenuIcon(const char* name, bool dark_icons) {
    QIcon icon;
    if (UISettings::values.theme != "lichii") {
        return icon;
    }

    const QString base = QStringLiteral(":/lichii/icons/menu_%1").arg(QLatin1String(name));
    QPixmap source = QtCommon::LoadPixmapForDpr(base + QStringLiteral("@2x.png"), 2.0);
    if (source.isNull()) {
        source = QPixmap(base + QStringLiteral(".png"));
    }
    if (source.isNull()) {
        return icon;
    }

    // Match the menu text: light icons on a dark menu, dark icons on a light menu. The
    // highlighted row is always white, whatever the menu background.
    const QColor normal = dark_icons ? QColor(0xec, 0xec, 0xef) : QColor(0x1d, 0x1d, 0x1f);
    icon.addPixmap(TintedPixmap(source, normal), QIcon::Normal);
    const QPixmap selected = TintedPixmap(source, Qt::white);
    icon.addPixmap(selected, QIcon::Active);
    icon.addPixmap(selected, QIcon::Selected);
    return icon;
}

bool MenuUsesDarkIcons() {
#if defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    // The macOS menu bar and QMenu popups are native and follow the system appearance, not the
    // app's Lichii palette.
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme != Qt::ColorScheme::Unknown) {
        return scheme == Qt::ColorScheme::Dark;
    }
#endif
    return UISettings::IsDarkTheme();
}

} // namespace

QIcon MenuIcon(const char* name) {
    return BuildMenuIcon(name, MenuUsesDarkIcons());
}

QIcon WidgetIcon(const char* name) {
    return BuildMenuIcon(name, UISettings::IsDarkTheme());
}

QPixmap ThemePixmap(const char* name, const QSize& logical_size, qreal dpr) {
    if (UISettings::values.theme != "lichii") {
        return {};
    }

    QPixmap source = QtCommon::LoadPixmapForDpr(
        QStringLiteral(":/lichii/icons/%1@2x.png").arg(QLatin1String(name)), dpr);
    if (source.isNull() || logical_size.isEmpty()) {
        return source;
    }
    return QtCommon::ScalePixmapForDpr(source, logical_size, dpr);
}

const std::optional<Common::UUID> GetProfileID() {
    // if there's only a single profile, the user probably wants to use that... right?
    const auto& profiles = QtCommon::system->GetProfileManager().FindExistingProfileUUIDs();
    if (profiles.size() == 1) {
        return profiles[0];
    }

    const auto select_profile = [] {
        const Core::Frontend::ProfileSelectParameters parameters{
            .mode = Service::AM::Frontend::UiMode::UserSelector,
            .invalid_uid_list = {},
            .display_options = {},
            .purpose = Service::AM::Frontend::UserSelectionPurpose::General,
        };
        QtProfileSelectionDialog dialog(*QtCommon::system, QtCommon::rootObject, parameters);
        dialog.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                              Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint);
        dialog.setWindowModality(Qt::WindowModal);

        if (dialog.exec() == QDialog::Rejected) {
            return -1;
        }

        return dialog.GetIndex();
    };

    const auto index = select_profile();
    if (index == -1) {
        return std::nullopt;
    }

    const auto uuid =
        QtCommon::system->GetProfileManager().GetUser(static_cast<std::size_t>(index));
    ASSERT(uuid);

    return uuid;
}
std::string GetProfileIDString() {
    const auto uuid = GetProfileID();
    if (!uuid)
        return "";

    auto user_id = uuid->AsU128();

    return fmt::format("{:016X}{:016X}", user_id[1], user_id[0]);
}
