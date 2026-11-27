// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QPalette>
#include <QPixmap>
#include <QString>
#include <QStyledItemDelegate>

#include "lichii/game/styles/style_helpers.h"

/// Art for a game without an icon: a gradient picked from the title, with its initials.
QPixmap GameListFallbackIcon(int size, const QString& title, const QPalette& palette);

/**
 * A stylized "card"-like delegate for the game grid view.
 * Adapted from QML
 */
class GameCard : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit GameCard(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

    QRect getCardRect(const QStyleOptionViewItem &option, const QModelIndex &index) const;

    bool hitTest(const QPoint& point, const QModelIndex& index,
                 const QWidget* widget, const QRect& cellRect) const;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    void setSize(const QSize& newSize, const QSize& contentSize, const int padding, const int columns);

    static constexpr int cardMargin = 8;
    /// Grid cells are this much taller than the card plus its margins: the selection ring and
    /// the drop shadow reach 2 px under the card, and anything drawn outside the cell is left
    /// behind or cut when only the cell is repainted (selection and hover changes).
    static constexpr int ringRoom = 2;

    /// The text under the art, sized to what it shows. Every card of a view shares it, so the
    /// grid stays even: the title always gets two lines when names are on.
    struct TextLayout {
        bool names = false;    ///< title lines
        bool meta = false;     ///< info line (play time, version) or size and type
        int pad = 12;          ///< side padding, smaller on small cards
        int title_px = 14;
        int meta_px = 11;
        int meta_lines = 1;    ///< info lines: the parts packed at this card width
        /// Fixed line of each part, the same on every card of the view so a part never moves
        /// between cards (shared with the library styles).
        LibraryStyles::InfoLayout info;
        int height = 0;        ///< 0: no text block at all
    };
    /// `art` is the artwork side; names/info follow View > Show Game Name / Show Info.
    static TextLayout Layout(int art, bool names, bool info);
    /// Cell height margin around the art, see Lichii::CardWidthMargin.
    static int HeightMargin(int art, bool names, bool info);

private:

    QSize m_size;
    QSize m_contentSize;
    int m_padding;
    int m_columns;
};
