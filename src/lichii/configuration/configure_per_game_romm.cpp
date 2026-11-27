// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "common/fs/path_util.h"
#include "common/romm_api.h"
#include "common/romm_config.h"
#include "lichii/configuration/configure_per_game_romm.h"
#include "lichii/romm_title_match_dialog.h"

ConfigurePerGameRomm::ConfigurePerGameRomm(QWidget* parent, u64 title_id_,
                                           const std::string& file_name_)
    : QWidget(parent), title_id(title_id_), file_name(file_name_) {
    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel(
        tr("Controls whether this title's save data syncs with your RomM cloud save (Tools > "
           "RomM...). Most titles never need this — it only matters if you picked the wrong "
           "catalogue entry, or chose \"Never Sync\" before this title existed on your instance "
           "and want to turn syncing back on."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    status_label = new QLabel(this);
    status_label->setWordWrap(true);
    layout->addWidget(status_label);

    choose_button = new QPushButton(tr("Choose RomM Title..."), this);
    layout->addWidget(choose_button);

    clear_button = new QPushButton(tr("Clear (Ask Again Next Sync)"), this);
    layout->addWidget(clear_button);

    layout->addStretch();

    connect(choose_button, &QPushButton::clicked, this, &ConfigurePerGameRomm::OnChooseTitle);
    connect(clear_button, &QPushButton::clicked, this, &ConfigurePerGameRomm::OnClearOverride);

    Refresh();
}

void ConfigurePerGameRomm::Refresh() {
    const auto override_id = Common::RommConfig::TitleOverride::GetRomId(title_id);
    if (!override_id.has_value()) {
        status_label->setText(tr("Not matched yet — you'll be asked to pick a title the next "
                                 "time cloud save sync runs for it."));
        clear_button->setEnabled(false);
    } else if (*override_id == 0) {
        status_label->setText(tr("Set to never sync."));
        clear_button->setEnabled(true);
    } else {
        // A blocking network call, same as the match-picker dialog itself; this is a properties
        // dialog someone opened on purpose, not a hot path, so a brief wait here is fine.
        const auto rom = Common::RommApi::GetRom(*override_id);
        status_label->setText(rom.has_value()
                                  ? tr("Matched to: %1").arg(QString::fromStdString(rom->name))
                                  : tr("Matched to rom_id %1, which no longer exists on this "
                                       "instance.")
                                        .arg(*override_id));
        clear_button->setEnabled(true);
    }
}

void ConfigurePerGameRomm::OnChooseTitle() {
    const auto hint = std::filesystem::path(Common::FS::ToU8String(file_name)).filename().string();
    const auto pick = RommTitleMatchDialog::PickRomForTitle(hint, this);
    switch (pick.action) {
    case RommTitleMatchDialog::Action::Selected:
        Common::RommConfig::TitleOverride::SetRomId(title_id, pick.rom_id);
        break;
    case RommTitleMatchDialog::Action::SkipPermanently:
        Common::RommConfig::TitleOverride::SetRomId(title_id, 0);
        break;
    case RommTitleMatchDialog::Action::SkipOnce:
        return; // Cancelled — leave whatever was already set untouched.
    }
    Refresh();
}

void ConfigurePerGameRomm::OnClearOverride() {
    Common::RommConfig::TitleOverride::Clear(title_id);
    Refresh();
}
