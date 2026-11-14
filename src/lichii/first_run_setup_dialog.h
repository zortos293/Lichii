// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

#include "qt_common/config/uisettings.h"

class QListWidget;
class QRadioButton;

/// The first-run setup. It picks a theme and a library style independently and shows a live
/// preview of the combination before the choices are applied.
class FirstRunSetupDialog : public QDialog {
    Q_OBJECT

public:
    explicit FirstRunSetupDialog(QWidget* parent = nullptr);
    ~FirstRunSetupDialog() override;

    [[nodiscard]] UISettings::Theme selected_theme() const;
    [[nodiscard]] UISettings::GameLibraryStyle selected_library_style() const;

private:
    void BuildThemeList();
    void UpdatePreview();

    QListWidget* theme_list{};
    QRadioButton* modern_style{};
    QRadioButton* classic_style{};
    QWidget* preview{};
    UISettings::GameLibraryStyle selected_style{UISettings::GameLibraryStyle::Modern};
};
