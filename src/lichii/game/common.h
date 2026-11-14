// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include <QStandardItem>
#include <QStringList>

#include "qt_common/game_list/game_list_p.h"

namespace Lichii {

/// Display size used by the icon-based library views: without a title label the user icon size is
/// used as-is, otherwise keep a minimum size that stays legible next to the title.
inline u32 CardIconDisplaySize(bool show_game_name) {
    const u32 icon_size = UISettings::values.game_cover_size.GetValue();
    return show_game_name ? std::max<u32>(icon_size, u32{128}) : icon_size;
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

    auto const sluggify = [](const QString& s) {
        QString o(s.normalized(QString::NormalizationForm_D).toLower());
        return o.replace(QRegularExpression(QStringLiteral("[^a-z\\s]")), QStringLiteral(""));
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
