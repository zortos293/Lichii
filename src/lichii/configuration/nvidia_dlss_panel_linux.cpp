// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef EGLANTINE_NGX

#include <memory>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QSaveFile>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include "common/dynamic_library.h"
#include "common/fs/fs_util.h"
#include "common/settings.h"
#include "video_core/neural_rendering/model.h"
#include "video_core/neural_rendering/runtime.h"
#include "video_core/vulkan_common/ngx_dlss.h"
#include "lichii/configuration/nvidia_dlss_panel.h"

namespace {
namespace Ngx = Vulkan::Ngx;
namespace NR = VideoCore::NeuralRendering;
constexpr qint64 MaxModelBytes = 70 * 1024 * 1024;

QString Tr(const char *text) { return QCoreApplication::translate("NvidiaDlssPanel", text); }
QString PathText(const std::filesystem::path &path) {
    return QString::fromStdString(Common::FS::PathToUTF8String(path));
}
QString ModelPath() { return PathText(Ngx::UserFolder() / Ngx::ModelFilename); }
QUrl AssetUrl(const QString &path) {
    return QUrl(QStringLiteral("https://raw.githubusercontent.com/NVIDIA/DLSS/%1/%2")
                    .arg(QString::fromLatin1(Ngx::SdkRevision), path));
}
QNetworkRequest Request(const QUrl &url) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(60000);
    return request;
}
bool ValidModel(const QByteArray &data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex() == Ngx::ModelSha256;
}
/// Atomically save imported or downloaded bytes after ensuring the user runtime directory
/// exists.
bool Save(const QString &path, const QByteArray &data) {
    if (!QDir().mkpath(PathText(Ngx::UserFolder())))
        return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}
} // namespace

/// Build Linux runtime controls, probe driver availability once, and connect status refreshes.
NvidiaDlssPanel::NvidiaDlssPanel(QWidget *parent) : QWidget(parent) {
    driver_present = Common::DynamicLibrary{"libnvidia-ngx.so.1"}.IsOpen();
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 0, 0, 0);
    auto *status_row = new QHBoxLayout;
    icon_label = new QLabel(this);
    status_label = new QLabel(this);
    status_label->setWordWrap(true);
    status_row->addWidget(icon_label);
    status_row->addWidget(status_label, 1);
    layout->addLayout(status_row);
    auto *buttons = new QHBoxLayout;
    download_button = new QPushButton(Tr("Download NVIDIA DLSS…"), this);
    import_button = new QPushButton(Tr("Import Linux DLSS…"), this);
    folder_button = new QPushButton(Tr("Open folder"), this);
    buttons->addWidget(download_button);
    buttons->addWidget(import_button);
    buttons->addWidget(folder_button);
    buttons->addStretch();
    layout->addLayout(buttons);
    neural_status_label = new QLabel(this);
    neural_status_label->setWordWrap(true);
    layout->addWidget(neural_status_label);
    auto* neural_buttons = new QHBoxLayout;
    neural_import_button = new QPushButton(Tr("Import neural rendering model…"), this);
    neural_buttons->addWidget(neural_import_button);
    neural_buttons->addStretch();
    layout->addLayout(neural_buttons);
    connect(neural_import_button, &QPushButton::clicked, this, &NvidiaDlssPanel::ImportNeuralModel);
    // Model validation reads 141 MiB. Cache it for this panel and keep it off the UI thread.
    auto* model_check = new QFutureWatcher<bool>(this);
    connect(model_check, &QFutureWatcher<bool>::finished, this, [this, model_check] {
        neural_model_valid = model_check->result();
        neural_model_checked = true;
        model_check->deleteLater();
        Refresh();
    });
    model_check->setFuture(
        QtConcurrent::run([path = NR::UserFolder()] { return NR::IsInstalled(path); }));
    connect(download_button, &QPushButton::clicked, this, &NvidiaDlssPanel::Download);
    connect(import_button, &QPushButton::clicked, this, &NvidiaDlssPanel::Import);
    connect(folder_button, &QPushButton::clicked, this, [] {
        QDir().mkpath(PathText(Ngx::UserFolder()));
        QDesktopServices::openUrl(QUrl::fromLocalFile(PathText(Ngx::UserFolder())));
    });
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] {
        if (isVisible())
            Refresh();
    });
    timer->start(1000);
    Refresh();
}

void NvidiaDlssPanel::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    Refresh();
}

