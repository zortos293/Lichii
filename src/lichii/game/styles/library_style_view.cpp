// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

#include <QCoreApplication>

#include "qt_common/config/uisettings.h"
#include "lichii/game/styles/library_style_registry.h"
#include "lichii/game/styles/library_style_view.h"

namespace LibraryStyles {

namespace {

std::vector<StyleInfo>& Registry() {
    static std::vector<StyleInfo> styles;
    return styles;
}

/// Menu order of the known styles; any other id is listed after them, sorted by id. Static
/// registration runs in link order, which is not stable, so the list is ordered here instead.
int Rank(std::string_view id) {
    static constexpr std::array<std::string_view, 8> order{
        "canvas", "carousel",   "channel_grid", "dual_pane",
        "home_row", "cartridges", "box_shelf",  "dense_grid",
    };
    const auto it = std::find(order.begin(), order.end(), id);
    return it == order.end() ? static_cast<int>(order.size())
                             : static_cast<int>(std::distance(order.begin(), it));
}

} // namespace

bool Register(const StyleInfo& info) {
    if (info.id == nullptr || info.factory == nullptr || Find(info.id) != nullptr) {
        return false;
    }
    auto& styles = Registry();
    styles.push_back(info);
    std::stable_sort(styles.begin(), styles.end(), [](const StyleInfo& a, const StyleInfo& b) {
        const int rank_a = Rank(a.id);
        const int rank_b = Rank(b.id);
        if (rank_a != rank_b) {
            return rank_a < rank_b;
        }
        return std::strcmp(a.id, b.id) < 0;
    });
    return true;
}

const std::vector<StyleInfo>& All() {
    return Registry();
}

const StyleInfo* Find(const std::string& id) {
    if (id.empty()) {
        return nullptr;
    }
    for (const auto& info : Registry()) {
        if (id == info.id) {
            return &info;
        }
    }
    return nullptr;
}

QString DisplayName(const StyleInfo& info) {
    // Registered with QT_TRANSLATE_NOOP("LibraryStyles", ...); an untranslated name falls back to
    // the source text.
    const QString translated = QCoreApplication::translate("LibraryStyles", info.display_name);
    return translated.isEmpty() ? QString::fromUtf8(info.display_name) : translated;
}

const StyleInfo* Active() {
    if (UISettings::values.game_list_mode.GetValue() != Settings::GameListMode::GridView) {
        return nullptr;
    }
    return Find(UISettings::values.library_layout.GetValue());
}

} // namespace LibraryStyles
