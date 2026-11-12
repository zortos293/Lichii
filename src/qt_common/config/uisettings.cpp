// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <QDir>
#include <QSettings>
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "qt_common/config/uisettings.h"

#ifndef CANNOT_EXPLICITLY_INSTANTIATE
namespace Settings {
template class Setting<bool>;
template class Setting<std::string>;
template class Setting<u16, true>;
template class Setting<u32>;
template class Setting<u8, true>;
template class Setting<u8>;
template class Setting<unsigned long long>;
} // namespace Settings
#endif

namespace FS = Common::FS;

namespace UISettings {

// This shouldn't have anything except static initializers (no functions). So
// QKeySequence(...).toString() is NOT ALLOWED HERE.
// This must be in alphabetical order according to action name as it must have the same order as
// UISetting::values.shortcuts, which is alphabetically ordered.
// clang-format off
const std::array<Shortcut, 32> default_hotkeys{{
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Audio Mute/Unmute")).toStdString(),          QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Ctrl+M"),  std::string("Home+Dpad_Right"), Qt::WindowShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Audio Volume Down")).toStdString(),          QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("-"),       std::string("Home+Dpad_Down"), Qt::ApplicationShortcut, true}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Audio Volume Up")).toStdString(),            QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("="),       std::string("Home+Dpad_Up"), Qt::ApplicationShortcut, true}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Capture Screenshot")).toStdString(),         QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Ctrl+P"),  std::string("Screenshot"), Qt::WidgetWithChildrenShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Change Adapting Filter")).toStdString(),     QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("F8"),      std::string("Home+L"), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Change Docked Mode")).toStdString(),         QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("F10"),     std::string("Home+X"), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Change GPU Mode")).toStdString(),            QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("F9"),      std::string("Home+R"), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Configure")).toStdString(),                  QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Ctrl+,"),  std::string(""), Qt::WidgetWithChildrenShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Configure Current Game")).toStdString(),     QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Ctrl+."),  std::string(""), Qt::WidgetWithChildrenShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Continue/Pause Emulation")).toStdString(),   QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("F4"),      std::string("Home+Plus"), Qt::WindowShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Exit Fullscreen")).toStdString(),            QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Esc"),     std::string(""), Qt::WindowShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Exit Eglantine")).toStdString(),             QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Ctrl+Q"),  std::string("Home+Minus"), Qt::WindowShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Fullscreen")).toStdString(),                 QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("F11"),     std::string("Home+B"), Qt::WindowShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Load File")).toStdString(),                  QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Ctrl+O"),  std::string(""), Qt::WidgetWithChildrenShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Browse Public Game Lobby")).toStdString(),   QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Ctrl+B"),  std::string(""), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Create Room")).toStdString(),                QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Ctrl+N"),  std::string(""), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Direct Connect to Room")).toStdString(),     QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Ctrl+C"),  std::string(""), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Leave Room")).toStdString(),                 QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Ctrl+L"),  std::string(""), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Show Current Room")).toStdString(),          QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Ctrl+R"),  std::string(""), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Restart Emulation")).toStdString(),          QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("F6"),      std::string("R+Plus+Minus"), Qt::WindowShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Stop Emulation")).toStdString(),             QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("F5"),      std::string("L+Plus+Minus"), Qt::WindowShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "TAS Record")).toStdString(),                 QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Ctrl+F7"), std::string(""), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "TAS Reset")).toStdString(),                  QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Ctrl+F6"), std::string(""), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "TAS Start/Stop")).toStdString(),             QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Ctrl+F5"), std::string(""), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Toggle Filter Bar")).toStdString(),          QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Ctrl+F"),  std::string(""), Qt::WindowShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Toggle Framerate Limit")).toStdString(),     QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Ctrl+U"),  std::string("Home+Y"), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Toggle Turbo Speed")).toStdString(),         QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Ctrl+Z"),  std::string(""), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Toggle Slow Speed")).toStdString(),          QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Ctrl+X"),  std::string(""), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Toggle Mouse Panning")).toStdString(),       QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Ctrl+F9"), std::string(""), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Toggle Renderdoc Capture")).toStdString(),   QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string(""),        std::string(""), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Toggle Status Bar")).toStdString(),          QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Ctrl+S"),  std::string(""), Qt::WindowShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Toggle Performance Overlay")).toStdString(), QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")).toStdString(), {std::string("Ctrl+V"),  std::string(""), Qt::WindowShortcut, false}},
}};
// clang-format on

