// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <iostream>
#include <source_location>

#include "qt_common/game_list/game_order.h"

namespace {

void Check(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) {
        std::cerr << "Game order check failed at line " << location.line() << '\n';
        std::exit(1);
    }
}

QStringList List(const char* keys) {
    return QString::fromLatin1(keys).split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

QStringList Moved(const char* shown, qsizetype from, qsizetype target, bool after) {
    QStringList list = List(shown);
    GameOrder::Move(list, from, target, after);
    return list;
}

} // namespace

int main() {
    Check(GameOrder::Key(0x0100F17E57000000ULL, QStringLiteral("C:/games/fx.nro")) ==
          QStringLiteral("0100F17E57000000"));
    Check(GameOrder::Key(0, QStringLiteral("C:/games/spin.nro")) ==
          QStringLiteral("C:/games/spin.nro"));
#ifdef _WIN32
    Check(GameOrder::Key(0, QStringLiteral("C:/games\\spin.nro")) ==
          QStringLiteral("C:/games/spin.nro"));
#endif

    // The config drops quotes and changes backslashes in a string: saved keys have neither, and
    // read back unchanged.
    for (const QString& key : {QStringLiteral("/games/a\"b.nro"), QStringLiteral("/games/a\\b.nro"),
                               QStringLiteral("C:/Games/100% done.nro"),
                               QStringLiteral("0100F17E57000000")}) {
        const QString saved = GameOrder::Encode(key);
        Check(!saved.contains(QLatin1Char('"')) && !saved.contains(QLatin1Char('\\')));
        Check(GameOrder::Decode(saved) == key);
    }
    Check(GameOrder::Encode(QStringLiteral("0100F17E57000000")) ==
          QStringLiteral("0100F17E57000000"));

    // Before or after a game further down, and before or after one further up.
    Check(Moved("A B C D", 0, 2, false) == List("B A C D"));
    Check(Moved("A B C D", 0, 2, true) == List("B C A D"));
    Check(Moved("A B C D", 3, 1, false) == List("A D B C"));
    Check(Moved("A B C D", 3, 1, true) == List("A B D C"));
    Check(Moved("A B C D", 0, 3, true) == List("B C D A"));
    Check(Moved("A B C D", 3, 0, false) == List("D A B C"));
    Check(Moved("A B C D", 1, 1, true) == List("A B C D"));

    // Nothing saved yet: the folder's games are saved in their new order.
    QStringList order;
    GameOrder::Place(order, List("B A C"));
    Check(order == List("B A C"));

    // Games of other folders keep their places, and the folder's games fill the places theirs
    // held; one not saved yet goes at the end.
    order = List("A X B Y");
    GameOrder::Place(order, List("B N A"));
    Check(order == List("B X N Y A"));

    // A game shown twice in a folder is saved once.
    order = List("A B");
    GameOrder::Place(order, List("B A B"));
    Check(order == List("B A"));

    const auto ranks = GameOrder::Ranks(List("B X N"));
    Check(ranks.value(QStringLiteral("B")) == 0);
    Check(ranks.value(QStringLiteral("N")) == 2);
    Check(!ranks.contains(QStringLiteral("A")));
    return 0;
}
