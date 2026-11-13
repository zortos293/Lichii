// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <vector>

#include <QColor>
#include <QFont>
#include <QModelIndex>
#include <QPalette>
#include <QPixmap>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>

#include "common/common_types.h"

class QPainter;
class QWidget;

namespace LibraryStyles {

class LibraryStyleView;

/// One controller hint: round button glyphs ("A", or "L" "R") and what they do.
struct ButtonHint {
    QStringList glyphs;
    QString label;
};

/// What a style needs to know about one row of the library model. Every field is filled for game
/// rows; the "Add games folder" row only has `is_add_folder` and `title`.
struct GameInfo {
    bool valid = false;
    bool is_game = false;
    bool is_add_folder = false;
    /// The title from the game metadata, or the file name when the game has none.
    QString title;
    QString file_name;
    QString path;
    u64 program_id = 0;
    /// "XCI", "NSP", "NUP", "NX" and so on.
    QString file_type;
    /// NUP or NX package: a title of the newer console generation. Use it for a badge or a
    /// different frame colour, never with a console or company name in the UI.
    bool is_nx2 = false;
    bool has_icon = false;
    u64 play_time_seconds = 0;
    /// "New" (never played), "45 min" or "12.5 h".
    QString play_time_text;
    /// Version of the applied update without a prefix ("1.4.3"), empty for the base version.
    QString update_version;
    /// "1.0 h played · v1.4.3": the play time and the update version, the line the concepts show.
    QString info_line;
    /// Readable size ("14.2 GiB").
    QString size_text;
    /// Program id as 16 upper-case hex digits.
    QString program_id_text;
};

/// S3 Game Info laid out for a view: each part switched on under View > Game Info keeps a fixed
/// line, packed once for the width from worst-case samples, so play time, version, size and type
/// sit in the same place on every item of the view.
struct InfoLayout {
    /// Line of each part (play time, version, size, file type); -1 when off or left out.
    std::array<int, 4> line_of{-1, -1, -1, -1};
    /// Number of lines, 0 for none.
    int lines = 0;
    /// "4.4 h" instead of "4.4 h played" on every item of the view.
    bool short_play = false;
    int width = 0;
};

/**
 * Shared data accessors and painting helpers for the library styles. All functions take indexes
 * of the model the view received in SetModel (or of the source model, both work: the data is read
 * through the index). Colours follow the palette of the widget, which follows the theme, so day
 * and night come for free.
 */
class LibraryStyleContext {
public:
    static GameInfo Info(const QModelIndex& index);

    static QString Title(const QModelIndex& index);
    static QString InfoLine(const QModelIndex& index);

    /// Cover art at `size` (cropped to fill, aspect kept). Games without art get a neutral
    /// gradient with the title initials; the "Add games folder" row gets a folder with a plus.
    static QPixmap Cover(const QModelIndex& index, const QSize& size, const QPalette& palette);
    /// The art of a game without an icon: a gradient picked from the title, with its initials.
    /// No console art, so it fits every style and the Modern grid.
    static void PaintPlaceholderArt(QPainter* painter, const QRect& rect, const QString& title,
                                    const QPalette& palette);

    static bool IsDark(const QPalette& palette);
    /// Selection colour of the theme.
    static QColor Accent(const QPalette& palette);
    /// Side of the square cover art in every grid style: View > Game Icon Size, at least 32 px.
    /// One setting gives the same art size in every style.
    static int ArtSize();
    /// Art of the one-row showcase styles (Home Row tiles, Showcase centre): 1.6x ArtSize.
    static int FeaturedArtSize();
    /// CSS-like box shadow for a rounded rect of `size`: blurred by `blur` px, `alpha` 0..1.
    /// Draw it at the rect's top left minus blur (plus your offset). Cached per size.
    static QPixmap SoftShadow(const QSize& size, qreal radius, int blur, qreal alpha, qreal dpr);
    /// Card surface colour behind covers and panels.
    static QColor Surface(const QPalette& palette);
    /// Secondary text colour (info lines, hints).
    static QColor MutedText(const QPalette& palette);

    /// Wooden tabletop shared by the Cartridges and Box Shelf styles, sized to the viewport.
    /// Draw it at the origin; it does not scroll.
    static QPixmap WoodTable(const QSize& size, qreal dpr, bool dark);
    /// Cover clipped to a rounded rectangle.
    static void PaintCover(QPainter* painter, const QRect& rect, const QModelIndex& index,
                           const QPalette& palette, int radius);
    /// Accent ring around `rect` (drawn just outside it), the selection mark of every style.
    static void PaintSelection(QPainter* painter, const QRect& rect, const QPalette& palette,
                               int radius, int width = 3);
    /// S4 lines of `text` for a box `width` px wide: up to `max_lines` lines, broken between
    /// words only. A line that is too wide ends in "...", unless it overflows by less than the
    /// "..." would take: then it stays whole, running up to `max_overflow` px past the box
    /// (default: the width of "...").
    static QStringList NameLines(const QString& text, const QFont& font, int width,
                                 int max_lines = 2, int max_overflow = -1);
    /// Title in up to `max_lines` lines inside `rect`, see NameLines.
    static void PaintName(QPainter* painter, const QRect& rect, const QString& text,
                          const QColor& color, const QFont& font, Qt::Alignment alignment,
                          int max_lines = 2, int max_overflow = -1);
    /// Lines a title may take: `max_lines`, or 1 with View > Game Info > Scroll Long Names.
    static int TitleLines(int max_lines = 2);
    /// A title, the S4 way (PaintName with TitleLines). With Scroll Long Names on, `glide_in`
    /// set (the selected item, painted straight onto that widget) and a name too long for its
    /// line, the name glides to its end and back instead of being cut; only that line of the
    /// widget repaints, and only while it moves.
    static void PaintTitle(QPainter* painter, const QRect& rect, const QString& text,
                           const QColor& color, const QFont& font, Qt::Alignment alignment,
                           int max_lines = 2, const QWidget* glide_in = nullptr,
                           int max_overflow = -1);