const Themes themes{{
    {"Default", "default"},
    {"Default Colorful", "colorful"},
    {"Dark", "qdarkstyle"},
    {"Dark Colorful", "colorful_dark"},
    {"Midnight Blue", "qdarkstyle_midnight_blue"},
    {"Midnight Blue Colorful", "colorful_midnight_blue"},
    {"Lichii Dark", "lichii"},
}};

namespace {

std::optional<CustomTheme> active_custom_theme;

bool IsDarkBuiltinTheme(const std::string& theme) {
    return theme == std::string("qdarkstyle") || theme == std::string("qdarkstyle_midnight_blue") ||
           theme == std::string("colorful_dark") ||
           theme == std::string("colorful_midnight_blue") || theme == std::string("lichii");
}

bool IsBuiltinTheme(const QString& id) {
    return std::any_of(themes.begin(), themes.end(), [&id](const auto& theme) {
        return id == QString::fromUtf8(theme.second);
    });
}

/// A theme folder holds style.qss and, optionally, theme.ini:
///   [Theme]
///   name=My Theme     ; shown in the theme list, the folder's name by default
///   base=lichii       ; the built-in theme that provides the icons and the palette
///   dark=true         ; whether the title bar and the links are those of a dark theme
///   inherit=false     ; true applies the base theme's stylesheet before style.qss
///   accent=#82a88b    ; the colour of selected icons, of the selection and of links
std::optional<CustomTheme> LoadCustomTheme(const QDir& themes_dir, const QString& id) {
    const QDir dir{themes_dir.filePath(id)};
    if (id.isEmpty() || !dir.exists(QStringLiteral("style.qss"))) {
        return std::nullopt;
    }

    CustomTheme theme;
    theme.id = id;
    theme.name = id;
    theme.dir = dir.absolutePath();
    theme.base = QString::fromUtf8(themes[static_cast<size_t>(default_theme)].second);

    const QSettings ini(dir.filePath(QStringLiteral("theme.ini")), QSettings::IniFormat);
    const QString name = ini.value(QStringLiteral("Theme/name")).toString().trimmed();
    if (!name.isEmpty()) {
        theme.name = name;
    }
    const QString base = ini.value(QStringLiteral("Theme/base")).toString().trimmed();
    if (IsBuiltinTheme(base)) {
        theme.base = base;
    }
    theme.dark = ini.value(QStringLiteral("Theme/dark"), IsDarkBuiltinTheme(theme.base.toStdString()))
                     .toBool();
    theme.inherit = ini.value(QStringLiteral("Theme/inherit"), false).toBool();
    theme.accent = ini.value(QStringLiteral("Theme/accent")).toString().trimmed();
    return theme;
}

} // Anonymous namespace

QString ThemesDir() {
    const auto path = FS::GetEdenPath(FS::EdenPath::EdenDir) / "themes";
    void(FS::CreateDir(path));
    return QString::fromStdString(FS::PathToUTF8String(path));
}