/// Update runtime and model status plus action availability, reusing cached driver and model
/// checks.
void NvidiaDlssPanel::Refresh() {
    const auto model = Ngx::FindModel();
    const auto state = Ngx::GetStatus();
    QString text;
    if (!driver_present) {
        text =
            Tr("The NVIDIA driver's NGX component is missing. Install a supported NVIDIA driver.");
    } else if (model.empty()) {
        text = Tr("Download or import the Linux DLSS runtime to enable NVIDIA DLSS replacement.");
    } else if (!state.error.empty()) {
        text = Tr("Native NGX: %1. Restart the game after resolving this.")
                   .arg(QString::fromStdString(state.error));
    } else if (state.available && state.evaluations != 0) {
        text = Tr("Native NGX active — %1 successful evaluations.").arg(state.evaluations);
    } else if (state.available) {
        text = Tr("Native NGX ready; waiting for a supported game pass.");
    } else {
        text = Tr(
            "Linux DLSS runtime installed. Device compatibility is checked when the game starts.");
    }
    status_label->setText(text);
    status_label->setToolTip(PathText(model.empty() ? Ngx::UserFolder() : model));
    icon_label->setText(state.available ? QStringLiteral("✓") : QStringLiteral("ⓘ"));
    download_button->setVisible(model.empty());
    const auto neural = NR::GetStatus();
    QString neural_text;
    if (neural_importing) {
        neural_text = Tr("Importing the neural rendering model…");
    } else if (!neural_model_checked) {
        neural_text = Tr("Checking the neural rendering model…");
    } else if (!neural_model_valid) {
        neural_text = Tr(
            "Import a DLSS neural rendering 310.8.0 model to use Standard, Natural or Cinematic.");
    } else if (Settings::values.neural_rendering.GetValue() == Settings::NeuralRendering::Off) {
        neural_text = Tr("Neural rendering is off. Choose a style and apply to enable it.");
    } else if (!neural.error.empty()) {
        neural_text = Tr("Neural rendering: %1. Restart the game after resolving this.")
                          .arg(QString::fromStdString(neural.error));
    } else if (neural.ready) {
        neural_text = Tr("Native neural rendering active — %1 frames submitted.")
                          .arg(neural.submitted_frames);
    } else {
        neural_text =
            Tr("Neural rendering model installed. Apply a style to use it with NVIDIA DLSS.");
    }
    neural_status_label->setText(neural_text);
    neural_status_label->setToolTip(PathText(NR::UserFolder()));
    neural_import_button->setEnabled(neural_model_checked && !neural_importing);
}

/// Import a selected neural-model DLL on a worker and refresh the UI with success or failure.
void NvidiaDlssPanel::ImportNeuralModel() {
    const auto path =
        QFileDialog::getOpenFileName(this, Tr("Import NVIDIA neural rendering model"), {},
                                     Tr("Neural rendering model (nvngx_dlssnr.dll)"));
    if (path.isEmpty())
        return;
    neural_importing = true;
    Refresh();
    auto* progress =
        new QProgressDialog(Tr("Importing the neural rendering model…"), {}, 0, 0, this);
    progress->setCancelButton(nullptr);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->show();
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, progress] {
        progress->close();
        progress->deleteLater();
        const auto error = watcher->result();
        watcher->deleteLater();
        neural_importing = false;
        if (error.isEmpty()) {
            neural_model_valid = true;
            neural_model_checked = true;
        } else {
            QMessageBox::warning(this, Tr("Neural rendering model"), error);
        }
        Refresh();
    });
    watcher->setFuture(QtConcurrent::run([source = std::filesystem::path{path.toStdString()},
                                          destination = NR::UserFolder()]() -> QString {
        try {
            NR::ImportModel(source, destination);
            return {};
        } catch (const std::exception& error) {
            return QString::fromUtf8(error.what());
        }
    }));
}

