// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2015 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <array>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include <QByteArrayView>
#include <QCache>
#include <QCoreApplication>
#include <QFileInfo>
#include <QImage>
#include <QLinearGradient>
#include <QMutex>
#include <QObject>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QStandardItem>
#include <QString>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/string_util.h"
#include "frontend_common/play_time_manager.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"

enum class GameListItemType {
    Game = QStandardItem::UserType + 1,
    CustomDir = QStandardItem::UserType + 2,
    SdmcDir = QStandardItem::UserType + 3,
    UserNandDir = QStandardItem::UserType + 4,
    SysNandDir = QStandardItem::UserType + 5,
    AddDir = QStandardItem::UserType + 6,
    Favorites = QStandardItem::UserType + 7,
};

Q_DECLARE_METATYPE(GameListItemType);

/**
 * Gets the default icon (for games without valid title metadata)
 * @param size The desired width and height of the default icon.
 * @return QPixmap default icon
 */
static bool IsLichiiTheme() {
    return UISettings::IsLichiiTheme();
}

static QPixmap GetDefaultIcon(u32 size) {
    if (size == 0 || !IsLichiiTheme()) {
        QPixmap icon(size, size);
        icon.fill(Qt::transparent);
        return icon;
    }
    // Lichii Dark draws a game without art as a tile with a controller glyph.
    const int side = static_cast<int>(size);
    QImage icon(side, side, QImage::Format_ARGB32_Premultiplied);
    icon.fill(Qt::transparent);
    QPainter painter(&icon);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0x23, 0x24, 0x2a));
    painter.drawRoundedRect(QRectF(0, 0, side, side), side * 0.18, side * 0.18);
    const int glyph = side * 3 / 8;
    painter.drawImage(QRect((side - glyph) / 2, (side - glyph) / 2, glyph, glyph),
                      QImage(QStringLiteral(":/lichii/icons/menu_gamepad@2x.png")));
    painter.end();
    return QPixmap::fromImage(icon);
}

inline bool IsFavoriteProgram(u64 program_id) {
    return program_id != 0 && UISettings::values.favorited_ids.contains(program_id);
}