std::vector<CustomTheme> ScanCustomThemes() {
    std::vector<CustomTheme> found;
    const QDir themes_dir{ThemesDir()};
    for (const QString& id : themes_dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (auto theme = LoadCustomTheme(themes_dir, id)) {
            found.push_back(std::move(*theme));
        }
    }
    std::sort(found.begin(), found.end(), [](const CustomTheme& a, const CustomTheme& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return found;
}

void RefreshActiveCustomTheme() {
    active_custom_theme.reset();
    const QString theme = QString::fromStdString(values.theme);
    const QString prefix = QString::fromUtf8(CustomThemePrefix);
    if (theme.startsWith(prefix)) {
        active_custom_theme = LoadCustomTheme(QDir{ThemesDir()}, theme.mid(prefix.size()));
    }
}

const std::optional<CustomTheme>& ActiveCustomTheme() {
    return active_custom_theme;
}

std::string EffectiveTheme() {
    if (active_custom_theme) {
        return active_custom_theme->base.toStdString();
    }
    if (values.theme.rfind(CustomThemePrefix, 0) == 0) {
        // A custom theme whose folder is gone
        return themes[static_cast<size_t>(default_theme)].second;
    }
    return values.theme;
}

bool IsDarkTheme() {
    if (active_custom_theme) {
        return active_custom_theme->dark;
    }
    return IsDarkBuiltinTheme(EffectiveTheme());
}

Values values = {};

Setting<u32, true>& CurrentGameIconSize() {
    return values.game_list_mode.GetValue() == Settings::GameListMode::TreeView
               ? values.game_icon_size
               : values.game_cover_size;
}

u32 CalculateWidth(u32 height, Settings::AspectRatio ratio) {
    switch (ratio) {
    case Settings::AspectRatio::R4_3:
        return height * 4 / 3;
    case Settings::AspectRatio::R21_9:
        return height * 21 / 9;
    case Settings::AspectRatio::R16_10:
        return height * 16 / 10;
    case Settings::AspectRatio::R16_9:
    case Settings::AspectRatio::Stretch:
        // TODO: Move this function wherever appropriate to implement Stretched aspect
        break;
    }
    return height * 16 / 9;
}

void SaveWindowState() {
    const auto window_state_config_loc =
        FS::PathToUTF8String(FS::GetEdenPath(FS::EdenPath::ConfigDir) / "window_state.ini");

    void(FS::CreateParentDir(window_state_config_loc));
    QSettings config(QString::fromStdString(window_state_config_loc), QSettings::IniFormat);

    config.setValue(QStringLiteral("geometry"), values.geometry);
    config.setValue(QStringLiteral("state"), values.state);
    config.setValue(QStringLiteral("geometryRenderWindow"), values.renderwindow_geometry);
    config.setValue(QStringLiteral("gameListHeaderState"), values.gamelist_header_state);

    config.sync();
}

void RestoreWindowState(std::unique_ptr<QtConfig>& qtConfig) {
    const auto window_state_config_loc =
        FS::PathToUTF8String(FS::GetEdenPath(FS::EdenPath::ConfigDir) / "window_state.ini");

    // Migrate window state from old location
    if (!FS::Exists(window_state_config_loc) && qtConfig->Exists("UI", "UILayout\\geometry")) {
        const auto config_loc =
            FS::PathToUTF8String(FS::GetEdenPath(FS::EdenPath::ConfigDir) / "qt-config.ini");
        QSettings config(QString::fromStdString(config_loc), QSettings::IniFormat);

        config.beginGroup(QStringLiteral("UI"));
        config.beginGroup(QStringLiteral("UILayout"));
        values.geometry = config.value(QStringLiteral("geometry")).toByteArray();
        values.state = config.value(QStringLiteral("state")).toByteArray();
        values.renderwindow_geometry =
            config.value(QStringLiteral("geometryRenderWindow")).toByteArray();
        values.gamelist_header_state =
            config.value(QStringLiteral("gameListHeaderState")).toByteArray();
        config.endGroup();
        config.endGroup();
        return;
    }

    void(FS::CreateParentDir(window_state_config_loc));
    const QSettings config(QString::fromStdString(window_state_config_loc), QSettings::IniFormat);

    values.geometry = config.value(QStringLiteral("geometry")).toByteArray();
    values.state = config.value(QStringLiteral("state")).toByteArray();
    values.renderwindow_geometry =
        config.value(QStringLiteral("geometryRenderWindow")).toByteArray();
    values.gamelist_header_state =
        config.value(QStringLiteral("gameListHeaderState")).toByteArray();
}

} // namespace UISettings
