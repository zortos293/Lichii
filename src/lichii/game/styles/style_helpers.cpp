// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <vector>

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QFontMetrics>
#include <QTransform>
#include <QHash>
#include <QImage>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLinearGradient>
#include <QListView>
#include <QPainter>
#include <QGuiApplication>
#include <QPainterPath>
#include <QRadialGradient>
#include <QRegularExpression>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>
#include <QVBoxLayout>

#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/game_list_p.h"
#include "qt_common/game_list/model.h"
#include "lichii/game/common.h"
#include "lichii/game/styles/library_style_view.h"
#include "lichii/game/styles/style_helpers.h"

namespace LibraryStyles {

namespace {

QString Tr(const char* text) {
    return QCoreApplication::translate("LibraryStyles", text);
}

QModelIndex Column(const QModelIndex& index, int column) {
    return index.sibling(index.row(), column);
}

QString PlayTimeText(u64 seconds) {
    if (seconds == 0) {
        return Tr("New");
    }
    if (seconds < 3600) {
        return Tr("%1 min").arg(std::max<u64>(1, seconds / 60));
    }
    return Tr("%1 h").arg(static_cast<double>(seconds) / 3600.0, 0, 'f', 1);
}

QString UpdateVersion(const QString& add_ons) {
    static const QRegularExpression regex{QStringLiteral(R"(^Update \(([0-9\.]+)\))"),
                                          QRegularExpression::MultilineOption};
    const auto match = regex.match(add_ons);
    return match.hasMatch() ? match.captured(1) : QString{};
}

void PaintAddFolder(QPainter* painter, const QRect& rect, const QPalette& palette) {
    const bool dark = LibraryStyleContext::IsDark(palette);
    painter->fillRect(rect, dark ? QColor(29, 30, 32) : QColor(243, 243, 244));
    const QColor accent = LibraryStyleContext::Accent(palette);
    const qreal side = std::min(rect.width(), rect.height());
    const QRectF folder(rect.center().x() - side * 0.24, rect.center().y() - side * 0.18,
                        side * 0.48, side * 0.36);
    QColor fill = accent;
    fill.setAlpha(dark ? 28 : 20);
    painter->setPen(QPen(accent, std::max(2.0, side * 0.014), Qt::SolidLine, Qt::RoundCap,
                         Qt::RoundJoin));
    painter->setBrush(fill);
    painter->drawRoundedRect(folder, side * 0.03, side * 0.03);
    const qreal plus = side * 0.08;
    const QPointF center = folder.center();
    painter->drawLine(QPointF(center.x() - plus, center.y()), QPointF(center.x() + plus, center.y()));
    painter->drawLine(QPointF(center.x(), center.y() - plus), QPointF(center.x(), center.y() + plus));
}

} // namespace

GameInfo LibraryStyleContext::Info(const QModelIndex& index) {
    GameInfo info;
    if (!index.isValid()) {
        return info;
    }
    const QModelIndex name = Column(index, GameListModel::COLUMN_NAME);
    const auto type = name.data(GameListItem::TypeRole).value<GameListItemType>();
    info.valid = true;
    if (type == GameListItemType::AddDir) {
        info.is_add_folder = true;
        info.title = Tr("Add games folder");
        return info;
    }
    if (type != GameListItemType::Game) {
        info.title = name.data(Qt::DisplayRole).toString();
        return info;
    }

    info.is_game = true;
    info.path = name.data(GameListItemPath::FullPathRole).toString();
    info.file_name = QFileInfo(info.path).completeBaseName();
    info.title = name.data(GameListItemPath::TitleRole).toString();
    if (info.title.isEmpty()) {
        info.title = info.file_name;
    }
    info.program_id = name.data(GameListItemPath::ProgramIdRole).toULongLong();
    info.program_id_text =
        QStringLiteral("%1").arg(info.program_id, 16, 16, QLatin1Char{'0'}).toUpper();
    info.file_type = name.data(GameListItemPath::FileTypeRole).toString();
    info.is_nx2 = info.file_type == QStringLiteral("NUP") || info.file_type == QStringLiteral("NX");
    info.has_icon = name.data(GameListItemPath::HasIconRole).toBool();
    info.play_time_seconds = name.data(GameListItemPath::PlayTimeSecondsRole).toULongLong();
    info.play_time_text = PlayTimeText(info.play_time_seconds);
    info.update_version = name.data(GameListItemPath::UpdateVersionRole).toString();
    if (info.update_version.isEmpty()) {
        info.update_version = UpdateVersion(
            Column(index, GameListModel::COLUMN_ADD_ONS).data(Qt::DisplayRole).toString());
    }
    info.size_text = Column(index, GameListModel::COLUMN_SIZE).data(Qt::DisplayRole).toString();
    // Unpacked folder games have no size yet ("Not scanned" in the list): keep it short here.
    static const QRegularExpression has_digit{QStringLiteral("[0-9]")};
    if (!info.size_text.isEmpty() && !info.size_text.contains(has_digit)) {
        info.size_text = QStringLiteral("NaN GB");
    }

    // Only the parts switched on under View > Game Info (the list view's columns).
    QStringList parts;
    if (UISettings::values.show_play_time.GetValue()) {
        parts.append(info.play_time_seconds == 0 ? info.play_time_text
                                                 : Tr("%1 played").arg(info.play_time_text));
    }
    if (UISettings::values.show_add_ons.GetValue() && !info.update_version.isEmpty()) {
        parts.append(QStringLiteral("v%1").arg(info.update_version));
    }
    if (UISettings::values.show_size.GetValue() && !info.size_text.isEmpty()) {
        parts.append(info.size_text);
    }
    if (UISettings::values.show_types.GetValue() && !info.file_type.isEmpty()) {
        parts.append(info.file_type);
    }
    info.info_line = parts.join(QStringLiteral(" · "));
    return info;
}

QString LibraryStyleContext::Title(const QModelIndex& index) {
    return Info(index).title;
}

QString LibraryStyleContext::InfoLine(const QModelIndex& index) {
    return Info(index).info_line;
}

QPixmap LibraryStyleContext::Cover(const QModelIndex& index, const QSize& size,
                                   const QPalette& palette) {
    if (!index.isValid() || size.isEmpty()) {
        return {};
    }
    const QModelIndex name = Column(index, GameListModel::COLUMN_NAME);
    const auto type = name.data(GameListItem::TypeRole).value<GameListItemType>();

    QPixmap result(size);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (type == GameListItemType::AddDir) {
        PaintAddFolder(&painter, result.rect(), palette);
        return result;
    }

    QPixmap art;
    if (name.data(GameListItemPath::HasIconRole).toBool()) {
        art = name.data(GameListItemPath::CardIconRole).value<QPixmap>();
        if (art.isNull()) {
            art = name.data(Qt::DecorationRole).value<QPixmap>();
        }
    }
    if (art.isNull()) {
        QString title = name.data(GameListItemPath::TitleRole).toString();
        if (title.isEmpty()) {
            title = QFileInfo(name.data(GameListItemPath::FullPathRole).toString())
                        .completeBaseName();
        }
        PaintPlaceholderArt(&painter, result.rect(), title, palette);
        return result;
    }
    QSize scaled = art.size();
    scaled.scale(size, Qt::KeepAspectRatioByExpanding);
    const QRect target((size.width() - scaled.width()) / 2, (size.height() - scaled.height()) / 2,
                       scaled.width(), scaled.height());
    painter.drawPixmap(target, art);
    return result;
}

void LibraryStyleContext::PaintPlaceholderArt(QPainter* painter, const QRect& rect,
                                              const QString& title, const QPalette& palette) {
    const int hue = static_cast<int>(qHash(title) % 360);
    const bool dark = IsDark(palette);
    QLinearGradient gradient(rect.topLeft(), rect.bottomRight());
    gradient.setColorAt(0.0, QColor::fromHsl(hue, 120, dark ? 92 : 150));
    gradient.setColorAt(1.0, QColor::fromHsl((hue + 40) % 360, 130, dark ? 52 : 108));
    painter->fillRect(rect, gradient);

    QString initials;
    for (const QString& word : title.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (word.at(0).isLetterOrNumber()) {
            initials.append(word.at(0).toUpper());
        }
        if (initials.size() == 2) {
            break;
        }
    }
    if (initials.isEmpty()) {
        initials = QStringLiteral("?");
    }
    QFont font = painter->font();
    font.setBold(true);
    font.setPixelSize(std::max(10, std::min(rect.width(), rect.height()) * 2 / 5));
    painter->save();
    painter->setFont(font);
    painter->setPen(QColor(255, 255, 255, 225));
    painter->drawText(rect, Qt::AlignCenter, initials);
    painter->restore();
}

bool LibraryStyleContext::IsDark(const QPalette& palette) {
    return palette.window().color().lightness() < 128;
}

QColor LibraryStyleContext::Accent(const QPalette& palette) {
    QColor accent = palette.highlight().color();
    // Stylesheets set the highlight as a translucent selection fill (the Lichii theme uses its
    // pink at 35 %); a selection ring needs it solid.
    accent.setAlpha(255);
    // Dark themes may also use a deep highlight that disappears as a thin ring on dark art; lift
    // it to a bright tone of the same hue.
    if (IsDark(palette) && accent.lightnessF() < 0.55) {
        float h = 0, s = 0, l = 0, a = 1;
        accent.getHslF(&h, &s, &l, &a);
        accent = QColor::fromHslF(h, std::max(s, 0.8f), 0.58f, a);
    }
    return accent;
}

QColor LibraryStyleContext::Surface(const QPalette& palette) {
    return IsDark(palette) ? QColor(30, 31, 33) : QColor(255, 255, 255);
}

QColor LibraryStyleContext::MutedText(const QPalette& palette) {
    QColor color = palette.text().color();
    color.setAlpha(IsDark(palette) ? 200 : 150);
    return color;
}

int LibraryStyleContext::ArtSize() {
    return std::max(32, static_cast<int>(UISettings::values.game_cover_size.GetValue()));
}

int LibraryStyleContext::FeaturedArtSize() {
    return ArtSize() * 8 / 5;
}

QPixmap LibraryStyleContext::SoftShadow(const QSize& size, qreal radius, int blur, qreal alpha,
                                        qreal dpr) {
    static QHash<QString, QPixmap> cache;
    const QString key = QStringLiteral("%1x%2|%3|%4|%5|%6")
                            .arg(size.width())
                            .arg(size.height())
                            .arg(qRound(radius))
                            .arg(blur)
                            .arg(qRound(alpha * 100))
                            .arg(qRound(dpr * 100));
    if (const auto it = cache.constFind(key); it != cache.constEnd()) {
        return *it;
    }
    if (cache.size() > 64) {
        cache.clear();
    }
    const int b = std::max(1, static_cast<int>(std::lround(blur * dpr)));
    const int w = static_cast<int>(std::lround(size.width() * dpr)) + 2 * b;
    const int h = static_cast<int>(std::lround(size.height() * dpr)) + 2 * b;
    // Alpha mask of the shape, then three box blurs (close to a Gaussian, like CSS blur).
    QImage mask(w, h, QImage::Format_Alpha8);
    mask.fill(0);
    {
        QPainter mp(&mask);
        mp.setRenderHint(QPainter::Antialiasing, true);
        mp.setPen(Qt::NoPen);
        mp.setBrush(QColor(0, 0, 0, 255));
        mp.drawRoundedRect(QRectF(b, b, w - 2 * b, h - 2 * b), radius * dpr, radius * dpr);
    }
    const int r = std::max(1, b / 3);
    std::vector<int> line(static_cast<size_t>(std::max(w, h)));
    const auto blur_pass = [&](bool horizontal) {
        const int outer = horizontal ? h : w;
        const int inner = horizontal ? w : h;
        for (int o = 0; o < outer; ++o) {
            const auto px = [&](int i) -> uchar& {
                return horizontal ? mask.scanLine(o)[i] : mask.scanLine(i)[o];
            };
            for (int i = 0; i < inner; ++i) {
                line[static_cast<size_t>(i)] = px(i);
            }
            int sum = 0;
            for (int i = -r; i <= r; ++i) {
                sum += line[static_cast<size_t>(std::clamp(i, 0, inner - 1))];
            }
            for (int i = 0; i < inner; ++i) {
                px(i) = static_cast<uchar>(sum / (2 * r + 1));
                sum += line[static_cast<size_t>(std::min(i + r + 1, inner - 1))] -
                       line[static_cast<size_t>(std::max(i - r, 0))];
            }
        }
    };
    for (int pass = 0; pass < 3; ++pass) {
        blur_pass(true);
        blur_pass(false);
    }
    QImage shadow(w, h, QImage::Format_ARGB32_Premultiplied);
    shadow.fill(Qt::transparent);
    {
        QPainter sp(&shadow);
        sp.setOpacity(std::clamp(alpha, 0.0, 1.0));
        sp.fillRect(shadow.rect(), Qt::black);
        sp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        sp.setOpacity(1.0);
        sp.drawImage(0, 0, mask);
    }
    QPixmap pixmap = QPixmap::fromImage(shadow);
    pixmap.setDevicePixelRatio(dpr);
    cache.insert(key, pixmap);
    return pixmap;
}

QPixmap LibraryStyleContext::WoodTable(const QSize& size, qreal dpr, bool dark) {
    // Warm tabletop: a radial glow from the upper left plus faint, nearly vertical grain lines.
    // It covers the whole viewport and stays put while the items scroll over it.
    static QPixmap wood_;
    static QString wood_key_;
    const QString key = QStringLiteral("%1x%2|%3|%4")
                            .arg(size.width())
                            .arg(size.height())
                            .arg(static_cast<int>(dpr * 100))
                            .arg(static_cast<int>(dark));
    if (!wood_.isNull() && wood_key_ == key) {
        return wood_;
    }
    const qreal w = std::max(1, size.width());
    const qreal h = std::max(1, size.height());
    QPixmap pixmap(static_cast<int>(std::ceil(w * dpr)), static_cast<int>(std::ceil(h * dpr)));
    pixmap.setDevicePixelRatio(dpr);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Ellipse centred at 30% / 20%, reaching the far corner (CSS "ellipse farthest-corner").
    const QPointF centre(w * 0.3, h * 0.2);
    const qreal rx = std::sqrt(2.0) * (w - centre.x());
    const qreal ry = std::sqrt(2.0) * (h - centre.y());
    QRadialGradient glow(QPointF(0, 0), 1.0);
    if (dark) {
        glow.setColorAt(0.0, QColor(0x6b, 0x4a, 0x33));
        glow.setColorAt(0.6, QColor(0x3d, 0x2a, 0x1d));
        glow.setColorAt(1.0, QColor(0x2a, 0x1c, 0x13));
    } else {
        glow.setColorAt(0.0, QColor(0xe6, 0xc9, 0xa0));
        glow.setColorAt(0.6, QColor(0xcd, 0xa5, 0x77));
        glow.setColorAt(1.0, QColor(0xb4, 0x8c, 0x5f));
    }
    QBrush brush(glow);
    QTransform to_ellipse;
    to_ellipse.translate(centre.x(), centre.y());
    to_ellipse.scale(rx, ry);
    brush.setTransform(to_ellipse);
    p.fillRect(QRectF(0, 0, w, h), brush);

    // Grain: a dark and a light hairline every 41 px, tilted 2 degrees.
    const qreal grain = dark ? 0.25 : 0.2;
    const QColor line_dark(0, 0, 0, static_cast<int>(255 * 0.35 * grain));
    const QColor line_light(255, 220, 180, static_cast<int>(255 * 0.12 * grain));
    p.translate(w / 2, h / 2);
    p.rotate(2.0);
    const qreal reach = std::hypot(w, h) / 2 + 42.0;
    for (qreal x = -reach; x < reach; x += 41.0) {
        p.fillRect(QRectF(x + 18.0, -reach, 1.0, 2 * reach), line_dark);
        p.fillRect(QRectF(x + 41.0, -reach, 1.0, 2 * reach), line_light);
    }
    p.end();

    wood_ = pixmap;
    wood_key_ = key;
    return wood_;
}

void LibraryStyleContext::PaintCover(QPainter* painter, const QRect& rect,
                                     const QModelIndex& index, const QPalette& palette,
                                     int radius) {
    if (rect.isEmpty()) {
        return;
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    QPainterPath clip;
    clip.addRoundedRect(rect, radius, radius);
    painter->setClipPath(clip);
    painter->drawPixmap(rect, Cover(index, rect.size() * painter->device()->devicePixelRatioF(),
                                    palette));
    painter->restore();
}

void LibraryStyleContext::PaintSelection(QPainter* painter, const QRect& rect,
                                         const QPalette& palette, int radius, int width) {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(Accent(palette), width));
    const qreal grow = width / 2.0 + 1.0;
    painter->drawRoundedRect(QRectF(rect).adjusted(-grow, -grow, grow, grow), radius + grow,
                             radius + grow);
    painter->restore();
}

QStringList LibraryStyleContext::NameLines(const QString& text, const QFont& font, int width,
                                           int max_lines, int max_overflow) {
    QStringList out;
    if (text.isEmpty() || width <= 0 || max_lines <= 0) {
        return out;
    }
    // Every frame asks again for the same names: remember the lines instead of laying them out.
    static QHash<QString, QStringList> cache;
    const QString key = font.key() + QLatin1Char('|') + QString::number(width) +
                        QLatin1Char('|') + QString::number(max_lines) + QLatin1Char('|') +
                        QString::number(max_overflow) + QLatin1Char('|') + text;
    if (const auto it = cache.constFind(key); it != cache.constEnd()) {
        return *it;
    }
    if (cache.size() > 4096) {
        cache.clear();
    }
    const QFontMetrics metrics(font);
    // A last letter is narrower than "...": keep the whole line when it overflows by less.
    const int ellipsis = metrics.horizontalAdvance(QChar(0x2026));
    const int slack = std::max(0, max_overflow < 0 ? ellipsis : std::min(max_overflow, ellipsis));
    const auto fit = [&](const QString& part) {
        return metrics.horizontalAdvance(part) <= width + slack
                   ? part
                   : metrics.elidedText(part, Qt::ElideRight, width);
    };
    if (max_lines == 1 || metrics.horizontalAdvance(text) <= width) {
        out.append(fit(text.trimmed()));
        cache.insert(key, out);
        return out;
    }

    // Word wrap only: a word wider than the line stays on its own line and is cut there.
    QTextOption option;
    option.setWrapMode(QTextOption::WordWrap);
    QTextLayout layout(text, font);
    layout.setTextOption(option);
    layout.beginLayout();
    for (int i = 0; i < max_lines; ++i) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) {
            break;
        }
        line.setLineWidth(width);
        if (i == max_lines - 1) {
            out.append(fit(text.mid(line.textStart()).trimmed()));
        } else {
            out.append(fit(text.mid(line.textStart(), line.textLength()).trimmed()));
        }
    }
    layout.endLayout();
    out.removeAll(QString{});
    cache.insert(key, out);
    return out;
}