/// Validate a selected Linux DLSS library and atomically save it into the user runtime
/// directory.
void NvidiaDlssPanel::Import() {
    const QString path = QFileDialog::getOpenFileName(this, Tr("Import Linux NVIDIA DLSS"), {},
                                                      Tr("Linux DLSS (libnvidia-ngx-dlss.so*)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaxModelBytes) {
        QMessageBox::warning(this, Tr("NVIDIA DLSS"), Tr("Could not read the DLSS runtime."));
        return;
    }
    const auto data = file.readAll();
    if (!ValidModel(data)) {
        QMessageBox::warning(this, Tr("NVIDIA DLSS"),
                             Tr("This file does not match the supported NVIDIA Linux DLSS 310.9.16 "
                                "runtime. Use Download to obtain the verified version."));
        return;
    }
    if (!Save(ModelPath(), data)) {
        QMessageBox::warning(this, Tr("NVIDIA DLSS"), Tr("Could not save the DLSS runtime."));
    }
    Refresh();
}

/// Fetch the runtime license for user acceptance, then download and validate the pinned Linux
/// DLSS asset.
void NvidiaDlssPanel::Download() {
    download_button->setEnabled(false);
    import_button->setEnabled(false);
    auto *network = new QNetworkAccessManager(this);
    auto *licence = network->get(Request(AssetUrl(QStringLiteral("LICENSE.txt"))));
    const auto done = [this, network] {
        download_button->setEnabled(true);
        import_button->setEnabled(true);
        network->deleteLater();
        Refresh();
    };
    connect(licence, &QNetworkReply::finished, this, [this, network, licence, done] {
        const auto licence_data = licence->readAll();
        if (licence->error() != QNetworkReply::NoError ||
            QCryptographicHash::hash(licence_data, QCryptographicHash::Sha256).toHex() !=
                "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0") {
            QMessageBox::warning(this, Tr("NVIDIA DLSS"),
                                 Tr("Could not retrieve NVIDIA's license. Please retry."));
            done();
            return;
        }
        QDialog dialog(this);
        dialog.setWindowTitle(Tr("Download Linux NVIDIA DLSS"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *intro =
            new QLabel(Tr("Download NVIDIA's Linux DLSS 310.9.16 runtime from its official "
                          "repository. Review NVIDIA's license before continuing."),
                       &dialog);
        intro->setWordWrap(true);
        layout->addWidget(intro);
        auto *text = new QPlainTextEdit(QString::fromUtf8(licence_data), &dialog);
        text->setReadOnly(true);
        text->setMinimumSize(640, 380);
        layout->addWidget(text);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
        buttons->addButton(Tr("Accept and Download"), QDialogButtonBox::AcceptRole);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted) {
            done();
            return;
        }
        struct Transfer {
            QSaveFile file{ModelPath()};
            QCryptographicHash hash{QCryptographicHash::Sha256};
            qint64 received{};
            bool failed{};
        };
        auto transfer = std::make_shared<Transfer>();
        if (!QDir().mkpath(PathText(Ngx::UserFolder())) ||
            !transfer->file.open(QIODevice::WriteOnly)) {
            QMessageBox::warning(this, Tr("NVIDIA DLSS"),
                                 Tr("Could not create the DLSS runtime file."));
            done();
            return;
        }
        auto *reply =
            network->get(Request(AssetUrl(QStringLiteral("lib/Linux_x86_64/rel/%1")
                                              .arg(QString::fromLatin1(Ngx::ModelFilename)))));
        auto *progress =
            new QProgressDialog(Tr("Downloading Linux NVIDIA DLSS…"), Tr("Cancel"), 0, 1000, this);
        progress->setWindowModality(Qt::WindowModal);
        progress->setMinimumDuration(0);
        progress->setValue(0);
        connect(progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
        connect(reply, &QNetworkReply::readyRead, this, [transfer, reply] {
            const auto data = reply->readAll();
            transfer->received += data.size();
            if (transfer->received > MaxModelBytes || transfer->file.write(data) != data.size()) {
                transfer->failed = true;
                reply->abort();
                return;
            }
            transfer->hash.addData(data);
        });
        connect(reply, &QNetworkReply::downloadProgress, progress,
                [progress](qint64 received, qint64 total) {
                    if (total > 0)
                        progress->setValue(static_cast<int>(received * 1000 / total));
                });
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, transfer, progress, licence_data, done] {
                    const bool cancelled = progress->wasCanceled();
                    progress->close();
                    progress->deleteLater();
                    const bool valid = !cancelled && !transfer->failed &&
                                       reply->error() == QNetworkReply::NoError &&
                                       transfer->hash.result().toHex() == Ngx::ModelSha256;
                    if (!valid ||
                        !Save(PathText(Ngx::UserFolder() / "LICENSE.txt"), licence_data) ||
                        !transfer->file.commit()) {
                        transfer->file.cancelWriting();
                        if (!cancelled)
                            QMessageBox::warning(this, Tr("NVIDIA DLSS"),
                                                 Tr("The download failed verification or could not "
                                                    "be saved. Please retry."));
                    }
                    done();
                });
    });
}

#endif
