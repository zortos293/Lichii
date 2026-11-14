// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <map>
#include <set>
#include <string_view>
#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>
#include <fmt/format.h>
#include "common/settings.h"
#include "qt_common/config/uisettings.h"
#include "lichii/configuration/settings_share.h"
#include "lichii/configuration/shared_widget.h"

namespace SettingsShare {

namespace {

using Settings::BasicSetting;
using Settings::Category;

constexpr auto Prefix = "LX";

struct Section {
    const char* id;
    const char* name;
    std::vector<Category> categories;
};

const std::vector<Section>& Sections() {
    static const std::vector<Section> sections = [] {
        std::vector<Section> list{
            {"gen", QT_TRANSLATE_NOOP("SettingsShare", "General"), {Category::UiGeneral}},
            {"sys",
             QT_TRANSLATE_NOOP("SettingsShare", "System"),
             {Category::Core, Category::System}},
            {"app", QT_TRANSLATE_NOOP("SettingsShare", "Applets"), {Category::LibraryApplet}},
            {"cpu",
             QT_TRANSLATE_NOOP("SettingsShare", "CPU"),
             {Category::Cpu, Category::CpuUnsafe}},
            {"gfx", QT_TRANSLATE_NOOP("SettingsShare", "Graphics"), {Category::Renderer}},
            {"gfxa",
             QT_TRANSLATE_NOOP("SettingsShare", "Advanced Graphics"),
             {Category::RendererAdvanced}},
            {"gfxx",
             QT_TRANSLATE_NOOP("SettingsShare", "Graphics Extensions"),
             {Category::RendererExtensions, Category::RendererHacks}},
            {"aud",
             QT_TRANSLATE_NOOP("SettingsShare", "Audio"),
             {Category::Audio, Category::SystemAudio, Category::UiAudio}},
        };
        Section all{"all", QT_TRANSLATE_NOOP("SettingsShare", "all settings"), {}};
        for (const auto& section : list) {
            all.categories.insert(all.categories.end(), section.categories.begin(),
                                  section.categories.end());
        }
        list.push_back(std::move(all));
        return list;
    }();
    return sections;
}

const Section* FindSection(const QString& id) {
    for (const auto& section : Sections()) {
        if (id == QLatin1String(section.id)) {
            return &section;
        }
    }
    return nullptr;
}

QString Tr(const char* text, int n = -1);

// "General settings", or "all settings" for the whole set.
QString SectionName(const Section& section) {
    const auto name = QCoreApplication::translate("SettingsShare", section.name);
    return std::string_view{section.id} == "all" ? name : Tr("%1 settings").arg(name);
}

QString Tr(const char* text, int n) {
    return QCoreApplication::translate("SettingsShare", text, nullptr, n);
}

// Values tied to this PC or account, never carried by a code.
const std::set<std::string> machine_keys{
    "vulkan_device", "output_device",  "input_device",  "device_name",
    "serial_unit",   "serial_battery", "touch_device",  "ir_sensor_device",
    "eden_token",    "eden_username",  "gamecard_path", "current_user",
};

bool IsShareable(const BasicSetting& setting) {
    return setting.Save() && !machine_keys.contains(setting.GetLabel());
}

/// Shareable settings of a section, in a stable order.
std::vector<BasicSetting*> SettingsOf(const Section& section) {
    std::vector<BasicSetting*> result;
    for (auto* linkage : {&Settings::values.linkage, &UISettings::values.linkage}) {
        for (const auto category : section.categories) {
            const auto it = linkage->by_category.find(category);
            if (it == linkage->by_category.end()) {
                continue;
            }
            for (auto* setting : it->second) {
                if (IsShareable(*setting)) {
                    result.push_back(setting);
                }
            }
        }
    }
    return result;
}

std::string Normalize(const BasicSetting& setting, const std::string& value) {
    if (setting.IsFloatingPoint()) {
        return fmt::format("{}", std::strtod(value.c_str(), nullptr));
    }
    return value;
}

std::string CurrentValue(const BasicSetting& setting, const ConfigurationShared::Builder& builder) {
    const auto* widget = builder.FindWidget(setting);
    auto value = widget != nullptr ? widget->Current(setting) : setting.ToString();
    if (value.empty() && setting.TypeId() != "string") {
        value = setting.ToString(); // Never share an empty value for a number or a choice.
    }
    return Normalize(setting, value);
}

/// The whole section as the fields show it; values at their defaults are left out (implied).
std::vector<Entry> Collect(const Section& section, const ConfigurationShared::Builder& builder) {
    std::vector<Entry> entries;
    for (const auto* setting : SettingsOf(section)) {
        const auto value = CurrentValue(*setting, builder);
        if (value != Normalize(*setting, setting->DefaultToString())) {
            entries.push_back({setting->GetLabel(), value});
        }
    }
    return entries;
}

struct Change {
    BasicSetting* setting;
    ConfigurationShared::Widget* widget;
    std::string old_value;
    std::string new_value;
    bool to_global; ///< Per-game: show the global value.
};

struct Plan {
    std::vector<Change> changes;
    QStringList skipped;
};

Plan MakePlan(const Decoded& code, const Section& section,
              const ConfigurationShared::Builder& builder, bool per_game) {
    Plan plan;
    std::vector<Change> per_game_wanted;
    std::map<const ConfigurationShared::Widget*, bool> widget_global;
    std::map<std::string, std::string> listed;
    for (const auto& entry : code.entries) {
        listed[entry.key] = entry.value;
    }

    const auto skip = [&plan](const std::string& key, const char* reason) {
        plan.skipped << QStringLiteral("%1 (%2)").arg(QString::fromStdString(key), Tr(reason));
    };

    for (auto* setting : SettingsOf(section)) {
        const auto it = listed.find(setting->GetLabel());
        const bool is_listed = it != listed.end();
        // Not listed means the sender has it at its default.
        const std::string value = is_listed ? it->second : setting->DefaultToString();
        if (is_listed) {
            listed.erase(it);
        }

        if (per_game && !setting->Switchable()) {
            if (is_listed) {
                skip(setting->GetLabel(), QT_TRANSLATE_NOOP("SettingsShare", "global only"));
            }
            continue;
        }
        auto* const widget = builder.FindWidget(*setting);
        if (widget == nullptr || !widget->isEnabled()) {
            if (is_listed) {
                skip(setting->GetLabel(),
                     widget == nullptr
                         ? QT_TRANSLATE_NOOP("SettingsShare", "not a field here, set it by hand")
                         : QT_TRANSLATE_NOOP("SettingsShare", "locked while a game runs"));
            }
            continue;
        }

        const auto current = CurrentValue(*setting, builder);
        if (per_game) {
            const bool to_global =
                Normalize(*setting, value) == Normalize(*setting, setting->ToStringGlobal());
            per_game_wanted.push_back({setting, widget, current, value, to_global});
            widget_global[widget] =
                widget_global.try_emplace(widget, true).first->second && to_global;
            continue;
        }
        if (current != Normalize(*setting, value)) {
            plan.changes.push_back({setting, widget, current, value, false});
        }
    }

    // A paired widget (field plus checkbox) shares one restore button, so it goes global
    // only when every setting it owns matches the global value; otherwise the other
    // setting is staged as a custom value equal to the global one.
    for (auto& change : per_game_wanted) {
        const bool to_global = widget_global[change.widget];
        const auto target = Normalize(*change.setting, change.new_value);
        if (to_global ? change.widget->ShowsGlobal()
                      : !change.widget->ShowsGlobal() && change.old_value == target) {
            continue;
        }
        change.to_global = to_global;
        if (to_global) {
            change.new_value.clear();
        }
        plan.changes.push_back(std::move(change));
    }

    for (const auto& [key, value] : listed) {
        skip(key, QT_TRANSLATE_NOOP("SettingsShare", "not in this section or unknown here"));
    }
    return plan;
}

void Stage(const Plan& plan) {
    // Paired checkboxes first: unchecking one resets the field it enables.
    for (const bool checkboxes : {true, false}) {
        for (const auto& change : plan.changes) {
            if ((change.widget->CheckboxSetting() == change.setting) != checkboxes) {
                continue;
            }
            if (change.to_global) {
                change.widget->StageGlobal();
            } else {
                change.widget->Stage(*change.setting, change.new_value);
            }
        }
    }
}

void CopyCode(QWidget* parent, const Section& section, const DialogHooks& hooks) {
    const auto entries = Collect(section, *hooks.builder);
    const auto code = Encode(QLatin1String(section.id), entries);
    QApplication::clipboard()->setText(code);

    QMessageBox box(QMessageBox::Information, Tr("Settings code copied"),
                    Tr("The %1 code is on the clipboard. Pasting it sets those settings to what "
                       "you have now.")
                        .arg(SectionName(section)),
                    QMessageBox::Ok, parent);
    box.setInformativeText(code.size() <= 200 ? code : code.left(200) + QStringLiteral("..."));
    box.exec();
}

void PasteCode(QDialog* dialog, const DialogHooks& hooks) {
    const auto clip = QApplication::clipboard()->text().trimmed();
    bool ok = false;
    const auto text = QInputDialog::getMultiLineText(
        dialog, Tr("Paste settings code"), Tr("Settings code:"),
        clip.startsWith(QString::fromLatin1(Prefix)) ? clip : QString{}, &ok);
    if (!ok || text.trimmed().isEmpty()) {
        return;
    }

    QString error;
    const auto code = Decode(text, error);
    const auto* section = code ? FindSection(code->section) : nullptr;
    if (code && section == nullptr) {
        error = Tr("This code is for a part of the settings this Lichii does not know. Update "
                   "Lichii to paste it.");
    }
    if (section == nullptr) {
        QMessageBox::warning(dialog, Tr("Paste settings code"), error);
        return;
    }

    const bool per_game = !Settings::IsConfiguringGlobal();
    const auto plan = MakePlan(*code, *section, *hooks.builder, per_game);

    QStringList details;
    for (const auto& change : plan.changes) {
        details << QStringLiteral("%1: %2 -> %3")
                       .arg(QString::fromStdString(change.setting->GetLabel()),
                            QString::fromStdString(change.old_value),
                            change.to_global ? Tr("global")
                                             : QString::fromStdString(change.new_value));
    }
    for (const auto& skipped : plan.skipped) {
        details << Tr("Skipped: %1").arg(skipped);
    }

    if (plan.changes.empty()) {
        QMessageBox box(
            QMessageBox::Information, Tr("Paste settings code"),
            Tr("Nothing to change: %1 already match this code.").arg(SectionName(*section)),
            QMessageBox::Ok, dialog);
        box.setDetailedText(details.join(QLatin1Char('\n')));
        box.exec();
        return;
    }

    QMessageBox box(QMessageBox::Question, Tr("Paste settings code"),
                    Tr("Set %1 to match this code? %n setting(s) change.", int(plan.changes.size()))
                        .arg(SectionName(*section)),
                    QMessageBox::Yes | QMessageBox::Cancel, dialog);
    QString info = Tr("Your GPU, audio devices, paths and accounts are never changed. Nothing is "
                      "saved until you press OK or Apply; Cancel drops it.");
    if (!plan.skipped.isEmpty()) {
        info += QLatin1Char(' ') +
                Tr("%n setting(s) in the code will be skipped.", int(plan.skipped.size()));
    }
    box.setInformativeText(info + QLatin1Char(' ') + Tr("Show Details lists each one."));
    box.setDetailedText(details.join(QLatin1Char('\n')));
    if (box.exec() == QMessageBox::Yes) {
        Stage(plan);
    }
}

} // namespace

QString Encode(const QString& section, const std::vector<Entry>& entries) {
    QByteArray pairs;
    for (const auto& entry : entries) {
        if (!pairs.isEmpty()) {
            pairs += ';';
        }
        pairs += QByteArray::fromStdString(entry.key) + '=' +
                 QUrl::toPercentEncoding(QString::fromStdString(entry.value));
    }
    const auto packed = qCompress(pairs, 9).toBase64(QByteArray::Base64UrlEncoding |
                                                     QByteArray::OmitTrailingEquals);
    const bool zipped = packed.size() < pairs.size();
    return QStringLiteral("%1%2%3.%4.%5")
        .arg(QString::fromLatin1(Prefix))
        .arg(FormatVersion)
        .arg(zipped ? QStringLiteral("z") : QString{})
        .arg(section, QString::fromLatin1(zipped ? packed : pairs));
}

std::optional<Decoded> Decode(const QString& code, QString& error) {
    QString compact = code;
    compact.remove(QRegularExpression(QStringLiteral("\\s")));

    static const QRegularExpression header(QStringLiteral("^LX(\\d+)(z?)\\.([a-z]+)\\."));
    const auto match = header.match(compact);
    if (!match.hasMatch()) {
        error = Tr("This is not a Lichii settings code.");
        return std::nullopt;
    }
    if (match.captured(1).toInt() > FormatVersion) {
        error = Tr("This code was made by a newer Lichii. Update Lichii to paste it.");
        return std::nullopt;
    }

    auto pairs = compact.mid(match.capturedEnd()).toLatin1();
    if (!match.captured(2).isEmpty()) {
        const auto decoded = QByteArray::fromBase64Encoding(
            pairs, QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
        pairs = decoded ? qUncompress(*decoded) : QByteArray{};
        if (pairs.isEmpty()) {
            error = Tr("This code is incomplete or damaged. Copy it again.");
            return std::nullopt;
        }
    }

    Decoded result{match.captured(3), {}};
    for (const auto& pair : pairs.split(';')) {
        const auto eq = pair.indexOf('=');
        if (eq <= 0) {
            continue;
        }
        result.entries.push_back({pair.left(eq).toStdString(),
                                  QUrl::fromPercentEncoding(pair.mid(eq + 1)).toStdString()});
    }
    return result;
}

void AddShareButton(QDialog* dialog, QDialogButtonBox* box, DialogHooks hooks) {
    auto* button = box->addButton(Tr("Share settings"), QDialogButtonBox::ResetRole);
    auto* menu = new QMenu(button);
    auto* copy_tab = menu->addAction(QString{});
    auto* copy_all = menu->addAction(Tr("Copy code for all settings"));
    menu->addSeparator();
    auto* paste = menu->addAction(Tr("Paste code..."));
    button->setMenu(menu);
    button->setToolTip(Tr("Copy these settings as a short code to share or keep, or paste one. "
                          "Pasting only fills in the fields."));

    QObject::connect(menu, &QMenu::aboutToShow, menu, [copy_tab, hooks] {
        const auto* section = FindSection(hooks.current_section());
        copy_tab->setEnabled(section != nullptr);
        copy_tab->setText(section == nullptr ? Tr("Copy code for this tab (none here)")
                                             : Tr("Copy code for %1").arg(SectionName(*section)));
    });
    QObject::connect(copy_tab, &QAction::triggered, dialog, [dialog, hooks] {
        if (const auto* section = FindSection(hooks.current_section())) {
            CopyCode(dialog, *section, hooks);
        }
    });
    QObject::connect(copy_all, &QAction::triggered, dialog, [dialog, hooks] {
        CopyCode(dialog, *FindSection(QStringLiteral("all")), hooks);
    });
    QObject::connect(paste, &QAction::triggered, dialog,
                     [dialog, hooks] { PasteCode(dialog, hooks); });
}

} // namespace SettingsShare