void LibraryStyleContext::PaintName(QPainter* painter, const QRect& rect, const QString& text,
                                    const QColor& color, const QFont& font,
                                    Qt::Alignment alignment, int max_lines, int max_overflow) {
    if (rect.isEmpty() || text.isEmpty()) {
        return;
    }
    const QFontMetrics metrics(font);
    const int lines = std::max(1, std::min(max_lines, rect.height() / metrics.lineSpacing()));
    const QStringList out = NameLines(text, font, rect.width(), lines, max_overflow);
    const int ellipsis = metrics.horizontalAdvance(QChar(0x2026));
    const int slack = std::max(0, max_overflow < 0 ? ellipsis : std::min(max_overflow, ellipsis));

    painter->save();
    painter->setFont(font);
    painter->setPen(color);
    // Room for the allowed overflow, on both sides when centred.
    const int left = (alignment & Qt::AlignHCenter) ? slack : 0;
    painter->drawText(rect.adjusted(-left, 0, slack, 0), alignment.toInt(),
                      out.join(QLatin1Char('\n')));
    painter->restore();
}

int LibraryStyleContext::TitleLines(int max_lines) {
    return Lichii::ScrollLongNames() ? 1 : max_lines;
}

namespace {

/// Repaints the gliding name of one widget: a single shot timer owned by the widget, started
/// by each paint that drew a gliding name, so it stops on its own once no name glides.
class NameGlideTimer final : public QTimer {
public:
    explicit NameGlideTimer(QWidget* widget) : QTimer{widget} {
        setObjectName(QStringLiteral("libraryNameGlide"));
        setSingleShot(true);
        connect(this, &QTimer::timeout, widget, [this, widget] {
            const QRect rect = pending;
            pending = {};
            widget->update(rect);
        });
    }

