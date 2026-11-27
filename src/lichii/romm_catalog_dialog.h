// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <optional>

#include <QDialog>

#include "common/romm_api.h"

class QLabel;
class QListWidget;
class QListWidgetItem;
class QProgressDialog;
class QPushButton;
class QTabWidget;
class QVBoxLayout;

// Tools > RomM... > Browse Catalog... — lets the person browse the Switch titles their RomM
// instance's master account can see, and download a title's base game / update / DLC files to
// disk. Base game downloads land in a folder this dialog offers to register as a game directory;
// update/DLC downloads land in one of Lichii's own "external content" directories (the ones
// configured under General > External Content, scanned for loose DLC/update files without
// installing them to NAND) — asking which one if more than one is configured, or offering to add
// one if none are. Either way, the file just needs to be somewhere Lichii is already looking, so
// this never needs to touch the NAND-install pipeline at all.
//
// Files for the selected title are split across two tabs — "ROM + Updates" and "DLC" — because
// the two have different usage patterns: a rom has at most a couple of updates, but some titles
// (Super Smash Bros. Ultimate: 99, Mario Kart 8 Deluxe: 8) have enough DLC that downloading them
// one click at a time is impractical; the DLC tab has its own "Download All" for that case.
class RommCatalogDialog : public QDialog {
    Q_OBJECT

public:
    // `on_content_changed` is called (on the UI thread) whenever a download changes what the game
    // list should show — a new game directory or external content directory got registered, or a
    // file landed in one that was already being scanned — so the caller can refresh. Kept as a
    // callback rather than a MainWindow* dependency so this dialog doesn't need to know about
    // MainWindow at all.
    explicit RommCatalogDialog(std::function<void()> on_content_changed,
                               QWidget* parent = nullptr);

private:
    void RefreshRomList();
    void OnRomSelected(QListWidgetItem* item);
    void ClearFileLists();
    void PopulateFileLists(const Common::RommApi::Rom& rom);
    void OnDownloadFile(const Common::RommApi::Rom& rom, const Common::RommApi::RomFile& file);
    void OnDownloadAllDlc();

    // Shared by OnDownloadFile (for a DLC/update) and OnDownloadAllDlc: resolves which external
    // content directory a non-main file should land in, prompting the person only when there's a
    // real choice to make (more than one configured, or none at all). Returns nullopt if the
    // person cancelled the prompt. `registers_dir` is set to true if a brand new directory was
    // picked and still needs to be added to Settings::values.external_content_dirs on success.
    std::optional<std::filesystem::path> ResolveExternalContentDestination(bool& registers_dir);
    // Same idea for the base game: one of the real ROM directories in the game list settings.
    std::optional<std::filesystem::path> ResolveGameDirDestination(bool& registers_dir);

    std::function<void()> on_content_changed;
    std::vector<Common::RommApi::Rom> current_roms;
    // The fully detailed rom (with files[] populated) for whichever row is currently selected —
    // separate from current_roms, which only ever holds the lightweight list-endpoint data except
    // for whichever single entry OnRomSelected last overwrote. Used by OnDownloadAllDlc so it
    // doesn't need to re-fetch or guess which rom is "current".
    std::optional<Common::RommApi::Rom> current_detail_rom;

    QListWidget* rom_list;
    QTabWidget* file_tabs;
    QVBoxLayout* main_files_layout;
    QVBoxLayout* dlc_files_layout;
    QPushButton* download_all_dlc_button;
    QLabel* status_label;
};
