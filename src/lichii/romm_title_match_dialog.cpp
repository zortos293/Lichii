// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <optional>
#include <vector>

#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QVariant>

#include "common/romm_api.h"
#include "romm_title_match_dialog.h"

namespace RommTitleMatchDialog {

namespace {

// A small self-contained QDialog, built in code (no .ui) since this is a one-shot picker with no
// state worth round-tripping through Qt Designer — same reasoning as RommSettingsDialog.
class Dialog : public QDialog {
public:
    explicit Dialog(const std::string& file_name_hint, QWidget* parent) : QDialog(parent) {
        setWindowTitle(tr("Match Save Data to a RomM Title"));
        setMinimumSize(480, 420);

        auto* layout = new QVBoxLayout(this);
        auto* intro = new QLabel(
            tr("Pick which RomM title \"%1\" corresponds to, so its cloud save can sync with "
               "your instance. Search below, or skip cloud save sync for it entirely.")
                .arg(QString::fromStdString(file_name_hint)),
            this);
        intro->setWordWrap(true);
        layout->addWidget(intro);

        search_edit = new QLineEdit(this);
        search_edit->setPlaceholderText(tr("Filter by name..."));
        layout->addWidget(search_edit);

        rom_list = new QListWidget(this);
        layout->addWidget(rom_list, 1);

        auto* skip_permanently_button =
            new QPushButton(tr("Never Sync This Title"), this);
        layout->addWidget(skip_permanently_button);

        auto* button_box =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        button_box->button(QDialogButtonBox::Ok)->setText(tr("Use Selected"));
        layout->addWidget(button_box);

        connect(search_edit, &QLineEdit::textChanged, this, &Dialog::ApplyFilter);
        connect(rom_list, &QListWidget::itemDoubleClicked, this, &Dialog::accept);
        connect(button_box, &QDialogButtonBox::accepted, this, &Dialog::accept);
        connect(button_box, &QDialogButtonBox::rejected, this, &Dialog::reject);
        connect(skip_permanently_button, &QPushButton::clicked, this, [this] {
            skip_permanently = true;
            reject();
        });

        // Blocking, on purpose: this dialog only appears from RommSync's download paths, which
        // already run synchronously on the UI thread — one more blocking network call here
        // doesn't change that trade-off.
        all_roms = Common::RommApi::ListSwitchRoms();
        ApplyFilter();
    }

    bool WasSkippedPermanently() const {
        return skip_permanently;
    }

    std::optional<u64> SelectedRomId() const {
        const auto* item = rom_list->currentItem();
        if (!item) {
            return std::nullopt;
        }
        return item->data(Qt::UserRole).toULongLong();
    }

private:
    void ApplyFilter() {
        rom_list->clear();
        const QString filter = search_edit->text();
        for (const auto& rom : all_roms) {
            const QString name = QString::fromStdString(rom.name);
            if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive)) {
                continue;
            }
            const QString label = rom.platform_slug == Common::RommApi::Switch2PlatformSlug
                                      ? tr("%1 (Switch 2)").arg(name)
                                      : name;
            auto* item = new QListWidgetItem(label, rom_list);
            item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(rom.id));
        }
    }

    QLineEdit* search_edit;
    QListWidget* rom_list;
    std::vector<Common::RommApi::Rom> all_roms;
    bool skip_permanently = false;
};

} // namespace

PickResult PickRomForTitle(const std::string& file_name_hint, QWidget* parent) {
    Dialog dialog(file_name_hint, parent);
    if (dialog.exec() == QDialog::Accepted) {
        if (const auto rom_id = dialog.SelectedRomId(); rom_id.has_value()) {
            return {Action::Selected, *rom_id};
        }
        return {Action::SkipOnce, 0}; // "Use Selected" with nothing actually selected.
    }
    if (dialog.WasSkippedPermanently()) {
        return {Action::SkipPermanently, 0};
    }
    return {Action::SkipOnce, 0};
}

} // namespace RommTitleMatchDialog