    static NameGlideTimer* For(QWidget* widget) {
        if (auto* timer = widget->findChild<QTimer*>(QStringLiteral("libraryNameGlide"),
                                                     Qt::FindDirectChildrenOnly)) {
            return static_cast<NameGlideTimer*>(timer);
        }
        return new NameGlideTimer(widget);
    }

    void Schedule(const QRect& rect, int delay) {
        pending |= rect;
        if (!isActive() || remainingTime() > delay) {
            start(delay);
        }
    }

private:
    QRect pending;
};

} // namespace

void LibraryStyleContext::PaintTitle(QPainter* painter, const QRect& rect, const QString& text,
                                     const QColor& color, const QFont& font,
                                     Qt::Alignment alignment, int max_lines,
                                     const QWidget* glide_in,
                                     int max_overflow) {
    if (rect.isEmpty() || text.isEmpty()) {
        return;
    }
    const int lines = TitleLines(max_lines);
    if (glide_in == nullptr || lines != 1) {
        PaintName(painter, rect, text, color, font, alignment, lines, max_overflow);
        return;
    }
    const QFontMetrics metrics(font);
    const int ellipsis = metrics.horizontalAdvance(QChar(0x2026));
    const int slack = std::max(0, max_overflow < 0 ? ellipsis : std::min(max_overflow, ellipsis));
    const int text_w = metrics.horizontalAdvance(text);
    const int extra = text_w - rect.width();
    if (!Lichii::ScrollLongNames() || extra <= slack) {
        PaintName(painter, rect, text, color, font, alignment, lines, max_overflow);
        return;
    }

    // One line, gliding to its end and back: a pause at each end, 25 ms per pixel between.
    constexpr qint64 pause = 1200;
    constexpr qint64 ms_per_px = 25;
    const qint64 travel = extra * ms_per_px;
    const qint64 t = QDateTime::currentMSecsSinceEpoch() % (2 * (pause + travel));
    qint64 x = 0;
    qint64 next = 0;
    if (t < pause) {
        next = pause - t;
    } else if (t < pause + travel) {
        x = (t - pause) / ms_per_px;
        next = ms_per_px - (t - pause) % ms_per_px;
    } else if (t < 2 * pause + travel) {
        x = extra;
        next = 2 * pause + travel - t;
    } else {
        x = extra - (t - 2 * pause - travel) / ms_per_px;
        next = ms_per_px - (t - 2 * pause - travel) % ms_per_px;
    }

    const int line_h = std::min(rect.height(), metrics.lineSpacing());
    int top = rect.top();
    if (alignment & Qt::AlignBottom) {
        top = rect.bottom() + 1 - line_h;
    } else if (alignment & Qt::AlignVCenter) {
        top = rect.top() + (rect.height() - line_h) / 2;
    }
    const QRect line_rect(rect.left(), top, rect.width(), line_h);
    painter->save();
    painter->setClipRect(line_rect, Qt::IntersectClip);
    painter->setFont(font);
    painter->setPen(color);
    painter->drawText(QRect(line_rect.left() - static_cast<int>(x), top, text_w + 2, line_h),
                      Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, text);
    painter->restore();

    // Repainting is not a change of the widget: a const view may glide its names too.
    NameGlideTimer::For(const_cast<QWidget*>(glide_in))->Schedule(
        painter->transform().mapRect(line_rect).adjusted(-2, -2, 2, 2),
        static_cast<int>(std::max<qint64>(15, next)));
}

