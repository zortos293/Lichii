// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Box Shelf library style. Concept by inxomnyaa.

#include <algorithm>
#include <cmath>
#include <QAbstractItemModel>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QRadialGradient>
#include <QResizeEvent>
#include <QStringList>
#include <QTimer>
#include <QTransform>
#include <QWheelEvent>
#include <QtMath>

#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/game_list_p.h"
#include "lichii/game/styles/box_shelf_style.h"
#include "lichii/game/styles/style_helpers.h"

namespace LibraryStyles {

namespace {

constexpr double kCaseAspect = 0.62;
constexpr double kTopMargin = 14.0;
constexpr double kBottomMargin = 24.0;

int ItemType(const QModelIndex& index) {
    return index.data(GameListItem::TypeRole).toInt();
}

bool IsGame(const QModelIndex& index) {
    return ItemType(index) == static_cast<int>(GameListItemType::Game);
}

bool IsFolderType(int type) {
    return type == static_cast<int>(GameListItemType::CustomDir) ||
           type == static_cast<int>(GameListItemType::SdmcDir) ||
           type == static_cast<int>(GameListItemType::UserNandDir) ||
           type == static_cast<int>(GameListItemType::SysNandDir);
}

QString TitleOf(const QModelIndex& index) {
    const QString full = index.data(GameListItemPath::TitleRole).toString();
    return full.isEmpty() ? index.data(Qt::DisplayRole).toString().section(QLatin1Char('\n'), 0, 0)
                          : full;
}

/// Stable identity of a game, used as the cache key base.
QString ItemKey(const QModelIndex& index) {
    return index.data(GameListItemPath::FullPathRole).toString() + QLatin1Char('|') +
           QString::number(index.data(GameListItemPath::ProgramIdRole).toULongLong());
}

QPixmap CoverOf(const QModelIndex& index) {
    if (!index.data(GameListItemPath::HasIconRole).toBool()) {
        return {};
    }
    QPixmap pixmap = index.data(GameListItemPath::CardIconRole).value<QPixmap>();
    if (pixmap.isNull()) {
        pixmap = index.data(Qt::DecorationRole).value<QPixmap>();
    }
    return pixmap;
}

QColor PlaceholderColor(const QString& title) {
    const uint hue = static_cast<uint>(qHash(title) % 360u);
    return QColor::fromHsl(static_cast<int>(hue), 150, 104);
}

QString Initials(const QString& title) {
    QString result;
    const QStringList words = title.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& word : words) {
        for (const QChar ch : word) {
            if (ch.isLetterOrNumber()) {
                result.append(ch.toUpper());
                break;
            }
        }
        if (result.size() >= 2) {
            break;
        }
    }
    return result;
}

/// Fills `r` with the cover art (cropped to fill) or a generic placeholder.
void DrawCover(QPainter* p, const QRectF& r, const QModelIndex& index, const QString& title) {
    p->save();
    p->setClipRect(r, Qt::IntersectClip);
    const QPixmap cover = CoverOf(index);
    if (!cover.isNull()) {
        const QSizeF scaled = QSizeF(cover.size()).scaled(r.size(), Qt::KeepAspectRatioByExpanding);
        const QRectF target(r.center().x() - scaled.width() / 2,
                            r.center().y() - scaled.height() / 2, scaled.width(),
                            scaled.height());
        p->drawPixmap(target, cover, QRectF(cover.rect()));
        p->restore();
        return;
    }

    {
        const QColor base = PlaceholderColor(title);
        QLinearGradient g(r.topLeft(), r.bottomRight());
        g.setColorAt(0.0, base.lighter(130));
        g.setColorAt(1.0, base.darker(170));
        p->fillRect(r, g);
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(255, 255, 255, 28));
        p->drawEllipse(QPointF(r.left() + r.width() * 0.78, r.top() + r.height() * 0.2),
                       r.width() * 0.42, r.width() * 0.42);
        QFont f = p->font();
        f.setBold(true);
        f.setPixelSize(std::max(8, static_cast<int>(r.width() * 0.4)));
        p->setFont(f);
        p->setPen(QColor(255, 255, 255, 215));
        p->drawText(r, Qt::AlignCenter, Initials(title));
    }
    p->restore();
}

/// Front outline of a case: square on the spine (left) side, rounded on the right.
QPainterPath CaseShape(double w, double h) {
    const double r = w * 0.035;
    QPainterPath path;
    path.moveTo(0, 0);
    path.lineTo(w - r, 0);
    path.quadTo(w, 0, w, r);
    path.lineTo(w, h - r);
    path.quadTo(w, h, w - r, h);
    path.lineTo(0, h);
    path.closeSubpath();
    return path;
}

/// CSS-like 3D for one case: `perspective(d) translateY(lift) scale(k) rotateY(angle)` with the
/// transform origin at the bottom centre of the case (local x 0..w, y 0..h, z towards viewer).
struct CaseView {
    double w = 0, h = 0, d = 800, angle_deg = 0, scale = 1, lift = 0;
    QPointF origin; // screen position of the bottom centre

    QPointF Map(double x, double y, double z) const {
        const double a = qDegreesToRadians(angle_deg);
        const double dx = (x - w / 2) * scale;
        const double dy = (y - h) * scale;
        const double dz = z * scale;
        const double xr = dx * std::cos(a) + dz * std::sin(a);
        const double zr = -dx * std::sin(a) + dz * std::cos(a);
        const double k = d / std::max(1.0, d - zr);
        return {origin.x() + xr * k, origin.y() + (dy - lift) * k};
    }
    QPolygonF Front() const {
        return QPolygonF{Map(0, 0, 0), Map(w, 0, 0), Map(w, h, 0), Map(0, h, 0)};
    }
    QPolygonF Spine(double depth) const {
        return QPolygonF{Map(0, 0, -depth), Map(0, 0, 0), Map(0, h, 0), Map(0, h, -depth)};
    }
};

