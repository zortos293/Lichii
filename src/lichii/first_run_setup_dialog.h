// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QWizard>

#include "qt_common/config/uisettings.h"

class QListWidget;
class QLineEdit;
class QRadioButton;
class QWizardPage;

/// The first-run setup. It picks a theme and a library style independently and shows a live
/// preview of the combination before the choices are applied.
class FirstRunSetupDialog : public QWizard {
    Q_OBJECT

public:
    explicit FirstRunSetupDialog(QWidget* parent = nullptr);
    ~FirstRunSetupDialog() override;

    [[nodiscard]] UISettings::Theme selected_theme() const;
    [[nodiscard]] UISettings::GameLibraryStyle selected_library_style() const;

private:
    void BuildThemeList();
    void UpdatePreview();
    bool validateCurrentPage() override;

    QListWidget* theme_list{};
    QLineEdit* firmware_path{};
    QLineEdit* keys_path{};
    QWizardPage* keys_page{};
    QRadioButton* modern_style{};
    QRadioButton* classic_style{};
    QWidget* preview{};
    bool firmware_source_is_zip{};
    bool keys_selection_changed{};
    bool firmware_imported{};
    bool installed_firmware_source_is_zip{};
    QString installed_keys_path;
    QString installed_firmware_path;
    QString installed_firmware_keys_path;
    UISettings::GameLibraryStyle selected_style{UISettings::GameLibraryStyle::Modern};
};