namespace {

constexpr std::array<const char*, 4> SampleInfo{"88.8 h played", "v8.88.8", "88.8 GB", "NSP"};

std::array<bool, 4> InfoPartsOn() {
    return {UISettings::values.show_play_time.GetValue(),
            UISettings::values.show_add_ons.GetValue(), UISettings::values.show_size.GetValue(),
            UISettings::values.show_types.GetValue()};
}

/// The four parts of `game`, with placeholders for missing values.
std::array<QString, 4> InfoParts(const GameInfo& game, bool short_play) {
    return {short_play || game.play_time_seconds == 0 ? game.play_time_text
                                                      : Tr("%1 played").arg(game.play_time_text),
            game.update_version.isEmpty() ? Tr("Base")
                                          : QStringLiteral("v") + game.update_version,
            game.size_text.isEmpty() ? QStringLiteral("NaN GB") : game.size_text, game.file_type};
}

} // namespace

InfoLayout LibraryStyleContext::LayoutInfo(const QFont& font, int width, int max_lines,
                                           const GameInfo* only) {
    InfoLayout layout;
    layout.width = width;
    const std::array<bool, 4> on = InfoPartsOn();
    std::array<QString, 4> long_parts;
    std::array<QString, 4> short_parts;
    if (only != nullptr) {
        long_parts = InfoParts(*only, false);
        short_parts = InfoParts(*only, true);
    } else {
        for (size_t i = 0; i < 4; ++i) {
            long_parts[i] = QString::fromLatin1(SampleInfo[i]);
        }
        short_parts = long_parts;
        short_parts[0] = QStringLiteral("88.8 h");
    }

    const QFontMetrics fm(font);
    const QString dot = QStringLiteral(" · ");
    struct Packed {
        std::array<int, 4> line_of{-1, -1, -1, -1};
        int lines = 0;
        bool fits = true;
        int kept = 0;
    };
    // As many parts per line as fit; a part too wide alone gets its own line.
    const auto pack = [&](const std::array<QString, 4>& parts) {
        Packed packed;
        QString current;
        int line = 0;
        for (size_t kind = 0; kind < 4; ++kind) {
            if (!on[kind] || parts[kind].isEmpty()) {
                continue;
            }
            const QString joined = current.isEmpty() ? parts[kind] : current + dot + parts[kind];
            if (!current.isEmpty() && fm.horizontalAdvance(joined) > width) {
                ++line;
                current = parts[kind];
            } else {
                current = joined;
            }
            packed.fits = packed.fits && fm.horizontalAdvance(current) <= width;
            packed.line_of[kind] = line;
            packed.lines = line + 1;
            packed.kept += line < max_lines ? 1 : 0;
        }
        return packed;
    };
    const Packed full = pack(long_parts);
    const Packed shorter = pack(short_parts);
    // The short play time when the long one takes more lines, does not fit or leaves parts out.
    layout.short_play =
        on[0] && (!(full.fits && full.lines <= shorter.lines) || shorter.kept > full.kept);
    const Packed& chosen = layout.short_play ? shorter : full;
    for (size_t kind = 0; kind < 4; ++kind) {
        layout.line_of[kind] = chosen.line_of[kind] < max_lines ? chosen.line_of[kind] : -1;
    }
    layout.lines = std::min(chosen.lines, std::max(0, max_lines));
    return layout;
}

