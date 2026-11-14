// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Shown under the NVIDIA DLSS setting: which of NVIDIA's files were found, and
// controls to obtain the platform runtime from NVIDIA's official repository.

#pragma once

#ifdef EGLANTINE_NVIDIA_DLSS

#include <QWidget>

class QLabel;
class QPushButton;

class NvidiaDlssPanel : public QWidget {
public:
    explicit NvidiaDlssPanel(QWidget* parent);

    void Refresh();

#ifdef EGLANTINE_STREAMLINE
    /// Whether DLSS Frame Generation is the chosen frame generation type: the panel then lists
    /// its files and downloads them with the rest.
    void SetFrameGenWanted(bool wanted);
#endif

protected:
    void showEvent(QShowEvent* event) override;

private:
    void Download();
#ifdef EGLANTINE_NGX
    void Import();
    void ImportNeuralModel();
    QPushButton* import_button{};
    QPushButton* neural_import_button{};
    QLabel* neural_status_label{};
    bool driver_present{};
    bool neural_model_checked{};
    bool neural_model_valid{};
    bool neural_importing{};
#endif

    QLabel* icon_label;
    QLabel* status_label;
    QPushButton* download_button;
    QPushButton* folder_button;
    bool frame_gen_wanted{};
};

#endif
