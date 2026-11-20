// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/httplib.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <system_error>

#include <QApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include <fmt/format.h>

#include "common/common_types.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/net/ca_store.h"
#include "lichii/configuration/flyhigh_panel.h"
#include "video_core/frame_gen/flyhigh_model.h"

namespace {

namespace FrameGen = VideoCore::FrameGen;
namespace Model = VideoCore::FrameGen::FlyHighModel;

QString Tr(const char* text) {
    return QCoreApplication::translate("FlyHighPanel", text);
}

QString PathText(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

QString Text(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

u64 DownloadSize() {
    u64 total = 0;
    for (const Model::File& file : Model::FILES) {
        total += file.size;
    }
    return total;
}

/// GETs one file of the model at the pinned commit. Empty with `error` set on failure.
std::optional<std::string> Fetch(std::string_view name, std::string& error,
                                 const std::function<bool(u64)>& progress = {}) {
    httplib::Client client{std::string{Model::DOWNLOAD_HOST}};
    client.set_connection_timeout(30);
    client.set_read_timeout(30);
    Common::Net::ConfigureCaStore(client);
    std::string body;
    int status = 0;
    const auto result = client.Get(
        fmt::format("{}{}", Model::DOWNLOAD_PATH, name),
        [&status](const httplib::Response& response) {
            status = response.status;
            return response.status == 200;
        },
        [&](const char* data, size_t length) {
            body.append(data, length);
            return !progress || progress(body.size());
        });
    if (!result || status != 200) {
        error = result ? fmt::format("HTTP {}", status) : httplib::to_string(result.error());
        return std::nullopt;
    }
    return body;
}

} // Anonymous namespace

FlyHighPanel::FlyHighPanel(QWidget* parent) : QWidget(parent) {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 0, 0, 8);
    layout->setSpacing(6);

    QHBoxLayout* status_row = new QHBoxLayout;
    status_row->setSpacing(8);
    icon_label = new QLabel(this);
    icon_label->setTextFormat(Qt::RichText);
    icon_label->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    icon_label->setFixedWidth(16);
    status_label = new QLabel(this);
    status_label->setTextFormat(Qt::RichText);
    status_label->setWordWrap(true);
    status_row->addWidget(icon_label);
    status_row->addWidget(status_label, 1);

    QHBoxLayout* button_row = new QHBoxLayout;
    button_row->setContentsMargins(24, 0, 0, 0);
    download_button = new QPushButton(this);
    folder_button = new QPushButton(Tr("Open Folder"), this);
    QLabel* about = new QLabel(QStringLiteral("<a href=\"%1\">%2</a>")
                                   .arg(Text(Model::RELEASE_PAGE), Tr("About FlyHigh")),
                               this);
    about->setOpenExternalLinks(true);
    about->setToolTip(Tr("The FlyHigh engine on GitHub. The library is Apache-2.0; the model "
                         "is CC BY-NC 4.0."));
    button_row->addWidget(download_button);
    button_row->addWidget(folder_button);
    button_row->addStretch(1);
    button_row->addWidget(about);

    layout->addLayout(status_row);
    layout->addLayout(button_row);

    connect(download_button, &QPushButton::clicked, this, [this] { Download(); });
    connect(folder_button, &QPushButton::clicked, this, [] {
        const std::filesystem::path folder = FrameGen::FlyHighModelDir();
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        QDesktopServices::openUrl(QUrl::fromLocalFile(PathText(folder)));
    });
    Refresh();
}

void FlyHighPanel::showEvent(QShowEvent* event) {
    Refresh();
    QWidget::showEvent(event);
}

void FlyHighPanel::Refresh() {
    const bool library = FrameGen::FlyHighLibraryPresent();
    const bool model = FrameGen::FlyHighModelInstalled();
    const bool ready = library && model;
    icon_label->setText(ready ? QStringLiteral("<span style=\"color:#2ea043\">&#10004;</span>")
                              : QStringLiteral("<span style=\"color:#d29922\">&#9888;</span>"));
    if (!library) {
        status_label->setText(
            Tr("<b>Library missing:</b> flyhigh.dll (libflyhigh.so on Linux) must be beside "
               "Lichii. Lichii's release archives include it."));
    } else if (!model) {
        status_label->setText(Tr("<b>Model needed:</b> FlyHigh's network weights, about %1 MB, "
                                 "are downloaded once into the user folder.")
                                  .arg((DownloadSize() + (1 << 19)) >> 20));
    } else {
        status_label->setText(Tr("<b>Ready.</b> FlyHigh runs on its own Vulkan device on the "
                                 "same GPU; the first start at a new resolution times its "
                                 "kernels for a few seconds."));
    }
    status_label->setToolTip(Tr("Folder: %1").arg(PathText(FrameGen::FlyHighModelDir())));
    download_button->setText(Tr("Download Model"));
    download_button->setVisible(library && !model);
}

void FlyHighPanel::Download() {
    DownloadFlyHighModel(this);
    Refresh();
}

void OfferFlyHighModel(QWidget* parent) {
    if (FrameGen::FlyHighModelInstalled() || !FrameGen::FlyHighLibraryPresent()) {
        return;
    }
    DownloadFlyHighModel(parent);
}

bool DownloadFlyHighModel(QWidget* parent) {
    const std::filesystem::path folder = FrameGen::FlyHighModelDir();
    const auto fail = [parent](const std::string& error) {
        LOG_ERROR(Frontend, "FlyHigh model download failed: {}", error);
        QMessageBox::warning(parent, Tr("FlyHigh"),
                             Tr("Could not download the FlyHigh model: %1. Optical Flow "
                                "Interpolation runs until it is there.")
                                 .arg(QString::fromStdString(error)));
        return false;
    };

    std::string error;
    std::optional<std::string> licence;
    {
        QProgressDialog busy(Tr("Contacting GitHub..."), QString{}, 0, 0, parent);
        busy.setWindowTitle(Tr("FlyHigh"));
        busy.setWindowModality(Qt::WindowModal);
        busy.setMinimumDuration(0);
        busy.show();
        QApplication::processEvents();
        licence = Fetch(Model::LICENSE_FILE, error);
    }
    if (!licence || licence->empty()) {
        return fail(error.empty() ? "could not retrieve the model's licence" : error);
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(Tr("Download the FlyHigh Model"));
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QLabel* intro = new QLabel(
        Tr("FlyHigh needs its network, about 2 MB, once. Lichii will download its weights (graph.json, flyhighfg.json and "
           "flyhighfg.bin) from the FlyHigh engine's repository on GitHub into %1.<br>They are "
           "licensed under the terms below.")
            .arg(PathText(folder)),
        &dialog);
    intro->setWordWrap(true);
    QPlainTextEdit* licence_text = new QPlainTextEdit(QString::fromStdString(*licence), &dialog);
    licence_text->setReadOnly(true);
    licence_text->setMinimumSize(640, 380);
    QDialogButtonBox* buttons = new QDialogButtonBox(&dialog);
    buttons->addButton(Tr("Accept and Download"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(intro);
    layout->addWidget(licence_text, 1);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    QProgressDialog progress(Tr("Downloading the FlyHigh model..."), Tr("Cancel"), 0, 1000,
                             parent);
    progress.setWindowTitle(Tr("FlyHigh"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setValue(0);
    QApplication::processEvents();
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const u64 total = DownloadSize();
    u64 done = 0;
    for (const Model::File& file : Model::FILES) {
        const std::optional<std::string> contents = Fetch(file.name, error, [&](u64 received) {
            progress.setValue(static_cast<int>((done + received) * 1000 / total));
            QApplication::processEvents();
            return !progress.wasCanceled();
        });
        if (progress.wasCanceled()) {
            return false;
        }
        if (!contents) {
            progress.close();
            return fail(error);
        }
        done += file.size;
        const QByteArray sha256 =
            QCryptographicHash::hash(QByteArrayView{contents->data(),
                                                    static_cast<qsizetype>(contents->size())},
                                     QCryptographicHash::Sha256)
                .toHex();
        if (contents->size() != file.size ||
            sha256 != QByteArray{file.sha256.data(), static_cast<qsizetype>(file.sha256.size())}) {
            progress.close();
            return fail(fmt::format("{} doesn't match the pinned model", file.name));
        }
        const std::filesystem::path target = folder / file.name;
        std::filesystem::path partial = target;
        partial += L".download";
        {
            std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
            stream.write(contents->data(), static_cast<std::streamsize>(contents->size()));
            if (!stream) {
                progress.close();
                return fail("could not write " + Common::FS::PathToUTF8String(partial));
            }
        }
        std::filesystem::rename(partial, target, ec);
        if (ec) {
            std::filesystem::remove(partial, ec);
            progress.close();
            return fail("could not save " + Common::FS::PathToUTF8String(target));
        }
    }
    {
        std::ofstream stream(folder / Model::LICENSE_FILE, std::ios::binary | std::ios::trunc);
        stream.write(licence->data(), static_cast<std::streamsize>(licence->size()));
    }
    LOG_INFO(Frontend, "Downloaded the FlyHigh model into {}", Common::FS::PathToUTF8String(folder));
    progress.setValue(1000);
    progress.close();
    return true;
}
