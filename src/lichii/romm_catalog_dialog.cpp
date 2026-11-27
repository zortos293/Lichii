// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "qt_common/config/uisettings.h"
#include "romm_catalog_dialog.h"

namespace {

QString FormatSize(u64 bytes) {
    constexpr std::array units{"B", "KiB", "MiB", "GiB"};
    double size = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (size >= 1024.0 && unit + 1 < units.size()) {
        size /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(size, 0, 'f', 1).arg(units[unit]);
}

QString CategoryLabel(Common::RommApi::RomFileCategory category) {
    switch (category) {
    case Common::RommApi::RomFileCategory::Main:
        return QObject::tr("Base Game");
    case Common::RommApi::RomFileCategory::Update:
        return QObject::tr("Update");
    case Common::RommApi::RomFileCategory::Dlc:
        return QObject::tr("DLC");
    default:
        return QObject::tr("Unknown");
    }
}

// Builds a scrollable tab page with its own VBoxLayout (plus trailing stretch), returning the
// layout callers add file rows to. Factored out since both tabs are built identically.
QVBoxLayout* MakeFileTabPage(QTabWidget* tabs, const QString& title, QWidget* parent) {
    auto* panel = new QWidget(parent);
    auto* layout = new QVBoxLayout(panel);
    layout->addStretch();
    auto* scroll = new QScrollArea(parent);
    scroll->setWidget(panel);
    scroll->setWidgetResizable(true);
    tabs->addTab(scroll, title);
    return layout;
}

} // namespace

RommCatalogDialog::RommCatalogDialog(std::function<void()> on_content_changed_,
                                     QWidget* parent)
    : QDialog(parent), on_content_changed(std::move(on_content_changed_)) {
    setWindowTitle(tr("RomM Catalog"));
    resize(760, 480);

    auto* outer_layout = new QVBoxLayout(this);
    status_label = new QLabel(tr("Loading catalogue..."), this);
    outer_layout->addWidget(status_label);

    auto* split_layout = new QHBoxLayout();
    outer_layout->addLayout(split_layout, 1);

    rom_list = new QListWidget(this);
    rom_list->setMinimumWidth(260);
    split_layout->addWidget(rom_list, 1);

    auto* right_side = new QVBoxLayout();
    split_layout->addLayout(right_side, 2);

    file_tabs = new QTabWidget(this);
    main_files_layout = MakeFileTabPage(file_tabs, tr("ROM + Updates"), this);
    dlc_files_layout = MakeFileTabPage(file_tabs, tr("DLC"), this);
    right_side->addWidget(file_tabs, 1);

    download_all_dlc_button = new QPushButton(tr("Download All DLC"), this);
    download_all_dlc_button->setEnabled(false);
    connect(download_all_dlc_button, &QPushButton::clicked, this,
            &RommCatalogDialog::OnDownloadAllDlc);
    right_side->addWidget(download_all_dlc_button);

    connect(rom_list, &QListWidget::itemClicked, this, &RommCatalogDialog::OnRomSelected);

    RefreshRomList();
}

void RommCatalogDialog::RefreshRomList() {
    status_label->setText(tr("Loading catalogue..."));
    rom_list->clear();

    auto* watcher = new QFutureWatcher<std::vector<Common::RommApi::Rom>>(this);
    connect(watcher, &QFutureWatcher<std::vector<Common::RommApi::Rom>>::finished, this,
            [this, watcher]() {
                current_roms = watcher->result();
                watcher->deleteLater();

                if (current_roms.empty()) {
                    status_label->setText(
                        tr("No titles found — check the master account is configured and this "
                           "RomM instance has a Switch or Switch 2 platform with games in it."));
                    return;
                }
                status_label->setText(tr("%1 title(s) found. Select one to see its files.")
                                          .arg(current_roms.size()));
                for (const auto& rom : current_roms) {
                    QString label = QString::fromStdString(rom.name);
                    if (rom.platform_slug == Common::RommApi::Switch2PlatformSlug) {
                        label += tr(" (Switch 2)");
                    }
                    rom_list->addItem(label);
                }
            });
    watcher->setFuture(QtConcurrent::run([] { return Common::RommApi::ListSwitchRoms(); }));
}

void RommCatalogDialog::OnRomSelected(QListWidgetItem* item) {
    const int row = rom_list->row(item);
    if (row < 0 || static_cast<std::size_t>(row) >= current_roms.size()) {
        return;
    }
    const auto& rom = current_roms[row];
    const u64 rom_id = rom.id;

    ClearFileLists();
    download_all_dlc_button->setEnabled(false);
    current_detail_rom.reset();
    main_files_layout->insertWidget(0, new QLabel(tr("Loading files..."), this));

    // The list endpoint never populates files[] (see the comment on ParseRom's fallback in
    // romm_api.cpp) — only the single-rom detail endpoint does, so a selection means "go fetch the
    // real detail now", not "read what we already have".
    auto* watcher = new QFutureWatcher<std::optional<Common::RommApi::Rom>>(this);
    connect(watcher, &QFutureWatcher<std::optional<Common::RommApi::Rom>>::finished, this,
            [this, watcher, rom_id, row]() {
                const auto detailed = watcher->result();
                watcher->deleteLater();

                // The person may have clicked a different row while this was in flight; only
                // apply the result if the selection hasn't moved on.
                if (rom_list->currentRow() != row) {
                    return;
                }
                ClearFileLists();

                if (!detailed.has_value()) {
                    main_files_layout->insertWidget(
                        0, new QLabel(tr("Couldn't load this title's files."), this));
                    return;
                }
                if (row >= 0 && static_cast<std::size_t>(row) < current_roms.size()) {
                    current_roms[static_cast<std::size_t>(row)] = *detailed;
                }
                current_detail_rom = *detailed;
                PopulateFileLists(*detailed);
            });
    watcher->setFuture(QtConcurrent::run([rom_id] { return Common::RommApi::GetRom(rom_id); }));
}

void RommCatalogDialog::ClearFileLists() {
    // Clear everything but each layout's trailing stretch.
    for (auto* layout : {main_files_layout, dlc_files_layout}) {
        while (layout->count() > 1) {
            auto* child = layout->takeAt(0);
            delete child->widget();
            delete child;
        }
    }
}

void RommCatalogDialog::PopulateFileLists(const Common::RommApi::Rom& rom) {
    if (rom.files.empty()) {
        main_files_layout->insertWidget(
            0, new QLabel(tr("This title has no downloadable files listed."), this));
        return;
    }

    int main_insert_at = 0;
    int dlc_insert_at = 0;
    int dlc_count = 0;
    for (const auto& file : rom.files) {
        const bool is_dlc = file.category == Common::RommApi::RomFileCategory::Dlc;

        auto* row_widget = new QWidget(this);
        auto* row_layout = new QHBoxLayout(row_widget);
        row_layout->setContentsMargins(0, 0, 0, 0);

        auto* label = new QLabel(QStringLiteral("[%1] %2 (%3)")
                                     .arg(CategoryLabel(file.category))
                                     .arg(QString::fromStdString(file.filename))
                                     .arg(FormatSize(file.size_bytes)),
                                 row_widget);
        label->setWordWrap(true);
        row_layout->addWidget(label, 1);

        auto* download_button = new QPushButton(tr("Download"), row_widget);
        connect(download_button, &QPushButton::clicked, this,
                [this, rom, file] { OnDownloadFile(rom, file); });
        row_layout->addWidget(download_button);

        if (is_dlc) {
            dlc_files_layout->insertWidget(dlc_insert_at++, row_widget);
            ++dlc_count;
        } else {
            main_files_layout->insertWidget(main_insert_at++, row_widget);
        }
    }

    download_all_dlc_button->setEnabled(dlc_count > 1);
    file_tabs->setTabText(1, dlc_count > 0 ? tr("DLC (%1)").arg(dlc_count) : tr("DLC"));
}

std::optional<std::filesystem::path> RommCatalogDialog::ResolveExternalContentDestination(
    bool& registers_dir) {
    registers_dir = false;

    // Updates/DLC go into one of Lichii's own "external content" directories (General > External
    // Content) rather than anywhere NAND-install-related — Lichii already scans those for loose
    // DLC/update files without installing them, which is exactly what a freshly downloaded one is.
    const auto& external_dirs = Settings::values.external_content_dirs;
    if (external_dirs.size() == 1) {
        return std::filesystem::path{external_dirs.front()};
    }
    if (external_dirs.size() > 1) {
        QStringList options;
        for (const auto& dir : external_dirs) {
            options << QString::fromStdString(dir);
        }
        bool accepted = false;
        const QString chosen = QInputDialog::getItem(
            this, tr("RomM"), tr("Which external content directory should this go in?"), options,
            0, false, &accepted);
        if (!accepted) {
            return std::nullopt;
        }
        return std::filesystem::path{chosen.toStdString()};
    }

    const QString chosen = QFileDialog::getExistingDirectory(
        this, tr("Select a directory for DLC/updates (this will be added to General > External "
                "Content)"));
    if (chosen.isEmpty()) {
        return std::nullopt;
    }
    registers_dir = true;
    return std::filesystem::path{chosen.toStdString()};
}

std::optional<std::filesystem::path> RommCatalogDialog::ResolveGameDirDestination(
    bool& registers_dir) {
    registers_dir = false;

    // The built-in SDMC/UserNAND/SysNAND entries in the game directory list are virtual: their
    // "path" is just a marker, not a folder a downloaded file could be written to. Only real ROM
    // directories qualify.
    std::vector<std::string> rom_dirs;
    for (const auto& game_dir : UISettings::values.game_dirs) {
        if (game_dir.path == "SDMC" || game_dir.path == "UserNAND" ||
            game_dir.path == "SysNAND") {
            continue;
        }
        rom_dirs.push_back(game_dir.path);
    }

    if (rom_dirs.size() == 1) {
        return std::filesystem::path{rom_dirs.front()};
    }
    if (rom_dirs.size() > 1) {
        QStringList options;
        for (const auto& dir : rom_dirs) {
            options << QString::fromStdString(dir);
        }
        bool accepted = false;
        const QString chosen = QInputDialog::getItem(
            this, tr("RomM"), tr("Which game directory should this go in?"), options, 0, false,
            &accepted);
        if (!accepted) {
            return std::nullopt;
        }
        return std::filesystem::path{chosen.toStdString()};
    }

    const QString chosen = QFileDialog::getExistingDirectory(
        this, tr("Select a directory for the game (it will be added to your game directories)"));
    if (chosen.isEmpty()) {
        return std::nullopt;
    }
    registers_dir = true;
    return std::filesystem::path{chosen.toStdString()};
}

void RommCatalogDialog::OnDownloadFile(const Common::RommApi::Rom& rom,
                                       const Common::RommApi::RomFile& file) {
    const bool is_main = file.category == Common::RommApi::RomFileCategory::Main;

    std::filesystem::path dest_dir;
    bool registers_game_dir = false;
    bool registers_external_content_dir = false;

    if (is_main) {
        const auto resolved = ResolveGameDirDestination(registers_game_dir);
        if (!resolved.has_value()) {
            return; // Person cancelled the directory prompt.
        }
        dest_dir = *resolved;
    } else {
        const auto resolved = ResolveExternalContentDestination(registers_external_content_dir);
        if (!resolved.has_value()) {
            return; // Person cancelled the directory prompt.
        }
        dest_dir = *resolved;
    }

    if (!Common::FS::CreateDirs(dest_dir)) {
        QMessageBox::critical(this, tr("RomM"),
                              tr("Couldn't create the destination folder for the download."));
        return;
    }
    const auto dest_path = dest_dir / file.filename;

    auto* progress = new QProgressDialog(
        tr("Downloading %1...").arg(QString::fromStdString(file.filename)), tr("Cancel"), 0, 100,
        this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setAttribute(Qt::WA_DeleteOnClose, true);
    progress->show();

    // Shared with the worker thread via should_cancel below; a plain bool wouldn't be safe to
    // read/write across threads without this.
    auto cancel_requested = std::make_shared<std::atomic<bool>>(false);
    QPointer<QProgressDialog> progress_guard(progress);
    connect(progress, &QProgressDialog::canceled, this,
            [cancel_requested] { cancel_requested->store(true); });

    auto* watcher = new QFutureWatcher<bool>(this);
    // DownloadRomContent's progress callback runs on the worker thread; hop back to the UI thread
    // for every update rather than touching the QProgressDialog directly from there.
    const auto on_progress = [this, progress_guard](u64 received, u64 total) {
        QMetaObject::invokeMethod(
            this,
            [progress_guard, received, total] {
                // QProgressDialog re-shows itself the next time setValue() is called after a
                // cancel, unless the caller checks wasCanceled() first — without this guard the
                // dialog appears to "not cancel" even though the click itself worked fine.
                if (!progress_guard || progress_guard->wasCanceled()) {
                    return;
                }
                if (total > 0) {
                    progress_guard->setMaximum(100);
                    progress_guard->setValue(static_cast<int>(received * 100 / total));
                } else {
                    progress_guard->setMaximum(0); // Unknown total: show a busy indicator instead.
                }
            },
            Qt::QueuedConnection);
    };

    connect(watcher, &QFutureWatcher<bool>::finished, this,
            [this, watcher, progress_guard, dest_path, registers_game_dir,
             registers_external_content_dir, file, cancel_requested]() {
                const bool ok = watcher->result();
                watcher->deleteLater();
                if (progress_guard) {
                    progress_guard->close();
                }

                if (!ok) {
                    if (cancel_requested->load()) {
                        return; // Cancelled on purpose — nothing to report as a failure.
                    }
                    QMessageBox::warning(
                        this, tr("RomM"),
                        tr("Download of '%1' failed. Check the log for details.")
                            .arg(QString::fromStdString(file.filename)));
                    return;
                }

                if (registers_game_dir) {
                    UISettings::GameDir game_dir{.path = dest_path.parent_path().string()};
                    if (!UISettings::values.game_dirs.contains(game_dir)) {
                        UISettings::values.game_dirs.append(game_dir);
                        if (on_content_changed) {
                            on_content_changed();
                        }
                    }
                } else if (registers_external_content_dir) {
                    auto& dirs = Settings::values.external_content_dirs;
                    const auto dir_str = dest_path.parent_path().string();
                    if (std::ranges::find(dirs, dir_str) == dirs.end()) {
                        dirs.push_back(dir_str);
                        if (on_content_changed) {
                            on_content_changed();
                        }
                    }
                } else {
                    // Directory was already being scanned (an existing game dir or external
                    // content dir), but the newly downloaded file itself still needs a rescan to
                    // be picked up.
                    if (on_content_changed) {
                        on_content_changed();
                    }
                }

                QMessageBox::information(
                    this, tr("RomM"),
                    tr("Downloaded to %1 — it should now show up in your game list.")
                        .arg(QString::fromStdString(dest_path.string())));
            });
    watcher->setFuture(QtConcurrent::run([rom_id = rom.id, file_id = file.id,
                                         filename = file.filename,
                                         dest_path_str = dest_path.string(), on_progress,
                                         cancel_requested] {
        return Common::RommApi::DownloadRomContent(
            rom_id, file_id, filename, dest_path_str, on_progress,
            [cancel_requested] { return cancel_requested->load(); });
    }));
}

void RommCatalogDialog::OnDownloadAllDlc() {
    if (!current_detail_rom.has_value()) {
        return;
    }
    const Common::RommApi::Rom rom = *current_detail_rom;

    std::vector<Common::RommApi::RomFile> dlc_files;
    u64 total_bytes = 0;
    for (const auto& file : rom.files) {
        if (file.category == Common::RommApi::RomFileCategory::Dlc) {
            dlc_files.push_back(file);
            total_bytes += file.size_bytes;
        }
    }
    if (dlc_files.size() < 2) {
        return; // Button is disabled in this case anyway; defensive no-op.
    }

    bool registers_external_content_dir = false;
    const auto resolved_dir = ResolveExternalContentDestination(registers_external_content_dir);
    if (!resolved_dir.has_value()) {
        return;
    }
    const auto dest_dir = *resolved_dir;

    if (QMessageBox::question(
            this, tr("RomM"),
            tr("Download all %1 DLC files for %2 (%3 total) to %4?")
                .arg(dlc_files.size())
                .arg(QString::fromStdString(rom.name))
                .arg(FormatSize(total_bytes))
                .arg(QString::fromStdString(dest_dir.string())),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    if (!Common::FS::CreateDirs(dest_dir)) {
        QMessageBox::critical(this, tr("RomM"),
                              tr("Couldn't create the destination folder for the download."));
        return;
    }

    auto* progress = new QProgressDialog(tr("Downloading DLC 1/%1...").arg(dlc_files.size()),
                                         tr("Cancel"), 0, static_cast<int>(dlc_files.size()), this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setAttribute(Qt::WA_DeleteOnClose, true);
    progress->show();

    auto cancel_requested = std::make_shared<std::atomic<bool>>(false);
    QPointer<QProgressDialog> progress_guard(progress);
    connect(progress, &QProgressDialog::canceled, this,
            [cancel_requested] { cancel_requested->store(true); });

    struct BatchResult {
        int succeeded = 0;
        int failed = 0;
        bool cancelled = false;
    };

    auto* watcher = new QFutureWatcher<BatchResult>(this);
    connect(watcher, &QFutureWatcher<BatchResult>::finished, this,
            [this, watcher, progress_guard, dest_dir, registers_external_content_dir]() {
                const auto result = watcher->result();
                watcher->deleteLater();
                if (progress_guard) {
                    progress_guard->close();
                }

                if (registers_external_content_dir && result.succeeded > 0) {
                    auto& dirs = Settings::values.external_content_dirs;
                    const auto dir_str = dest_dir.string();
                    if (std::ranges::find(dirs, dir_str) == dirs.end()) {
                        dirs.push_back(dir_str);
                    }
                }
                if (result.succeeded > 0 && on_content_changed) {
                    on_content_changed();
                }

                if (result.cancelled) {
                    QMessageBox::information(
                        this, tr("RomM"),
                        tr("Cancelled — %1 file(s) downloaded before stopping.")
                            .arg(result.succeeded));
                } else if (result.failed > 0) {
                    QMessageBox::warning(this, tr("RomM"),
                                        tr("%1 of %2 DLC file(s) failed to download. Check the "
                                           "log for details.")
                                            .arg(result.failed)
                                            .arg(result.succeeded + result.failed));
                } else {
                    QMessageBox::information(
                        this, tr("RomM"), tr("Downloaded %1 DLC file(s) successfully.")
                                             .arg(result.succeeded));
                }
            });

    watcher->setFuture(QtConcurrent::run([rom_id = rom.id, dlc_files,
                                         dest_dir_str = dest_dir.string(), progress_guard,
                                         cancel_requested]() -> BatchResult {
        BatchResult result;
        for (std::size_t i = 0; i < dlc_files.size(); ++i) {
            if (cancel_requested->load()) {
                result.cancelled = true;
                break;
            }
            const auto& file = dlc_files[i];

            QMetaObject::invokeMethod(
                qApp,
                [progress_guard, i, count = dlc_files.size(), name = file.filename] {
                    if (!progress_guard || progress_guard->wasCanceled()) {
                        return;
                    }
                    progress_guard->setLabelText(
                        QStringLiteral("Downloading DLC %1/%2: %3...")
                            .arg(i + 1)
                            .arg(count)
                            .arg(QString::fromStdString(name)));
                    progress_guard->setValue(static_cast<int>(i));
                },
                Qt::QueuedConnection);

            const auto dest_path = (std::filesystem::path{dest_dir_str} / file.filename).string();
            const bool ok = Common::RommApi::DownloadRomContent(
                rom_id, file.id, file.filename, dest_path, {},
                [cancel_requested] { return cancel_requested->load(); });
            if (ok) {
                ++result.succeeded;
            } else if (cancel_requested->load()) {
                result.cancelled = true;
                break;
            } else {
                ++result.failed;
            }
        }
        QMetaObject::invokeMethod(
            qApp,
            [progress_guard, count = dlc_files.size()] {
                if (progress_guard) {
                    progress_guard->setValue(static_cast<int>(count));
                }
            },
            Qt::QueuedConnection);
        return result;
    }));
}