/// Sticker in the Cartridges look: rounded tag, soft shadow, tilted 8 degrees.
void DrawSticker(QPainter* p, const QPointF& right_top, const QString& text, const QFont& font,
                 const QColor& fill) {
    const QFontMetricsF fm(font);
    const double w = fm.horizontalAdvance(text) + font.pixelSize() * 1.4;
    const double h = font.pixelSize() * 1.75;
    const QRectF rect(right_top.x() - w, right_top.y(), w, h);
    p->save();
    p->translate(rect.center());
    p->rotate(8.0);
    p->translate(-rect.center());
    const double r = h * 0.28;
    p->setPen(Qt::NoPen);
    p->setBrush(QColor(0, 0, 0, 70));
    p->drawRoundedRect(rect.translated(0, h * 0.12), r, r);
    p->setBrush(fill);
    p->drawRoundedRect(rect, r, r);
    p->setFont(font);
    p->setPen(QColor(0x3b, 0x2a, 0x12));
    p->drawText(rect, Qt::AlignCenter, text);
    p->restore();
}

} // namespace

BoxShelfStyle::BoxShelfStyle(QWidget* parent) : LibraryStyleView(parent), m_pixmaps(64 * 1024) {
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
}

BoxShelfStyle::~BoxShelfStyle() = default;

void BoxShelfStyle::SetModel(QAbstractItemModel* model) {
    if (m_model) {
        QObject::disconnect(m_model.data(), nullptr, this, nullptr);
    }
    m_model = model;
    m_current = QPersistentModelIndex();
    m_current_key.clear();
    m_scroll = 0.0;
    m_gen.clear();
    if (model) {
        const auto schedule = [this] { ScheduleRebuild(); };
        connect(model, &QAbstractItemModel::modelReset, this, schedule);
        connect(model, &QAbstractItemModel::rowsInserted, this, schedule);
        connect(model, &QAbstractItemModel::rowsRemoved, this, schedule);
        connect(model, &QAbstractItemModel::rowsMoved, this, schedule);
        connect(model, &QAbstractItemModel::layoutChanged, this, schedule);
        connect(model, &QAbstractItemModel::dataChanged, this,
                [this](const QModelIndex& top_left, const QModelIndex& bottom_right) {
                    InvalidateRange(top_left, bottom_right);
                    update();
                });
        connect(model, &QObject::destroyed, this, [this] {
            m_entries.clear();
            m_current = QPersistentModelIndex();
            m_current_key.clear();
            m_pixmaps.clear();
            m_gen.clear();
            update();
        });
    }
    RebuildRows();
    update();
}

QModelIndex BoxShelfStyle::CurrentIndex() const {
    return QModelIndex(m_current);
}

void BoxShelfStyle::SetCurrentIndex(const QModelIndex& index) {
    EnsureRows();
    if (!m_model) {
        return;
    }
    if (!index.isValid()) {
        if (m_current.isValid()) {
            m_current = QPersistentModelIndex();
            m_current_key.clear();
            update();
            NotifyCurrent();
        }
        return;
    }
    if (index.model() != m_model.data()) {
        return;
    }
    const QPersistentModelIndex wanted(index.siblingAtColumn(0));
    for (int i = 0; i < ItemCount(); ++i) {
        if (m_entries[static_cast<size_t>(i)] == wanted) {
            SetCurrentPos(i);
            return;
        }
    }
}

void BoxShelfStyle::SetShowNames(bool show) {
    if (m_show_names == show) {
        return;
    }
    m_show_names = show;
    ClampScroll();
    update();
}

void BoxShelfStyle::SetShowInfo(bool show) {
    if (m_show_info == show) {
        return;
    }
    m_show_info = show;
    update();
}

QString BoxShelfStyle::CycleOption() {
    m_spines = !m_spines;
    ClampScroll();
    const int pos = CurrentPos();
    if (pos >= 0) {
        EnsureVisible(pos);
    }
    update();
    return m_spines ? tr("Cases: spine") : tr("Cases: front");
}

// ---------------------------------------------------------------------------------------------
// Model tracking
// ---------------------------------------------------------------------------------------------

void BoxShelfStyle::ScheduleRebuild() {
    if (m_rebuild_pending) {
        return;
    }
    m_rebuild_pending = true;
    QTimer::singleShot(0, this, [this] {
        if (m_rebuild_pending) {
            RebuildRows();
            update();
        }
    });
}

void BoxShelfStyle::EnsureRows() {
    if (m_rebuild_pending) {
        RebuildRows();
    }
}

void BoxShelfStyle::InvalidateRange(const QModelIndex& top_left, const QModelIndex& bottom_right) {
    if (!m_model || !top_left.isValid() || !bottom_right.isValid() || top_left.column() > 0 ||
        bottom_right.column() < 0) {
        return;
    }
    const QModelIndex parent = top_left.parent();
    for (int row = top_left.row(); row <= bottom_right.row(); ++row) {
        const QModelIndex idx = m_model->index(row, 0, parent);
        if (!idx.isValid() || !IsGame(idx)) {
            continue;
        }
        const QString key = ItemKey(idx);
        ++m_gen[key];
    }
}

void BoxShelfStyle::NotifyCurrent() {
    if (m_notifying) {
        return;
    }
    m_notifying = true;
    emit CurrentChanged(QModelIndex(m_current));
    m_notifying = false;
}