QStringList LibraryStyleContext::InfoLines(const GameInfo& game, const InfoLayout& layout,
                                           const QFont& font) {
    QStringList lines;
    if (!game.is_game || layout.lines <= 0) {
        return lines;
    }
    const std::array<QString, 4> parts = InfoParts(game, layout.short_play);
    const QString short_play = InfoParts(game, true)[0];
    const QFontMetrics fm(font);
    const QString dot = QStringLiteral(" · ");
    for (int line = 0; line < layout.lines; ++line) {
        QStringList on_line;
        for (size_t kind = 0; kind < 4; ++kind) {
            if (layout.line_of[kind] == line && !parts[kind].isEmpty()) {
                on_line << parts[kind];
            }
        }
        // Real values can be longer than the samples: "played" goes first, then whole parts
        // from the end; only a single part too wide for the line is cut.
        if (layout.line_of[0] == line && !on_line.isEmpty() && on_line.first() != short_play &&
            fm.horizontalAdvance(on_line.join(dot)) > layout.width) {
            on_line.first() = short_play;
        }
        while (on_line.size() > 1 && fm.horizontalAdvance(on_line.join(dot)) > layout.width) {
            on_line.removeLast();
        }
        lines << fm.elidedText(on_line.join(dot), Qt::ElideRight, layout.width);
    }
    return lines;
}