/// The favorite mark everywhere (list, menus, the Favorites row, every library style): a
/// chubby pink heart sticker with a white border and a soft shadow outside it. Drawn once per
/// size and kept. `size`: the pixmap's side in logical pixels. `icon`: upright and filling the
/// square, for menus, the Favorites row and next to names; otherwise tilted like a sticker on
/// a cover, with room for its shadow.
inline QPixmap FavoriteHeart(int size, qreal dpr, bool icon = false, qreal tilt = -10.0) {
    static QCache<QString, QPixmap> cache{48};
    static QMutex mutex;
    size = std::max(8, size);
    const QString key = QStringLiteral("%1|%2|%3|%4").arg(size).arg(dpr).arg(icon).arg(tilt);
    const QMutexLocker lock{&mutex};
    if (const QPixmap* cached = cache.object(key)) {
        return *cached;
    }
    const int px = static_cast<int>(std::ceil(size * dpr));
    QImage image(px, px, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing, true);
        // The heart spans x -0.56..0.56, y -0.48..0.42; with its white edge about 1.4 wide.
        p.translate(px / 2.0, px / 2.0 + (icon ? px * 0.02 : 0.0));
        const qreal unit = icon ? px / 1.42 : px / 2.0;
        p.scale(unit, unit);
        if (!icon) {
            p.rotate(tilt);
        }
        QPainterPath heart;
        heart.moveTo(0.0, 0.42);
        heart.cubicTo(-0.25, 0.28, -0.56, 0.08, -0.56, -0.16);
        heart.cubicTo(-0.56, -0.36, -0.42, -0.48, -0.26, -0.48);
        heart.cubicTo(-0.13, -0.48, -0.04, -0.40, 0.0, -0.30);
        heart.cubicTo(0.04, -0.40, 0.13, -0.48, 0.26, -0.48);
        heart.cubicTo(0.42, -0.48, 0.56, -0.36, 0.56, -0.16);
        heart.cubicTo(0.56, 0.08, 0.25, 0.28, 0.0, 0.42);
        heart.closeSubpath();
        const qreal border = icon ? 0.2 : 0.26; // the white sticker edge (stroke width)
        // Soft shadow outside the white edge, a little lower: stacked faint widenings.
        const QPainterPath low = heart.translated(0.0, icon ? 0.05 : 0.13);
        const int steps = icon ? 3 : 6;
        const qreal strength = icon ? 0.35 : 0.55;
        for (int i = steps; i >= 1; --i) {
            QColor shade(0, 0, 0, static_cast<int>(255 * strength / steps));
            p.setPen(QPen(shade, border + 0.05 * i, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(shade);
            p.drawPath(low);
        }
        p.setPen(QPen(Qt::white, border, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::white);
        p.drawPath(heart);
        QLinearGradient pink(0.0, -0.5, 0.0, 0.5);
        pink.setColorAt(0.0, QColor(0xff, 0x6f, 0xa8));
        pink.setColorAt(1.0, QColor(0xe8, 0x2f, 0x72));
        p.setPen(Qt::NoPen);
        p.setBrush(pink);
        p.drawPath(heart);
    }
    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(dpr);
    cache.insert(key, new QPixmap(pixmap));
    return pixmap;
}

/// The heart sticker centred on `center`, `size` wide (one size per few pixels, so a cover in
/// motion reuses a handful of pixmaps). Upright: drawn in screen space, never through a
/// perspective transform, which is slow for every cover on every frame.
/// `tilt`: 0 where the caller turns the painter itself (with the stickers of a cartridge or case).
inline void PaintFavoriteHeartAt(QPainter* painter, const QPointF& center, qreal size,
                                 qreal tilt = -10.0) {
    const int step = std::max(2, static_cast<int>(size / 12));
    const int px = std::max(12, static_cast<int>(std::lround(size / step)) * step);
    const qreal dpr = painter->device() != nullptr ? painter->device()->devicePixelRatioF() : 1.0;
    painter->drawPixmap(QPointF(center.x() - px / 2.0, center.y() - px / 2.0),
                        FavoriteHeart(px, dpr, false, tilt));
}

/// The size of the heart on art `side` pixels wide: the same share of the art at every size.
inline qreal FavoriteHeartSize(qreal side) {
    return std::max(12.0, side * 0.26);
}

/// The heart on a cover or tile: in its top left corner, sized with the art.
inline void PaintFavoriteHeart(QPainter* painter, const QRectF& art) {
    const qreal side = std::min(art.width(), art.height());
    const qreal size = FavoriteHeartSize(side);
    PaintFavoriteHeartAt(painter, QPointF(art.left() + size * 0.5, art.top() + size * 0.5),
                         size);
}

static QPixmap RoundedIcon(const QPixmap& picture) {
    if (!IsLichiiTheme() || picture.isNull()) {
        return picture;
    }
    QImage icon(picture.size(), QImage::Format_ARGB32_Premultiplied);
    icon.fill(Qt::transparent);
    QPainter painter(&icon);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    const qreal radius = picture.width() * 0.18;
    path.addRoundedRect(QRectF(icon.rect()), radius, radius);
    painter.setClipPath(path);
    painter.drawPixmap(0, 0, picture);
    painter.end();
    return QPixmap::fromImage(icon);
}

static QPixmap ThemeIcon(const char* name) {
    if (IsLichiiTheme()) {
        // Lichii Dark keeps folder rows compact with the 16 px line icons from its set.
        const std::string_view icon{name};
        const char* lichii_name = icon == "sd_card"    ? "sd"
                                  : icon == "chip"     ? "cpu"
                                  : icon == "list-add" ? "plus"
                                  : icon == "star"     ? "star"
                                                       : "folder";
        QPixmap pixmap(
            QStringLiteral(":/lichii/icons/menu_%1@2x.png").arg(QLatin1String(lichii_name)));
        pixmap.setDevicePixelRatio(2.0);
        return pixmap;
    }

    const int size = UISettings::values.folder_icon_size.GetValue();

    return QIcon::fromTheme(QLatin1String(name))
        .pixmap(size, size)
        .scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

class GameListItem : public QStandardItem {

public:
    // used to access type from item index
    static constexpr int TypeRole = Qt::UserRole + 1;
    static constexpr int SortRole = Qt::UserRole + 2;
    GameListItem() = default;
    explicit GameListItem(const QString& string) : QStandardItem(string) {
        setData(string, SortRole);
    }
};

/**
 * A specialization of GameListItem for path values.
 * This class ensures that for every full path value it holds, a correct string representation
 * of just the filename (with no extension) will be displayed to the user.
 * If this class receives valid title metadata, it will also display game icons and titles.
 */
class GameListItemPath : public GameListItem {
public:
    /// 0 for a favorite, 1 otherwise: GameListModel::sort puts favorites first by it.
    static constexpr int FavoriteRankRole = Qt::UserRole + 60;
    static constexpr int TitleRole = SortRole + 1;
    static constexpr int FullPathRole = SortRole + 2;
    static constexpr int ProgramIdRole = SortRole + 3;
    static constexpr int FileTypeRole = SortRole + 4;
    /// True when the picture comes from the game or its custom artwork, not the default art.
    static constexpr int HasIconRole = SortRole + 5;
    static constexpr int CardIconRole = SortRole + 6;
    /// Play time in seconds (qulonglong).
    static constexpr int PlayTimeSecondsRole = SortRole + 7;
    /// Readable play time without a label, or "Never Played".
    static constexpr int PlayTimeTextRole = SortRole + 8;
    /// Version of the applied update ("1.4.3"), empty when only the base game is present.
    static constexpr int UpdateVersionRole = SortRole + 9;

    /// Bounded, shared store for card-sized artwork: decode and scale each game's artwork once
    /// and keep at most MaxCost bytes resident instead of decoding it into every item.
    class CardArtCache {
    public:
        static QPixmap Lookup(const QString& key) {
            const QMutexLocker lock{&Mutex()};
            if (auto* cached = Cache().object(key)) {
                return *cached;
            }
            return {};
        }

        static void Insert(const QString& key, const QPixmap& pixmap) {
            if (pixmap.isNull()) {
                return;
            }
            const QMutexLocker lock{&Mutex()};
            Cache().insert(key, new QPixmap(pixmap),
                           static_cast<int>(std::min<qint64>(
                               qint64(pixmap.width()) * pixmap.height() * pixmap.depth() / 8,
                               MaxCost)));
        }

    private:
        static QCache<QString, QPixmap>& Cache() {
            static QCache<QString, QPixmap> cache{MaxCost};
            return cache;
        }

        static QMutex& Mutex() {
            static QMutex mutex;
            return mutex;
        }

        static constexpr int MaxCost = 32 * 1024 * 1024;
    };

    GameListItemPath() = default;
    GameListItemPath(const QString& game_path, const std::vector<u8>& picture_data,
                     const QString& game_name, const QString& game_type, u64 program_id,
                     u64 play_time, const QString& patch_versions) {
        setData(type(), TypeRole);
        setData(game_path, FullPathRole);
        setData(game_name, TitleRole);
        setData(qulonglong(program_id), ProgramIdRole);
        setData(game_type, FileTypeRole);

        const QString play_time_text =
            play_time > 0
                ? QString::fromStdString(PlayTime::PlayTimeManager::GetReadablePlayTime(play_time))
                : QObject::tr("Never Played");
        const auto readable_play_time =
            play_time > 0 ? QObject::tr("Play Time: %1").arg(play_time_text) : play_time_text;

        const QString update_version = [&patch_versions]() -> QString {
            const QStringList lines = patch_versions.split(QLatin1Char('\n'));
            const QRegularExpression regex{QStringLiteral(R"(^Update \(([0-9\.]+)\))")};
            for (const QString& line : std::as_const(lines)) {
                const auto match = regex.match(line);
                if (match.hasMatch() && match.hasCaptured(1))
                    return match.captured(1);
            }
            return {};
        }();
        const auto enabled_update =
            update_version.isEmpty() ? QObject::tr("Version: 1.0.0")
                                     : QObject::tr("Version: %1").arg(update_version);

        setData(qulonglong(play_time), PlayTimeSecondsRole);
        setData(play_time_text, PlayTimeTextRole);
        setData(update_version, UpdateVersionRole);

        const auto tooltip = QStringLiteral("%1\n%2").arg(readable_play_time, enabled_update);

        setData(tooltip, Qt::ToolTipRole);

        SetPicture(this, picture_data);
    }

    /// The list icon at another size, from the card art the item already keeps: a size change
    /// in the list no longer needs a scan of the library.
    static void ResizePicture(QStandardItem* item, u32 size) {
        if (size == 0) {
            item->setData(QPixmap{}, Qt::DecorationRole);
            return;
        }
        const QPixmap card = item->data(CardIconRole).value<QPixmap>();
        const QPixmap picture =
            item->data(HasIconRole).toBool() && !card.isNull()
                ? card.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                : GetDefaultIcon(size);
        item->setData(RoundedIcon(picture), Qt::DecorationRole);
    }

    /// Shows an image file as a game's picture, or the default art when it is not an image.
    static void SetPicture(QStandardItem* item, const std::vector<u8>& picture_data) {
        const u32 size = UISettings::CurrentGameIconSize().GetValue();

        QPixmap picture;
        const bool has_icon =
            picture.loadFromData(picture_data.data(), static_cast<u32>(picture_data.size()));
        if (!has_icon) {
            picture = GetDefaultIcon(size);
        }
        // Decode and scale the card artwork once, then share it through the bounded cache; the
        // item itself stores a cheap copy-on-write reference.
        const u64 program_id = item->data(ProgramIdRole).toULongLong();
        const QString game_path = item->data(FullPathRole).toString();
        const auto icon_hash = qHash(QByteArrayView{
            reinterpret_cast<const char*>(picture_data.data()),
            static_cast<qsizetype>(picture_data.size())});
        const QString card_cache_key =
            QStringLiteral("%1|%2|%3")
                .arg(qulonglong(program_id))
                .arg(game_path)
                .arg(static_cast<qulonglong>(icon_hash));
        QPixmap card_picture = has_icon ? CardArtCache::Lookup(card_cache_key) : QPixmap{};
        if (has_icon && card_picture.isNull()) {
            const u32 card_target = std::max<u32>(size, u32{384});
            if (std::max(picture.width(), picture.height()) > static_cast<int>(card_target)) {
                card_picture = picture.scaled(card_target, card_target, Qt::KeepAspectRatio,
                                              Qt::SmoothTransformation);
            } else {
                card_picture = picture;
            }
            CardArtCache::Insert(card_cache_key, card_picture);
        }
        picture = picture.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

        item->setData(RoundedIcon(picture), Qt::DecorationRole);
        item->setData(has_icon, HasIconRole);
        item->setData(card_picture, CardIconRole);
    }

    int type() const override {
        return static_cast<int>(GameListItemType::Game);
    }

    QVariant data(int role) const override {
        if (role == FavoriteRankRole) {
            return IsFavoriteProgram(data(ProgramIdRole).toULongLong()) ? 0 : 1;
        }
        if (role == Qt::DisplayRole || role == SortRole) {
            std::string filename;
            Common::SplitPath(data(FullPathRole).toString().toStdString(), nullptr, &filename,
                              nullptr);

            const std::array<QString, 4> row_data{{
                QString::fromStdString(filename),
                data(FileTypeRole).toString(),
                QString::fromStdString(fmt::format("{:016X}", data(ProgramIdRole).toULongLong())),
                data(TitleRole).toString(),
            }};

            const auto& row1 = row_data.at(UISettings::values.row_1_text_id.GetValue());
            // don't show row 2 on grid view
            switch (UISettings::values.game_list_mode.GetValue()) {

            case Settings::GameListMode::TreeView: {
                const int row2_id = UISettings::values.row_2_text_id.GetValue();

                if (role == SortRole) {
                    return row1.toLower();
                }

                // None
                if (row2_id == 4) {
                    return row1;
                }

                const auto& row2 = row_data.at(row2_id);

                if (row1 == row2) {
                    return row1;
                }

                return QStringLiteral("%1\n    %2").arg(row1, row2);
            }
            case Settings::GameListMode::GridView:
            case Settings::GameListMode::CarouselView:
                return row1;
            default:
                break;
            }
        }

        return GameListItem::data(role);
    }
};

class GameListItemCompat : public GameListItem {
    Q_DECLARE_TR_FUNCTIONS(GameListItemCompat)
public:
    static constexpr int CompatNumberRole = SortRole;
    GameListItemCompat() = default;
    explicit GameListItemCompat(const QString& compatibility) {
        setData(type(), TypeRole);

        struct CompatStatus {
            QString color;
            const char* text;
            const char* tooltip;
        };
        // clang-format off
        const auto ingame_status =
                       CompatStatus{QStringLiteral("#f2d624"), QT_TR_NOOP("Ingame"),     QT_TR_NOOP("Game starts, but crashes or major glitches prevent it from being completed.")};
        static const std::map<QString, CompatStatus> status_data = {
            {QStringLiteral("0"),  {QStringLiteral("#5c93ed"), QT_TR_NOOP("Perfect"),    QT_TR_NOOP("Game can be played without issues.")}},
            {QStringLiteral("1"),  {QStringLiteral("#47d35c"), QT_TR_NOOP("Playable"),   QT_TR_NOOP("Game functions with minor graphical or audio glitches and is playable from start to finish.")}},
            {QStringLiteral("2"),  ingame_status},
            {QStringLiteral("3"),  ingame_status}, // Fallback for the removed "Okay" category
            {QStringLiteral("4"),  {QStringLiteral("#FF0000"), QT_TR_NOOP("Intro/Menu"), QT_TR_NOOP("Game loads, but is unable to progress past the Start Screen.")}},
            {QStringLiteral("5"),  {QStringLiteral("#828282"), QT_TR_NOOP("Won't Boot"), QT_TR_NOOP("The game crashes when attempting to startup.")}},
            {QStringLiteral("99"), {QStringLiteral("#000000"), QT_TR_NOOP("Not Tested"), QT_TR_NOOP("The game has not yet been tested.")}},
        };
        // clang-format on

        auto iterator = status_data.find(compatibility);
        if (iterator == status_data.end()) {
            LOG_WARNING(Frontend, "Invalid compatibility number {}", compatibility.toStdString());
            return;
        }
        const CompatStatus& status = iterator->second;
        setData(compatibility, CompatNumberRole);
        setText(tr(status.text));
        setToolTip(tr(status.tooltip));
        setData(QtCommon::CreateCirclePixmapFromColor(status.color), Qt::DecorationRole);
    }

    int type() const override {
        return static_cast<int>(GameListItemType::Game);
    }

    bool operator<(const QStandardItem& other) const override {
        return data(CompatNumberRole).value<QString>() <
               other.data(CompatNumberRole).value<QString>();
    }
};

/**
 * A specialization of GameListItem for size values.
 * This class ensures that for every numerical size value it holds (in bytes), a correct
 * human-readable string representation will be displayed to the user.
 */
class GameListItemSize : public GameListItem {
public:
    static constexpr int SizeRole = SortRole;

    GameListItemSize() = default;
    explicit GameListItemSize(const qulonglong size_bytes) {
        setData(type(), TypeRole);
        setData(size_bytes, SizeRole);
    }

    void setData(const QVariant& value, int role) override {
        // By specializing setData for SizeRole, we can ensure that the numerical and string
        // representations of the data are always accurate and in the correct format.
        if (role == SizeRole) {
            qulonglong size_bytes = value.toULongLong();
            GameListItem::setData(QtCommon::ReadableByteSize(size_bytes), Qt::DisplayRole);
            GameListItem::setData(value, SizeRole);
        } else {
            GameListItem::setData(value, role);
        }
    }

    int type() const override {
        return static_cast<int>(GameListItemType::Game);
    }

    /**
     * This operator is, in practice, only used by the TreeView sorting systems.
     * Override it so that it will correctly sort by numerical value instead of by string
     * representation.
     */
    bool operator<(const QStandardItem& other) const override {
        return data(SizeRole).toULongLong() < other.data(SizeRole).toULongLong();
    }
};

/**
 * GameListItem for Play Time values.
 * This object stores the play time of a game in seconds, and its readable
 * representation in minutes/hours
 */
class GameListItemPlayTime : public GameListItem {
public:
    static constexpr int PlayTimeRole = SortRole;

    GameListItemPlayTime() = default;
    explicit GameListItemPlayTime(const qulonglong time_seconds) {
        setData(time_seconds, PlayTimeRole);
    }

    void setData(const QVariant& value, int role) override {
        qulonglong time_seconds = value.toULongLong();
        GameListItem::setData(
            QString::fromStdString(PlayTime::PlayTimeManager::GetReadablePlayTime(time_seconds)),
            Qt::DisplayRole);
        GameListItem::setData(value, PlayTimeRole);
    }

    bool operator<(const QStandardItem& other) const override {
        return data(PlayTimeRole).toULongLong() < other.data(PlayTimeRole).toULongLong();
    }
};

class GameListDir : public GameListItem {
public:
    static constexpr int GameDirRole = Qt::UserRole + 2;

    explicit GameListDir(UISettings::GameDir& directory,
                         GameListItemType dir_type_ = GameListItemType::CustomDir)
        : dir_type{dir_type_} {
        setData(type(), TypeRole);

        UISettings::GameDir* game_dir = &directory;
        setData(QVariant(UISettings::values.game_dirs.indexOf(directory)), GameDirRole);

        const char* icon_name = nullptr;

        switch (dir_type) {
        case GameListItemType::SdmcDir:
            icon_name = "sd_card";
            setData(QObject::tr("Installed SD Titles"), Qt::DisplayRole);
            break;
        case GameListItemType::UserNandDir:
            icon_name = "chip";
            setData(QObject::tr("Installed NAND Titles"), Qt::DisplayRole);
            break;
        case GameListItemType::SysNandDir:
            icon_name = "chip";
            setData(QObject::tr("System Titles"), Qt::DisplayRole);
            break;
        case GameListItemType::CustomDir: {
            const QString path = QString::fromStdString(game_dir->path);
            icon_name = QFileInfo::exists(path) ? "folder" : "bad_folder";
            setData(path, Qt::DisplayRole);
            break;
        }
        default:
            break;
        }

        if (icon_name != nullptr)
            setData(ThemeIcon(icon_name), Qt::DecorationRole);
    }

    int type() const override {
        return static_cast<int>(dir_type);
    }

    /**
     * Override to prevent automatic sorting between folders and the addDir button.
     */
    bool operator<(const QStandardItem& other) const override {
        return false;
    }

private:
    GameListItemType dir_type;
};

class GameListAddDir : public GameListItem {
public:
    explicit GameListAddDir() {
        setData(type(), TypeRole);

        setData(ThemeIcon("list-add"), Qt::DecorationRole);
        setData(QObject::tr("Add New Game Directory"), Qt::DisplayRole);
    }

    int type() const override {
        return static_cast<int>(GameListItemType::AddDir);
    }

    bool operator<(const QStandardItem& other) const override {
        return false;
    }
};

class GameListFavorites : public GameListItem {
public:
    explicit GameListFavorites() {
        setData(type(), TypeRole);

        setData(FavoriteHeart(IsLichiiTheme() ? 16 : static_cast<int>(
                                  UISettings::values.folder_icon_size.GetValue()),
                              2.0, true),
                Qt::DecorationRole);
        setData(QObject::tr("Favorites"), Qt::DisplayRole);
    }

    int type() const override {
        return static_cast<int>(GameListItemType::Favorites);
    }

    bool operator<(const QStandardItem& other) const override {
        return false;
    }
};
