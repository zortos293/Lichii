// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QDir>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

/// The order the user gave the game list: one list of game keys for every folder, so a game keeps
/// its place among the games of each folder it is shown in.
namespace GameOrder {

/// A game's key: its program id, or its path when it has none. The path is kept with '/'
/// separators, as the scan can give it with both and the config file reads it back with '/'.
inline QString Key(quint64 program_id, const QString& path) {
    if (program_id == 0) {
        return QDir::cleanPath(QDir::fromNativeSeparators(path));
    }
    return QStringLiteral("%1").arg(program_id, 16, 16, QLatin1Char('0')).toUpper();
}

/// A key as saved in the config, which drops quotes and changes backslashes in a string: percent
/// encoded, so that a path with either reads back unchanged.
inline QString Encode(const QString& key) {
    return QString::fromLatin1(key.toUtf8().toPercentEncoding("/:"));
}

inline QString Decode(const QString& saved) {
    return QString::fromUtf8(QByteArray::fromPercentEncoding(saved.toLatin1()));
}

/// Writes `shown`, the keys of one folder's games in their new order, into `order`: they take the
/// places their keys held, those not in it yet going at the end. Other games keep their places.
inline void Place(QStringList& order, const QStringList& shown) {
    QStringList games;
    QSet<QString> seen;
    for (const QString& key : shown) {
        if (!seen.contains(key)) {
            seen.insert(key);
            games.append(key);
        }
    }
    QStringList result;
    result.reserve(order.size() + games.size());
    qsizetype next = 0;
    for (const QString& key : std::as_const(order)) {
        if (!seen.contains(key)) {
            result.append(key);
        } else if (next < games.size()) {
            result.append(games[next++]);
        }
    }
    while (next < games.size()) {
        result.append(games[next++]);
    }
    order = std::move(result);
}

/// Moves the game at `from` in `shown` next to the one at `target`: before it, or after it.
inline void Move(QStringList& shown, qsizetype from, qsizetype target, bool after) {
    if (from == target) {
        return;
    }
    qsizetype to = after ? target + 1 : target;
    if (target > from) {
        --to;
    }
    shown.move(from, to);
}

/// Each key's place in `order`.
inline QHash<QString, int> Ranks(const QStringList& order) {
    QHash<QString, int> ranks;
    ranks.reserve(order.size());
    for (int i = 0; i < order.size(); ++i) {
        ranks.insert(order[i], i);
    }
    return ranks;
}

} // namespace GameOrder