void LibraryStyleContext::PaintInfoLines(QPainter* painter, const QRect& rect,
                                         const QStringList& lines, const QColor& color,
                                         const QFont& font, Qt::Alignment alignment) {
    if (lines.isEmpty() || rect.isEmpty()) {
        return;
    }
    const QFontMetrics fm(font);
    const int line_h = fm.lineSpacing();
    const int block = line_h * static_cast<int>(lines.size());
    int y = rect.top();
    if (alignment & Qt::AlignBottom) {
        y = rect.bottom() + 1 - block;
    } else if (alignment & Qt::AlignVCenter) {
        y = rect.top() + (rect.height() - block) / 2;
    }
    const Qt::Alignment horizontal = alignment & Qt::AlignHorizontal_Mask;
    painter->save();
    painter->setFont(font);
    painter->setPen(color);
    for (const QString& line : lines) {
        painter->drawText(QRect(rect.left(), y, rect.width(), line_h),
                          horizontal.toInt() | Qt::AlignVCenter | Qt::TextSingleLine, line);
        y += line_h;
    }
    painter->restore();
}

void LibraryStyleContext::SetFamilyWithFallbacks(QFont& font, const QString& family) {
    QStringList families;
    if (!family.isEmpty()) {
        families << family;
    }
    // Japanese fonts first: Chinese ones lack the Japanese kanji forms of many titles.
    families << QStringLiteral("Yu Gothic UI") << QStringLiteral("Yu Gothic")
             << QStringLiteral("Meiryo UI") << QStringLiteral("Meiryo")
             << QStringLiteral("MS UI Gothic") << QStringLiteral("MS Gothic")
             << QStringLiteral("Noto Sans CJK JP") << QStringLiteral("Noto Sans JP")
             << QStringLiteral("Hiragino Sans") << QStringLiteral("Microsoft YaHei UI")
             << QStringLiteral("Microsoft JhengHei UI") << QStringLiteral("Malgun Gothic")
             << QStringLiteral("PingFang SC") << QFont().family();
    font.setFamilies(families);
}

void LibraryStyleContext::PaintInfoPanel(QPainter* painter, const QRect& rect,
                                         const QModelIndex& index, const QPalette& palette,
                                         const QFont& font) {
    if (rect.isEmpty() || !index.isValid()) {
        return;
    }
    const GameInfo info = Info(index);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    QColor border = Accent(palette);
    border.setAlpha(170);
    painter->setPen(QPen(border, 1.5));
    painter->setBrush(Surface(palette));
    painter->drawRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);

    const QRect inner = rect.adjusted(12, 8, -12, -8);
    QFont title_font = font;
    title_font.setBold(true);
    const QStringList info_lines = InfoLines(info, LayoutInfo(font, inner.width(), 1), font);
    const int info_height = info_lines.isEmpty() ? 0 : QFontMetrics(font).height() + 4;
    PaintTitle(painter, inner.adjusted(0, 0, 0, -info_height), info.title,
               palette.text().color(), title_font, Qt::AlignHCenter | Qt::AlignVCenter);
    if (info_height > 0) {
        PaintInfoLines(painter,
                       QRect(inner.left(), inner.bottom() - info_height, inner.width(),
                             info_height),
                       info_lines, MutedText(palette), font, Qt::AlignHCenter | Qt::AlignVCenter);
    }
    painter->restore();
}

