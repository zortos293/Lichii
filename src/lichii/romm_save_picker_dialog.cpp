// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QAbstractItemView>
#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "romm_save_picker_dialog.h"

namespace RommSavePickerDialog {

namespace {

class Dialog : public QDialog {
public:
    Dialog(const std::vector<Common::RommApi::RemoteSave>& saves_, const QString& header,
           QWidget* parent)
        : QDialog(parent), saves(saves_) {
        setWindowTitle(tr("Choose a Save to Download"));
        setMinimumSize(640, 360);

        auto* layout = new QVBoxLayout(this);
        auto* intro = new QLabel(header, this);
        intro->setWordWrap(true);
        layout->addWidget(intro);

        table = new QTableWidget(this);
        table->setColumnCount(5);
        table->setHorizontalHeaderLabels(
            {tr("File"), tr("Source"), tr("Slot"), tr("Updated"), tr("Size")});
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->verticalHeader()->setVisible(false);
        table->horizontalHeader()->setStretchLastSection(true);
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        layout->addWidget(table, 1);

        external_checkbox = new QCheckBox(
            tr("Show saves uploaded outside Lichii (other emulators, tools or manual uploads)"),
            this);
        layout->addWidget(external_checkbox);

        hint_label = new QLabel(this);
        hint_label->setWordWrap(true);
        layout->addWidget(hint_label);

        buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
        download_button = buttons->addButton(tr("Download"), QDialogButtonBox::AcceptRole);
        layout->addWidget(buttons);

        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(table, &QTableWidget::itemSelectionChanged, this,
                [this] { download_button->setEnabled(table->currentRow() >= 0); });
        connect(table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
            table->selectRow(row);
            accept();
        });
        connect(external_checkbox, &QCheckBox::toggled, this, [this] { Populate(); });

        Populate();
    }

    std::optional<u64> SelectedSaveId() const {
        const int row = table->currentRow();
        if (row < 0) {
            return std::nullopt;
        }
        return table->item(row, 0)->data(Qt::UserRole).toULongLong();
    }

private:
    void Populate() {
        const bool show_external = external_checkbox->isChecked();
        table->setRowCount(0);
        int hidden = 0;
        for (const auto& save : saves) {
            if (!save.UploadedByLichii() && !show_external) {
                ++hidden;
                continue;
            }
            const int row = table->rowCount();
            table->insertRow(row);

            auto* file_item = new QTableWidgetItem(QString::fromStdString(save.file_name));
            file_item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(save.save_id));
            table->setItem(row, 0, file_item);

            QString source = save.emulator.empty() ? tr("Unknown")
                                                   : QString::fromStdString(save.emulator);
            if (save.UploadedByLichii()) {
                source = tr("Lichii");
            }
            table->setItem(row, 1, new QTableWidgetItem(source));
            table->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(save.slot)));

            const auto when = QDateTime::fromString(QString::fromStdString(save.updated_at),
                                                    Qt::ISODate);
            table->setItem(row, 3,
                           new QTableWidgetItem(when.isValid()
                                                    ? QLocale().toString(when.toLocalTime(),
                                                                         QLocale::ShortFormat)
                                                    : QString::fromStdString(save.updated_at)));
            table->setItem(row, 4,
                           new QTableWidgetItem(
                               QLocale().formattedDataSize(static_cast<qint64>(save.size_bytes))));
        }

        if (table->rowCount() > 0) {
            table->selectRow(0);
        }
        download_button->setEnabled(table->currentRow() >= 0);

        if (hidden > 0) {
            hint_label->setText(
                tr("%n save(s) from other sources are hidden.", nullptr, hidden));
        } else if (table->rowCount() == 0) {
            hint_label->setText(tr("RomM has no saves for this title."));
        } else {
            hint_label->clear();
        }
    }

    std::vector<Common::RommApi::RemoteSave> saves;
    QTableWidget* table = nullptr;
    QCheckBox* external_checkbox = nullptr;
    QLabel* hint_label = nullptr;
    QDialogButtonBox* buttons = nullptr;
    QPushButton* download_button = nullptr;
};

} // Anonymous namespace

std::optional<u64> PickSave(const std::vector<Common::RommApi::RemoteSave>& saves,
                            const QString& header, QWidget* parent) {
    Dialog dialog(saves, header, parent);
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return dialog.SelectedSaveId();
}

} // namespace RommSavePickerDialog