void BoxShelfStyle::RebuildRows() {
    m_rebuild_pending = false;
    m_entries.clear();
    // Cached pixmaps stay valid: they are keyed by game identity, size and a per game generation.

    if (!m_model) {
        m_current = QPersistentModelIndex();
        m_current_key.clear();
        return;
    }

    // Games are shown flat: top level games plus the games inside each folder row. Favorites and
    // the "add folder" row are not shown.
    const int top_rows = m_model->rowCount();
    for (int r = 0; r < top_rows; ++r) {
        const QModelIndex idx = m_model->index(r, 0);
        const int type = ItemType(idx);
        if (type == static_cast<int>(GameListItemType::Game)) {
            m_entries.emplace_back(idx);
        } else if (IsFolderType(type)) {
            const int children = m_model->rowCount(idx);
            for (int c = 0; c < children; ++c) {
                const QModelIndex child = m_model->index(c, 0, idx);
                if (IsGame(child)) {
                    m_entries.emplace_back(child);
                }
            }
        }
    }
    ClampScroll();

    if (CurrentPos() >= 0) {
        return;
    }
    // The current game may have come back as a new index (model reset): find it by identity.
    if (!m_current_key.isEmpty()) {
        for (int i = 0; i < ItemCount(); ++i) {
            if (ItemKey(IndexAtPos(i)) == m_current_key) {
                m_current = m_entries[static_cast<size_t>(i)];
                EnsureVisible(i);
                return;
            }
        }
    }
    if (m_entries.empty()) {
        const bool had = m_current.isValid();
        m_current = QPersistentModelIndex();
        m_current_key.clear();
        if (had) {
            NotifyCurrent();
        }
    } else {
        m_current = m_entries.front();
        m_current_key = ItemKey(QModelIndex(m_current));
        EnsureVisible(0);
        NotifyCurrent();
    }
}

// ---------------------------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------------------------

BoxShelfStyle::Metrics BoxShelfStyle::ComputeMetrics() const {
    Metrics m;
    const double w = std::max(1, width());
    const double usable = std::max(60.0, w - 48.0);
    if (!m_spines) {
        // The front art is View > Game Icon Size wide; columns leave room for the turned
        // spine, the stickers and the concept's 26 px gap.
        m.case_w = std::min<double>(LibraryStyleContext::ArtSize(), (usable - 26.0) / 1.12);
        m.cols = std::max(1, static_cast<int>(usable / (m.case_w * 1.12 + 26.0)));
        m.cell_w = usable / m.cols;
        m.case_h = m.case_w / kCaseAspect;
        m.face_w = m.case_w;
    } else {
        // Spines: the same case height as the front view, the spine ~13 % of it.
        m.face_w = std::min<double>(LibraryStyleContext::ArtSize(), usable / 1.12);
        m.case_h = m.face_w / kCaseAspect;
        m.case_w = std::clamp(m.case_h * 0.13, 16.0, 64.0);
        m.cell_w = m.case_w + std::max(3.0, m.case_w * 0.12);
        m.cols = std::max(1, static_cast<int>(usable / m.cell_w));
        m.cell_w = usable / m.cols > m.cell_w * 1.25 ? m.cell_w : usable / m.cols;
    }
    // Front with names on: every case gets a bubble of up to two lines underneath. Spines:
    // only the selected one does, but its row keeps the room.
    if (m_show_names || m_spines) {
        // Matches DrawNameBubble: gap, two lines and padding, scaled with the case.
        const double bs = std::clamp(m.face_w / 160.0, 0.65, 1.0);
        QFont f = font();
        f.setPixelSize(std::max(9, static_cast<int>(std::lround(13.0 * bs))));
        f.setBold(true);
        m.bubble_h = 22.0 * bs + QFontMetrics(f).lineSpacing() * 2 + 16.0 * bs + 6.0;
    } else {
        m.bubble_h = 0.0;
    }
    // Spines: a short shelf (fewer games than fit in a row) is centred too.
    const int shown_cols = m_spines ? std::clamp(ItemCount(), 1, m.cols) : m.cols;
    m.left = (w - shown_cols * m.cell_w) / 2.0;
    // The selected case grows 10 % upwards from its base and lifts (front) or lifts (spines),
    // plus its ring and stickers; keep the first row clear of the top edge.
    m.top_pad = m.case_h * 0.1 + m.face_w * 0.2 + 8.0;
    m.row_h = m.top_pad + m.case_h + std::max(34.0, m.bubble_h) + 10.0;
    return m;
}

QModelIndex BoxShelfStyle::IndexAtPos(int pos) const {
    if (pos < 0 || pos >= ItemCount()) {
        return {};
    }
    return QModelIndex(m_entries[static_cast<size_t>(pos)]);
}

int BoxShelfStyle::CurrentPos() const {
    if (!m_current.isValid()) {
        return -1;
    }
    for (int i = 0; i < ItemCount(); ++i) {
        if (m_entries[static_cast<size_t>(i)] == m_current) {
            return i;
        }
    }
    return -1;
}

QRectF BoxShelfStyle::ItemRect(const Metrics& m, int pos) const {
    const int row = pos / m.cols;
    const int col = pos % m.cols;
    const double x = m.left + col * m.cell_w + (m.cell_w - m.case_w) / 2.0;
    const double y = kTopMargin + row * m.row_h + m.top_pad - m_scroll;
    return QRectF(x, y, m.case_w, m.case_h);
}

int BoxShelfStyle::HitTest(const QPointF& point) const {
    const int count = ItemCount();
    if (count == 0) {
        return -1;
    }
    const Metrics m = ComputeMetrics();
    const double fy = point.y() + m_scroll - kTopMargin;
    const double fx = point.x() - m.left;
    if (fy < 0.0 || fx < 0.0) {
        return -1;
    }
    const int row = static_cast<int>(fy / m.row_h);
    const int col = static_cast<int>(fx / m.cell_w);
    if (col >= m.cols) {
        return -1;
    }
    const int pos = row * m.cols + col;
    if (pos < 0 || pos >= count) {
        return -1;
    }
    const QRectF rect = ItemRect(m, pos).adjusted(-4, -12, 4, 6);
    return rect.contains(point) ? pos : -1;
}