    /// Fixed Game Info lines for items whose info gets `width` px in `font`, at most
    /// `max_lines` lines. The short play time when the long one would need more lines or not
    /// fit; parts that still do not fit in `max_lines` are left out, from the end, for the whole
    /// view. `only`: lay out the parts of that one game instead of the worst case, for a spot
    /// that only ever shows one game and whose width changes with it.
    static InfoLayout LayoutInfo(const QFont& font, int width, int max_lines = 4,
                                 const GameInfo* only = nullptr);
    /// The Game Info lines of `game` in `layout`, "Base" and "NaN GB" for missing values. A line
    /// that is still too wide drops "played", then whole parts from its end (S3).
    static QStringList InfoLines(const GameInfo& game, const InfoLayout& layout,
                                 const QFont& font);
    /// `lines` one under the other in `rect`, aligned by `alignment`.
    static void PaintInfoLines(QPainter* painter, const QRect& rect, const QStringList& lines,
                               const QColor& color, const QFont& font, Qt::Alignment alignment);

    /// `family` first, then system fonts with CJK glyphs, so titles in Japanese, Chinese or
    /// Korean never show as boxes with a theme font that has only Latin glyphs.
    static void SetFamilyWithFallbacks(QFont& font, const QString& family);

    /// Rounded panel with the title on top and the info line under it.
    static void PaintInfoPanel(QPainter* painter, const QRect& rect, const QModelIndex& index,
                               const QPalette& palette, const QFont& font);
    /// Solid band at the bottom of a cover with the name and one info line on it (the Tinfoil
    /// concept), for styles that show names on top of the art. `glide_in`: see PaintTitle.
    static void PaintCoverCaption(QPainter* painter, const QRect& cover_rect,
                                  const QModelIndex& index, const QFont& font,
                                  const QFont& info_font, bool show_name, bool show_info,
                                  const QWidget* glide_in = nullptr);

    /// The library buttons: A Play, B Names, X `option` (when the style has one), Y Info,
    /// L R Style. Letters are Switch positions, like the controller mapping.
    static std::vector<ButtonHint> StandardHints(const QString& option = {});
    /// True for the shoulder buttons (L, R, LB, RB, ZL, ZR): drawn as rounded bars, not discs.
    static bool IsShoulderGlyph(const QString& glyph);
    /// Width of one button glyph: a disc of `radius`, or a wider bar for a shoulder button.
    static qreal GlyphWidth(const QString& glyph, qreal radius);
    /// One button glyph from `left`, centred on `center_y`: a disc with the letter, or a rounded
    /// bar for a shoulder button. Outlined with `pen_width`, or `filled` with the letter in
    /// `letter`. Returns the glyph's box.
    static QRectF PaintGlyph(QPainter* painter, const QString& glyph, qreal left, qreal center_y,
                             qreal radius, const QColor& ink, qreal pen_width, const QFont& font,
                             bool filled = false, const QColor& letter = {});
    /// Width of a hint row drawn with PaintHints.
    static qreal HintsWidth(const std::vector<ButtonHint>& hints, const QFont& font, qreal radius);
    /// A hint row from `left`, centred on `center_y`. `filled`: solid discs with the letter cut
    /// out in `letter` (Home Row look); otherwise outlined rings (Canvas look).
    static void PaintHints(QPainter* painter, const std::vector<ButtonHint>& hints, qreal left,
                           qreal center_y, const QFont& font, qreal radius, const QColor& ink,
                           bool filled = false, const QColor& letter = {});
    /// The Canvas hint pill, centred at the bottom of `area`: rounded translucent bar with ring
    /// glyphs. Styles keep HintPillReserve px free under their last row.
    static void PaintHintPill(QPainter* painter, const QRect& area,
                              const std::vector<ButtonHint>& hints, bool dark);
    static constexpr int HintPillReserve = 64;

    /// The Lichii icon tiled in staggered rows on a grid slanted -18 degrees, each icon upright.
    /// tile is the icon size in px; opacity is how strongly it shows over the background.
    static void PaintLogoTiles(QPainter* painter, const QRectF& area, qreal tile, qreal opacity);
};

/// Minimal view used by the placeholder files until a style lands: a plain list of the titles under
/// the style name.
LibraryStyleView* MakePlaceholderView(QWidget* parent, const QString& style_name);

} // namespace LibraryStyles