void LibraryStyleContext::PaintCoverCaption(QPainter* painter, const QRect& cover_rect,
                                            const QModelIndex& index, const QFont& font,
                                            const QFont& info_font, bool show_name,
                                            bool show_info, const QWidget* glide_in) {
    if (cover_rect.isEmpty() || !index.isValid()) {
        return;
    }
    const GameInfo info = Info(index);
    // Tinfoil concept: a solid band along the bottom, bold name over one muted info line, both
    // left aligned, padding 5 x 6 px on a 120 px tile.
    const double k = std::clamp(cover_rect.width() / 120.0, 0.8, 1.6);
    const int pad_x = qRound(6 * k);
    const int pad_y = qRound(5 * k);
    QFont title_font = font;
    title_font.setBold(true);
    const QFontMetrics title_metrics(title_font);
    const QFontMetrics info_metrics(info_font);
    const int text_w = cover_rect.width() - 2 * pad_x;
    const QStringList info_lines =
        show_info && info.is_game
            ? InfoLines(info, LayoutInfo(info_font, text_w, 1), info_font)
            : QStringList{};
    const int name_h = show_name ? title_metrics.lineSpacing() : 0;
    const int info_h = info_metrics.lineSpacing() * static_cast<int>(info_lines.size());
    if (name_h + info_h == 0) {
        return;
    }
    const int band_h = std::min(cover_rect.height(), name_h + info_h + 2 * pad_y);
    const QRect band(cover_rect.left(), cover_rect.bottom() - band_h + 1, cover_rect.width(),
                     band_h);
    const bool dark = QGuiApplication::palette().window().color().lightness() < 128;

    painter->save();
    QPainterPath clip;
    clip.addRoundedRect(QRectF(cover_rect), 4, 4);
    painter->setClipPath(clip, Qt::IntersectClip);
    painter->fillRect(band, dark ? QColor(10, 10, 14, 209) : QColor(255, 255, 255, 230));
    const QColor name_color = dark ? QColor(0xe9, 0xe9, 0xec) : QColor(0x1e, 0x1e, 0x22);
    const QColor info_color = dark ? QColor(0xa9, 0xa9, 0xb2) : QColor(0x6c, 0x6c, 0x76);
    const QRect text = band.adjusted(pad_x, pad_y, -pad_x, -pad_y);
    if (show_name) {
        PaintTitle(painter, QRect(text.left(), text.top(), text.width(), name_h), info.title,
                   name_color, title_font, Qt::AlignLeft | Qt::AlignVCenter, 1, glide_in);
    }
    if (info_h > 0) {
        PaintInfoLines(painter, QRect(text.left(), text.top() + name_h, text.width(), info_h),
                       info_lines, info_color, info_font, Qt::AlignLeft | Qt::AlignVCenter);
    }
    painter->restore();
}

namespace {

class PlaceholderView final : public LibraryStyleView {
public:
    PlaceholderView(QWidget* parent, const QString& style_name) : LibraryStyleView{parent} {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 8, 12, 8);
        auto* label = new QLabel(style_name, this);
        QFont font = label->font();
        font.setBold(true);
        label->setFont(font);
        list = new QListView(this);
        list->setEditTriggers(QAbstractItemView::NoEditTriggers);
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        list->setContextMenuPolicy(Qt::CustomContextMenu);
        layout->addWidget(label);
        layout->addWidget(list, 1);
        setFocusProxy(list);

        connect(list, &QListView::doubleClicked, this, &LibraryStyleView::LaunchRequested);
        connect(list, &QListView::customContextMenuRequested, this, [this](const QPoint& pos) {
            emit ContextMenuRequested(list->indexAt(pos), list->viewport()->mapToGlobal(pos));
        });
    }

    void SetModel(QAbstractItemModel* model) override {
        list->setModel(model);
        connect(list->selectionModel(), &QItemSelectionModel::currentChanged, this,
                [this](const QModelIndex& current) { emit CurrentChanged(current); });
    }
    QModelIndex CurrentIndex() const override {
        return list->currentIndex();
    }
    void SetCurrentIndex(const QModelIndex& index) override {
        list->setCurrentIndex(index);
        list->scrollTo(index);
    }
    void SetShowNames(bool) override {}
    void SetShowInfo(bool) override {}

private:
    QListView* list = nullptr;
};

} // namespace

LibraryStyleView* MakePlaceholderView(QWidget* parent, const QString& style_name) {
    return new PlaceholderView(parent, style_name);
}

std::vector<ButtonHint> LibraryStyleContext::StandardHints(const QString& option) {
    std::vector<ButtonHint> hints{{{QStringLiteral("A")}, Tr("Play")},
                                  {{QStringLiteral("B")}, Tr("Names")}};
    if (!option.isEmpty()) {
        hints.push_back({{QStringLiteral("X")}, option});
    }
    hints.push_back({{QStringLiteral("Y")}, Tr("Info")});
    hints.push_back({{QStringLiteral("L"), QStringLiteral("R")}, Tr("Style")});
    return hints;
}

namespace {
constexpr qreal HintGlyphGap = 4.0;
constexpr qreal HintLabelGap = 5.0;

qreal HintItemGap(qreal radius) {
    return std::round(radius * 2.2);
}
} // namespace

bool LibraryStyleContext::IsShoulderGlyph(const QString& glyph) {
    static const QStringList shoulders{QStringLiteral("L"),  QStringLiteral("R"),
                                       QStringLiteral("LB"), QStringLiteral("RB"),
                                       QStringLiteral("ZL"), QStringLiteral("ZR")};
    return shoulders.contains(glyph);
}

qreal LibraryStyleContext::GlyphWidth(const QString& glyph, qreal radius) {
    if (!IsShoulderGlyph(glyph)) {
        return radius * 2;
    }
    return std::round(radius * (glyph.size() > 1 ? 3.3 : 2.7));
}