double BoxShelfStyle::MaxScroll(const Metrics& m) const {
    const int count = ItemCount();
    const int rows = (count + m.cols - 1) / m.cols;
    // The hint pill covers the bottom; the last row scrolls clear of it.
    const double total = kTopMargin + rows * m.row_h + kBottomMargin + LibraryStyleContext::HintPillReserve;
    return std::max(0.0, total - height());
}

void BoxShelfStyle::ClampScroll() {
    m_scroll = std::clamp(m_scroll, 0.0, MaxScroll(ComputeMetrics()));
}

void BoxShelfStyle::EnsureVisible(int pos) {
    if (pos < 0 || pos >= ItemCount()) {
        return;
    }
    const Metrics m = ComputeMetrics();
    const int row = pos / m.cols;
    const double top = kTopMargin + row * m.row_h;
    const double bottom = top + m.row_h;
    if (top < m_scroll) {
        m_scroll = top;
    } else if (bottom > m_scroll + height() - LibraryStyleContext::HintPillReserve) {
        m_scroll = bottom - height() + LibraryStyleContext::HintPillReserve;
    }
    ClampScroll();
}

void BoxShelfStyle::SetCurrentPos(int pos) {
    const int count = ItemCount();
    if (count == 0) {
        return;
    }
    pos = std::clamp(pos, 0, count - 1);
    if (pos == CurrentPos()) {
        EnsureVisible(pos);
        update();
        return;
    }
    m_current = m_entries[static_cast<size_t>(pos)];
    m_current_key = ItemKey(QModelIndex(m_current));
    EnsureVisible(pos);
    update();
    NotifyCurrent();
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

void BoxShelfStyle::keyPressEvent(QKeyEvent* event) {
    if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
        QWidget::keyPressEvent(event);
        return;
    }
    EnsureRows();
    const int count = ItemCount();
    if (count == 0) {
        QWidget::keyPressEvent(event);
        return;
    }
    const Metrics m = ComputeMetrics();
    int pos = CurrentPos();
    if (pos < 0) {
        pos = 0;
    }
    const int last_row = (count - 1) / m.cols;
    const int visible_rows = std::max(1, static_cast<int>(height() / m.row_h));

    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_A:
        SetCurrentPos(pos - 1);
        break;
    case Qt::Key_Right:
    case Qt::Key_D:
        SetCurrentPos(pos + 1);
        break;
    case Qt::Key_Up:
    case Qt::Key_W:
        if (pos - m.cols >= 0) {
            SetCurrentPos(pos - m.cols);
        }
        break;
    case Qt::Key_Down:
    case Qt::Key_S:
        if (pos + m.cols < count) {
            SetCurrentPos(pos + m.cols);
        } else if (pos / m.cols < last_row) {
            SetCurrentPos(count - 1);
        }
        break;
    case Qt::Key_PageUp:
        SetCurrentPos(std::max(0, pos - m.cols * visible_rows));
        break;
    case Qt::Key_PageDown:
        SetCurrentPos(std::min(count - 1, pos + m.cols * visible_rows));
        break;
    case Qt::Key_Home:
        SetCurrentPos(0);
        break;
    case Qt::Key_End:
        SetCurrentPos(count - 1);
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (m_current.isValid()) {
            emit LaunchRequested(QModelIndex(m_current));
        }
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

void BoxShelfStyle::mousePressEvent(QMouseEvent* event) {
    EnsureRows();
    setFocus(Qt::MouseFocusReason);
    if (event->button() == Qt::LeftButton || event->button() == Qt::RightButton) {
        const int pos = HitTest(event->position());
        if (pos >= 0) {
            SetCurrentPos(pos);
            if (event->button() == Qt::RightButton && m_current.isValid()) {
                emit ContextMenuRequested(QModelIndex(m_current),
                                          event->globalPosition().toPoint());
            }
            event->accept();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void BoxShelfStyle::mouseDoubleClickEvent(QMouseEvent* event) {
    EnsureRows();
    if (event->button() == Qt::LeftButton) {
        const int pos = HitTest(event->position());
        if (pos >= 0) {
            SetCurrentPos(pos);
            if (m_current.isValid()) {
                emit LaunchRequested(QModelIndex(m_current));
            }
            event->accept();
            return;
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void BoxShelfStyle::wheelEvent(QWheelEvent* event) {
    if (event->modifiers() & Qt::ControlModifier) {
        event->ignore();
        return;
    }
    // Touchpads report exact pixels; a notched wheel reports 120 units per step.
    double dy = 0.0;
    if (!event->pixelDelta().isNull()) {
        dy = event->pixelDelta().y();
    } else {
        dy = event->angleDelta().y() * 0.6;
    }
    if (dy != 0.0) {
        m_scroll -= dy;
        ClampScroll();
        update();
    }
    event->accept();
}

void BoxShelfStyle::resizeEvent(QResizeEvent* event) {
    LibraryStyleView::resizeEvent(event);
    ClampScroll();
    update();
}

// ---------------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------------

bool BoxShelfStyle::IsDark() const {
    return palette().window().color().lightness() < 128;
}

void BoxShelfStyle::paintEvent([[maybe_unused]] QPaintEvent* event) {
    EnsureRows();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const bool dark = IsDark();
    // The same wooden tabletop as the Cartridges style.
    p.drawPixmap(QPointF(0, 0), LibraryStyleContext::WoodTable(size(), devicePixelRatioF(), dark));

    const int count = ItemCount();
    if (count == 0) {
        p.setPen(palette().text().color());
        p.drawText(rect(), Qt::AlignCenter, tr("No games to show"));
        return;
    }

    ClampScroll();
    const Metrics m = ComputeMetrics();
    const int rows = (count + m.cols - 1) / m.cols;
    const int first_row =
        std::clamp(static_cast<int>((m_scroll - kTopMargin) / m.row_h), 0, rows - 1);
    const int last_row =
        std::clamp(static_cast<int>((m_scroll + height() - kTopMargin) / m.row_h), 0, rows - 1);
    const int selected_pos = CurrentPos();

    for (int row = first_row; row <= last_row; ++row) {
        const int begin = row * m.cols;
        const int end = std::min(count, begin + m.cols);
        for (int pos = begin; pos < end; ++pos) {
            if (pos != selected_pos) {
                DrawItem(p, m, pos, false, dark);
            }
        }
    }
    if (!m_spines && m_show_names) {
        for (int pos = first_row * m.cols; pos < std::min(count, (last_row + 1) * m.cols); ++pos) {
            if (pos != selected_pos) {
                DrawNameBubble(p, m, ItemRect(m, pos), TitleOf(IndexAtPos(pos)), false);
            }
        }
    }
    if (selected_pos >= 0 && selected_pos / m.cols >= first_row - 1 &&
        selected_pos / m.cols <= last_row + 1) {
        DrawItem(p, m, selected_pos, true, dark);
        // The selected case always names itself.
        DrawNameBubble(p, m, ItemRect(m, selected_pos), TitleOf(IndexAtPos(selected_pos)), true);
    }

    // Slim scroll indicator.
    const double max_scroll = MaxScroll(m);
    if (max_scroll > 0.0) {
        const double h = height();
        const double total = h + max_scroll;
        const double handle = std::max(28.0, h * h / total);
        const double y = (h - handle) * (m_scroll / max_scroll);
        QColor c = palette().text().color();
        c.setAlpha(80);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(width() - 7.0, y + 2.0, 4.0, handle - 4.0), 2, 2);
    }

    LibraryStyleContext::PaintHintPill(&p, rect(),
                                       LibraryStyleContext::StandardHints(m_spines ? tr("Front") : tr("Spine")), dark);
}

QString BoxShelfStyle::PixmapKey(QChar kind, const QModelIndex& index, const Metrics& m, bool dark,
                                 double dpr) const {
    // Built by concatenation: a game path may contain "%1", which QString::arg would substitute.
    const QString base = ItemKey(index);
    return QString(kind) + QLatin1Char('|') + base + QLatin1Char('|') +
           QString::number(m_gen.value(base)) + QLatin1Char('|') +
           QString::number(qRound(m.case_w * 10)) + QLatin1Char('x') +
           QString::number(qRound(m.case_h * 10)) + QLatin1Char('|') +
           QString::number(dark ? 1 : 0) + QLatin1Char('|') + QString::number(dpr);
}

QPixmap BoxShelfStyle::CasePixmap(const QModelIndex& index, const Metrics& m, bool dark) {
    const double dpr = devicePixelRatioF();
    const QString key = PixmapKey(QLatin1Char('C'), index, m, dark, dpr);
    if (const QPixmap* cached = m_pixmaps.object(key)) {
        return *cached;
    }

    // The case seen from the front, after the Box shelf concept. Newer-generation titles: a red
    // case with a header band. Others: a clear case (pale frame, open on the spine side) with a
    // red corner tab. The glyph is a neutral rounded pill; no console names.
    const double w = m.case_w;
    const double h = m.case_h;
    const double u = w / 150.0; // concept units: the concept case is 150 px wide
    QPixmap pm(QSize(std::max(1, qCeil(w * dpr)), std::max(1, qCeil(h * dpr))));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const bool is_game = IsGame(index);
    const LibraryStyles::GameInfo info =
        is_game ? LibraryStyleContext::Info(index) : LibraryStyles::GameInfo{};
    const bool nx2 = info.is_nx2 || (info.program_id >> 56) == 0x04;
    const QColor red(0xe3, 0x14, 0x1f);
    const QColor clear(226, 228, 233, 242);

    const QPainterPath shape = CaseShape(w, h);
    p.setClipPath(shape);
    const double border = std::max(2.0, 3.0 * u);
    p.fillRect(QRectF(0, 0, w, h), nx2 ? red : clear);

    const auto pill = [&](const QPointF& center, double pw, double ph) {
        p.setPen(QPen(Qt::white, std::max(1.2, 2.0 * u)));
        p.setBrush(Qt::NoBrush);
        const double r = std::min(pw, ph) * 0.33;
        p.drawRoundedRect(QRectF(center.x() - pw / 2, center.y() - ph / 2, pw, ph), r, r);
    };

    QRectF art;
    if (nx2) {
        const QRectF band(border, border, w - 2 * border, h * 0.14 - border);
        art = QRectF(border, band.bottom(), w - 2 * border, h - band.bottom() - border);
        QFont two = font();
        two.setBold(true);
        two.setPixelSize(std::max(7, qRound(16.0 * u)));
        const double two_w = QFontMetricsF(two).horizontalAdvance(QStringLiteral("2"));
        const double gw = 12.0 * u;
        const double gap = 3.0 * u;
        const double x0 = band.center().x() - (gw + gap + two_w) / 2;
        pill(QPointF(x0 + gw / 2, band.center().y()), gw, 15.0 * u);
        p.setFont(two);
        p.setPen(Qt::white);
        p.drawText(QRectF(x0 + gw + gap, band.top(), two_w + 2, band.height()),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("2"));
    } else {
        art = QRectF(0, border, w - border, h - 2 * border);
    }
    DrawCover(&p, art, index, TitleOf(index));
    if (!nx2 && is_game) {
        const double corner = w * 0.3;
        p.fillRect(QRectF(art.left(), art.top(), corner, corner), red);
        pill(QPointF(art.left() + corner / 2, art.top() + corner / 2), 13.0 * u, 16.0 * u);
    }

    // Gloss from the top left.
    QLinearGradient gloss(0, 0, w, w * 0.18);
    gloss.setColorAt(0.0, QColor(255, 255, 255, 51));
    gloss.setColorAt(0.35, QColor(255, 255, 255, 0));
    p.fillRect(QRectF(0, 0, w, h), gloss);
    p.end();

    const int cost = std::max(1, pm.width() * pm.height() * 4 / 1024);
    m_pixmaps.insert(key, new QPixmap(pm), cost);
    return pm;
}

QPixmap BoxShelfStyle::SpinePixmap(const QModelIndex& index, const Metrics& m, bool dark) {
    const double dpr = devicePixelRatioF();
    const QString key = PixmapKey(QLatin1Char('S'), index, m, dark, dpr);
    if (const QPixmap* cached = m_pixmaps.object(key)) {
        return *cached;
    }

    const double w = m.case_w;
    const double h = m.case_h;
    QPixmap pm(QSize(std::max(1, qCeil(w * dpr)), std::max(1, qCeil(h * dpr))));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    // The red spine of the turned cases in the front view; the darker red one for NX2 games.
    const LibraryStyles::GameInfo info =
        IsGame(index) ? LibraryStyleContext::Info(index) : LibraryStyles::GameInfo{};
    const bool nx2 = info.is_nx2 || (info.program_id >> 56) == 0x04;
    const QColor base = nx2 ? QColor(0xb4, 0x0d, 0x17) : QColor(0xe3, 0x14, 0x1f);
    const QString title = TitleOf(index);
    QPainterPath shape;
    shape.addRoundedRect(QRectF(0.5, 0.5, w - 1.0, h - 1.0), std::min(3.0, w * 0.12),
                         std::min(3.0, w * 0.12));
    p.setClipPath(shape);

    QLinearGradient g(0, 0, w, 0);
    g.setColorAt(0.0, base.darker(112));
    g.setColorAt(0.4, base.lighter(108));
    g.setColorAt(1.0, base.darker(118));
    p.fillRect(QRectF(0, 0, w, h), g);

    // Small cover thumbnail at the top, title running down the spine.
    const QRectF thumb(3.0, 3.0, w - 6.0, w - 6.0);
    DrawCover(&p, thumb, index, title);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(255, 255, 255, 70), 1.0));
    p.drawRect(thumb);

    QFont f = font();
    f.setBold(true);
    f.setPixelSize(std::clamp(static_cast<int>(w * 0.42), 9, 22));
    p.setFont(f);
    p.setPen(QColor(255, 255, 255));
    const double top = w + 2.0;
    const double avail = std::max(10.0, h - top - 6.0);
    p.save();
    p.translate(w / 2.0, top);
    p.rotate(90.0);
    // One line down the spine (S4); a name that only just overflows may use the 6 px margin.
    LibraryStyleContext::PaintName(&p, QRectF(0, -w / 2.0, avail, w).toAlignedRect(), title,
                                   p.pen().color(), f, Qt::AlignVCenter | Qt::AlignLeft, 1, 6);
    p.restore();

    p.setClipping(false);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 110), 1.0));
    p.drawPath(shape);
    p.end();

    const int cost = std::max(1, pm.width() * pm.height() * 4 / 1024);
    m_pixmaps.insert(key, new QPixmap(pm), cost);
    return pm;
}

void BoxShelfStyle::DrawItem(QPainter& p, const Metrics& m, int pos, bool selected, bool dark) {
    if (!m_spines) {
        DrawFront(p, m, pos, selected);
        return;
    }
    const QModelIndex index = IndexAtPos(pos);
    if (!index.isValid()) {
        return;
    }
    const QRectF base = ItemRect(m, pos);
    if (base.bottom() < -60.0 || base.top() > height() + 60.0) {
        return;
    }
    // Like the front view: every case stands 6 concept px up, the selected one grows 10 % from
    // its bottom centre. Unselected spines match the near (spine) edge of a turned front case,
    // which the perspective makes 4 % taller.
    const double u = m.face_w / 150.0;
    const double k = selected ? 1.1 : 1.04;
    const double w = m.case_w * k;
    const double h = m.case_h * k;
    const QRectF r(base.center().x() - w / 2.0, base.bottom() - 6.0 * u - h, w, h);

    // Soft shadow like the front view's cases: a few grown, faint copies of the outline.
    {
        const double radius = std::min(3.0, w * 0.12);
        p.save();
        p.setPen(Qt::NoPen);
        for (int i = 1; i <= 5; ++i) {
            p.setBrush(QColor(0, 0, 0, selected ? 22 : 18));
            p.drawRoundedRect(r.adjusted(-0.6 * i, -0.4 * i, 0.6 * i, 0.4 * i)
                                  .translated(0, selected ? 10.0 : 5.0),
                              radius + 0.6 * i, radius + 0.6 * i);
        }
        p.restore();
    }

    // Unselected: a pixel aligned blit, no scaled (slow) pixmap drawing per case and frame.
    const QPixmap pix = SpinePixmap(index, m, dark);
    const QRectF target = selected ? r : QRectF(std::round(r.x()), std::round(r.y()), w, h);
    p.drawPixmap(target, pix, QRectF(pix.rect()));

    if (selected) {
        // The front view's ring: 4 concept px out, 4 px wide.
        const double grow = 4.0 * u;
        const double radius = std::min(3.0, w * 0.12) + grow;
        p.save();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(LibraryStyleContext::Accent(palette()), std::max(2.5, 4.0 * u),
                      Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        p.drawRoundedRect(target.adjusted(-grow, -grow, grow, grow), radius, radius);
        p.restore();
    }

    // Stickers: like Dense Grid, Info shows them on the selected case only, beside its spine.
    if (m_show_info && selected && IsGame(index)) {
        const LibraryStyles::GameInfo info = LibraryStyleContext::Info(index);
        QFont f = font();
        f.setWeight(QFont::Black);
        f.setPixelSize(std::clamp(qRound(12.0 * u), 9, 16));
        const double tag_h = f.pixelSize() * 1.75;
        const QFontMetricsF tag_metrics(f);
        double widest = 0.0;
        if (info.is_game && UISettings::values.show_play_time.GetValue()) {
            widest = tag_metrics.horizontalAdvance(info.play_time_text);
        }
        if (UISettings::values.show_add_ons.GetValue() && !info.update_version.isEmpty()) {
            widest = std::max(widest, tag_metrics.horizontalAdvance(QStringLiteral("v") +
                                                                     info.update_version));
        }
        widest += f.pixelSize() * 1.4;
        QPointF anchor(r.right() + 6.0 * u + widest, r.top() + h * 0.17);
        if (anchor.x() > width() - 6.0) {
            anchor.setX(r.left() - 6.0 * u); // no room on the right: stick them on the left
        }
        if (info.is_game && UISettings::values.show_play_time.GetValue()) { // "New" if unplayed
            DrawSticker(&p, anchor, info.play_time_text, f, QColor(0xff, 0xd3, 0x6b));
            anchor.ry() += tag_h + 6.0 * u;
        }
        if (UISettings::values.show_add_ons.GetValue() && !info.update_version.isEmpty()) {
            DrawSticker(&p, anchor, QStringLiteral("v") + info.update_version, f,
                        QColor(0xff, 0xf3, 0xc9));
        }
    }
}

void BoxShelfStyle::DrawFront(QPainter& p, const Metrics& m, int pos, bool selected) {
    const QModelIndex index = IndexAtPos(pos);
    if (!index.isValid()) {
        return;
    }
    const QRectF base = ItemRect(m, pos);
    if (base.bottom() < -80.0 || base.top() > height() + 80.0) {
        return;
    }
    const double w = m.case_w;
    const double h = m.case_h;
    const double u = w / 150.0;

    // Unselected cases are turned so the spine shows; the selected one turns to face you.
    CaseView view;
    view.w = w;
    view.h = h;
    view.d = 800.0 * u;
    view.angle_deg = selected ? 0.0 : 24.0;
    view.scale = selected ? 1.1 : 1.0;
    view.lift = 6.0 * u;
    view.origin = QPointF(base.center().x(), base.bottom());
    const QPolygonF front = view.Front();
    const QRectF rect_src(0, 0, w, h);

    // Soft drop shadow under the front.
    p.save();
    p.setPen(Qt::NoPen);
    for (int i = 1; i <= 5; ++i) {
        p.setBrush(QColor(0, 0, 0, selected ? 22 : 18));
        const QPointF c = front.boundingRect().center();
        QTransform grow;
        grow.translate(c.x(), c.y() + (selected ? 12.0 : 7.0) * u);
        grow.scale(1.0 + 0.012 * i, 1.0 + 0.008 * i);
        grow.translate(-c.x(), -c.y());
        p.drawPolygon(grow.map(front));
    }
    p.restore();

    const LibraryStyles::GameInfo info =
        IsGame(index) ? LibraryStyleContext::Info(index) : LibraryStyles::GameInfo{};
    const bool nx2 = info.is_nx2 || (info.program_id >> 56) == 0x04;

    // Spine face, hinged on the left edge and folded back.
    if (!selected) {
        const double depth = 13.0 * u;
        QTransform spine_map;
        if (QTransform::quadToQuad(QPolygonF(QRectF(0, 0, depth, h)), view.Spine(depth),
                                   spine_map)) {
            p.save();
            p.setTransform(spine_map, true);
            p.setPen(Qt::NoPen);
            QLinearGradient g(0, 0, depth, 0);
            if (nx2) {
                g.setColorAt(0.0, QColor(0xa9, 0x0c, 0x15));
                g.setColorAt(0.4, QColor(0xc9, 0x11, 0x1c));
                g.setColorAt(1.0, QColor(0xa9, 0x0c, 0x15));
                p.fillRect(QRectF(0, 0, depth, h), g);
            } else {
                // Clear plastic with the red cover sheet showing through.
                g.setColorAt(0.0, QColor(0xd6, 0xd8, 0xde));
                g.setColorAt(0.4, QColor(0xee, 0xf0, 0xf3));
                g.setColorAt(1.0, QColor(0xd6, 0xd8, 0xde));
                p.fillRect(QRectF(0, 0, depth, h), g);
                QLinearGradient sheet(0, 0, depth, 0);
                sheet.setColorAt(0.0, QColor(0xc8, 0x10, 0x1b));
                sheet.setColorAt(0.4, QColor(0xe3, 0x14, 0x1f));
                sheet.setColorAt(1.0, QColor(0xc8, 0x10, 0x1b));
                // Full height: a clear stub under the sheet read as the spine poking out.
                p.fillRect(QRectF(0, 0, depth - 1.0, h), sheet);
            }
            p.restore();
        }
    }

    // Front face.
    QTransform front_map;
    if (QTransform::quadToQuad(QPolygonF(rect_src), front, front_map)) {
        const QPixmap pix = CasePixmap(index, m, IsDark());
        p.save();
        p.setTransform(front_map, true);
        p.drawPixmap(rect_src, pix, QRectF(pix.rect()));
        if (selected) {
            // Ring just outside the case, following its square-left, rounded-right outline.
            const double grow = 4.0 * u / view.scale;
            QPainterPath ring = CaseShape(w + 2 * grow, h + 2 * grow);
            ring.translate(-grow, -grow);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(LibraryStyleContext::Accent(palette()), std::max(2.5, 4.0 * u) / view.scale,
                          Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
            p.drawPath(ring);
        }
        p.restore();
    }

    // Stickers: like Dense Grid, Info shows them on the selected case only.
    if (m_show_info && selected && IsGame(index)) {
        QFont f = font();
        f.setWeight(QFont::Black);
        f.setPixelSize(std::clamp(qRound(12.0 * u), 9, 16));
        const double tag_h = f.pixelSize() * 1.75;
        const QPointF top_right = front.at(1);
        const QPointF bottom_right = front.at(2);
        const double t = 0.17;
        // Right edges 10 concept px past the case. The tag font has a floor, so on small cases
        // the stickers would cover the art: shift them out until they cover at most 26 % of it.
        const QFontMetricsF tag_metrics(f);
        double widest = 0.0;
        if (info.is_game && UISettings::values.show_play_time.GetValue()) {
            widest = tag_metrics.horizontalAdvance(info.play_time_text);
        }
        if (UISettings::values.show_add_ons.GetValue() && !info.update_version.isEmpty()) {
            widest = std::max(widest, tag_metrics.horizontalAdvance(QStringLiteral("v") +
                                                                     info.update_version));
        }
        widest += f.pixelSize() * 1.4;
        const double out = 10.0 * u + std::max(0.0, widest - 10.0 * u - w * 0.26);
        QPointF anchor = top_right + (bottom_right - top_right) * t + QPointF(out, 0);
        if (selected) {
            anchor += QPointF(6.0 * u, -22.0 * u);
        }
        if (info.is_game && UISettings::values.show_play_time.GetValue()) { // "New" if unplayed
            DrawSticker(&p, anchor, info.play_time_text, f, QColor(0xff, 0xd3, 0x6b));
            anchor.ry() += tag_h + 6.0 * u;
        }
        if (UISettings::values.show_add_ons.GetValue() && !info.update_version.isEmpty()) {
            DrawSticker(&p, anchor, QStringLiteral("v") + info.update_version, f,
                        QColor(0xff, 0xf3, 0xc9));
        }
    }
}

void BoxShelfStyle::DrawNameBubble(QPainter& p, const Metrics& m, const QRectF& base,
                                   const QString& title, bool selected) {
    if (title.isEmpty()) {
        return;
    }
    // Concept bubble, shrunk with small cases (down to 65 %) so names never outgrow them.
    const double bs = std::clamp(m.face_w / 160.0, 0.65, 1.0);
    QFont f = font();
    f.setPixelSize(std::max(9, static_cast<int>(std::lround(13.0 * bs))));
    f.setBold(true);
    const QFontMetrics fm(f);
    const double pad_x = 12.0 * bs;
    const double pad_y = 8.0 * bs;
    const double corner = 12.0 * bs;
    // Up to 190 px, or the case width + 20 px when every name shows, so neighbours never overlap.
    // Spines: only the selected one has a bubble, so it may pass its neighbours.
    const double max_w = m_spines ? std::min(190.0 * bs, width() - 12.0)
                                  : std::min(m_show_names ? m.case_w + 20.0 : 190.0 * bs,
                                             m.cell_w - 8.0);
    // Sized to the S4 lines of the name; a gliding name (Scroll Long Names) takes the full width.
    const int text_max = static_cast<int>(std::max(20.0, max_w - 2 * pad_x));
    const int title_lines = LibraryStyleContext::TitleLines(2);
    const QStringList lines =
        LibraryStyleContext::NameLines(title, f, text_max, title_lines, static_cast<int>(pad_x));
    int text_w = 0;
    for (const QString& line : lines) {
        text_w = std::max(text_w, fm.horizontalAdvance(line));
    }
    if (selected && title_lines == 1 && fm.horizontalAdvance(title) > text_max + pad_x) {
        text_w = text_max;
    }
    const double text_h = fm.lineSpacing() * std::max<qsizetype>(1, lines.size());
    const double bw = std::min(text_w, text_max) + 2 * pad_x;
    const double bh = text_h + 2 * pad_y;
    const double arrow = 7.0 * bs;
    // Bubble 22 px below the case (scaled), its arrow reaching up towards it.
    const double top = base.bottom() + 22.0 * bs - arrow;
    double left = base.center().x() - bw / 2;
    left = std::clamp(left, 6.0, std::max(6.0, width() - 6.0 - bw));
    const QRectF rect(left, top + arrow, bw, bh);

    p.save();
    p.setPen(Qt::NoPen);
    for (int i = 1; i <= 4; ++i) {
        p.setBrush(QColor(0, 0, 0, 18));
        p.drawRoundedRect(rect.adjusted(-i, -i + 5 * bs, i, i + 5 * bs), corner + i, corner + i);
    }
    const QColor bubble(255, 255, 255, 235);
    p.setBrush(bubble);
    p.drawRoundedRect(rect, corner, corner);
    const double cx =
        std::clamp(base.center().x(), rect.left() + corner + 2, rect.right() - corner - 2);
    p.drawPolygon(QPolygonF{{cx - arrow, rect.top() + 0.75},
                            {cx, rect.top() - arrow},
                            {cx + arrow, rect.top() + 0.75}});
    // Two lines at most, cut with "...", like the cartridge bubbles; the selected case's long
    // name glides with Scroll Long Names.
    LibraryStyleContext::PaintTitle(&p, rect.adjusted(pad_x, pad_y, -pad_x, -pad_y).toAlignedRect(),
                                    title, QColor(0x16, 0x14, 0x1c), f, Qt::AlignCenter,
                                    title_lines, selected ? this : nullptr,
                                    static_cast<int>(pad_x));
    p.restore();
}

namespace {

LibraryStyleView* Make(QWidget* parent) {
    return new BoxShelfStyle(parent);
}

[[maybe_unused]] const bool registered =
    LibraryStyles::Register({"box_shelf", QT_TRANSLATE_NOOP("LibraryStyles", "Box Shelf"), &Make});

} // namespace

} // namespace LibraryStyles
