// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include <QStandardItem>
#include <QStringList>

#include "qt_common/game_list/game_list_p.h"

namespace Lichii {

/// Side of the square cover art on the cards: View > Game Icon Size (at least 32 px), the same
/// rule as the library styles. Names never change it; they only add the text below the art.
inline u32 CardIconDisplaySize([[maybe_unused]] bool show_game_name = true) {
    const u32 size = UISettings::values.game_cover_size.GetValue();
    return size == 0 ? 0 : std::max<u32>(size, u32{32});
}

/// View > Game Info > Scroll Long Names: one title line, the selected card's name scrolls.
inline bool ScrollLongNames() {
    return UISettings::values.game_name_overflow.GetValue() == Settings::GameNameOverflow::Scroll;
}

/// View > Game Icon Size > None: the cards show their text only.
inline bool CardArtHidden() {
    return UISettings::values.game_cover_size.GetValue() == 0;
}

/// Width the cards are built around: the art side, or 96 px of text when the art is hidden.
inline u32 CardWidthBase() {
    return CardArtHidden() ? u32{96} : CardIconDisplaySize();
}

/// Card cell width around the icon size in the grid and the carousel. GameCard::paint draws the
/// artwork 10 pixels narrower than the card cell; GameCard::HeightMargin gives the height.
constexpr int CardWidthMargin = 24;

/// View > Game Info: any part (play time, version, size, file type) switched on. The same
/// settings pick the list view's columns.
inline bool ShowAnyGameInfo() {
    return UISettings::values.show_play_time.GetValue() ||
           UISettings::values.show_add_ons.GetValue() || UISettings::values.show_size.GetValue() ||
           UISettings::values.show_types.GetValue();
}

inline bool ContainsAllWords(const QString& haystack, const QString& userinput) {
    const QStringList userinput_split = userinput.split(QLatin1Char{' '}, Qt::SkipEmptyParts);
    return std::all_of(userinput_split.begin(), userinput_split.end(), [&haystack](const QString& s) {
        return haystack.contains(s);
    });
}

inline bool FilterMatches(const QString& filter, const QStandardItem* item) {
    if (filter.isEmpty())
        return true;

    // Compiled once: this runs for every game on every search keystroke, in several views.
    static const QRegularExpression non_letters(QStringLiteral("[^a-z\\s]"));
    auto const sluggify = [](const QString& s) {
        QString o(s.normalized(QString::NormalizationForm_D).toLower());
        return o.replace(non_letters, QString{});
    };

    const auto norm_filter = sluggify(filter);
    const auto program_id = item->data(GameListItemPath::ProgramIdRole).toULongLong();
    const QString file_path = sluggify(item->data(GameListItemPath::FullPathRole).toString());
    const QString file_title = sluggify(item->data(GameListItemPath::TitleRole).toString());
    const QString file_program_id = QStringLiteral("%1").arg(program_id, 16, 16, QLatin1Char{'0'});
    const QString file_name = sluggify(file_path.mid(file_path.lastIndexOf(QLatin1Char{'/'}) + 1) + QLatin1Char{' '} + file_title);
    return (ContainsAllWords(file_name, norm_filter)
        || ContainsAllWords(file_title, norm_filter))
        || (file_program_id.size() == 16 && file_program_id.contains(norm_filter));
}

} // namespace Lichii