QRectF LibraryStyleContext::PaintGlyph(QPainter* painter, const QString& glyph, qreal left,
                                       qreal center_y, qreal radius, const QColor& ink,
                                       qreal pen_width, const QFont& font, bool filled,
                                       const QColor& letter) {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    if (filled) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(ink);
    } else {
        painter->setPen(QPen(ink, pen_width));
        painter->setBrush(Qt::NoBrush);
    }
    QRectF box;
    if (IsShoulderGlyph(glyph)) {
        // Shoulder buttons are wide rounded bars, so they never read as the round sticks.
        const qreal h = std::round(radius * 1.7);
        box = QRectF(left, center_y - h / 2, GlyphWidth(glyph, radius), h);
        const qreal corner = h * 0.32;
        painter->drawRoundedRect(box, corner, corner);
    } else {
        box = QRectF(left, center_y - radius, radius * 2, radius * 2);
        painter->drawEllipse(box);
    }
    painter->setFont(font);
    painter->setPen(filled ? letter : ink);
    painter->drawText(box, Qt::AlignCenter, glyph);
    painter->restore();
    return box;
}

qreal LibraryStyleContext::HintsWidth(const std::vector<ButtonHint>& hints, const QFont& font,
                                      qreal radius) {
    const QFontMetricsF fm(font);
    qreal total = 0;
    for (const ButtonHint& hint : hints) {
        for (const QString& glyph : hint.glyphs) {
            total += GlyphWidth(glyph, radius) + HintGlyphGap;
        }
        total += -HintGlyphGap + HintLabelGap + fm.horizontalAdvance(hint.label);
    }
    return total + HintItemGap(radius) * std::max<qreal>(0, static_cast<qreal>(hints.size()) - 1);
}

void LibraryStyleContext::PaintHints(QPainter* painter, const std::vector<ButtonHint>& hints,
                                     qreal left, qreal center_y, const QFont& font, qreal radius,
                                     const QColor& ink, bool filled, const QColor& letter) {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    QFont glyph_font = font;
    glyph_font.setBold(true);
    glyph_font.setPixelSize(std::max(7, static_cast<int>(radius * 1.15)));
    const QFontMetricsF fm(font);
    qreal x = left;
    for (const ButtonHint& hint : hints) {
        for (int i = 0; i < hint.glyphs.size(); ++i) {
            PaintGlyph(painter, hint.glyphs[i], x, center_y, radius, ink,
                       std::max(1.2, radius * 0.15), glyph_font, filled, letter);
            x += GlyphWidth(hint.glyphs[i], radius) +
                 (i + 1 < hint.glyphs.size() ? HintGlyphGap : 0);
        }
        x += HintLabelGap;
        painter->setFont(font);
        painter->setPen(ink);
        const qreal label_w = fm.horizontalAdvance(hint.label);
        painter->drawText(QRectF(x, center_y - fm.height(), label_w + 2, fm.height() * 2),
                          Qt::AlignLeft | Qt::AlignVCenter, hint.label);
        x += label_w + HintItemGap(radius);
    }
    painter->restore();
}

void LibraryStyleContext::PaintHintPill(QPainter* painter, const QRect& area,
                                        const std::vector<ButtonHint>& hints, bool dark) {
    QFont font = painter->font();
    font.setBold(true);
    font.setPixelSize(13);
    const qreal radius = 9.5;
    const qreal width = HintsWidth(hints, font, radius);
    const qreal bar_h = 40.0;
    const QRectF bar(area.center().x() + 1 - width / 2 - 20, area.bottom() + 1 - bar_h - 10,
                     width + 40, bar_h);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(dark ? QColor(20, 16, 14, 190) : QColor(255, 255, 255, 205));
    painter->drawRoundedRect(bar, bar_h / 2, bar_h / 2);
    painter->restore();
    const QColor ink = dark ? QColor(255, 255, 255, 225) : QColor(30, 26, 24, 225);
    PaintHints(painter, hints, bar.left() + 20, bar.center().y(), font, radius, ink);
}

void LibraryStyleContext::PaintLogoTiles(QPainter* painter, const QRectF& area, qreal tile,
                                         qreal opacity) {
    static const QPixmap logo(QStringLiteral(":/icons/default/256x256/lichii.png"));
    if (logo.isNull() || tile < 4 || area.isEmpty()) {
        return;
    }
    const qreal step = tile * 1.7;
    const qreal reach = std::hypot(area.width(), area.height());
    painter->save();
    painter->setClipRect(area, Qt::IntersectClip);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->setOpacity(painter->opacity() * opacity);
    // The rows run on a grid slanted -18 degrees; each icon stays upright at its grid point.
    QTransform slant;
    slant.translate(area.center().x(), area.center().y());
    slant.rotate(-18);
    int row = 0;
    for (qreal y = -reach / 2; y < reach / 2 + step; y += step, ++row) {
        const qreal shift = (row % 2) ? step / 2 : 0;
        for (qreal x = -reach / 2 - step + shift; x < reach / 2 + step; x += step) {
            const QPointF c = slant.map(QPointF(x + tile / 2, y + tile / 2));
            const QRectF target(c.x() - tile / 2, c.y() - tile / 2, tile, tile);
            if (target.intersects(area)) {
                painter->drawPixmap(target, logo, QRectF(logo.rect()));
            }
        }
    }
    painter->restore();
}

} // namespace LibraryStyles
