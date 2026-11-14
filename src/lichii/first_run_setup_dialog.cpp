// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstddef>
#include <string>

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QColor>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLinearGradient>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPointF>
#include <QPushButton>
#include <QRadioButton>
#include <QRectF>
#include <QSize>
#include <QSizePolicy>
#include <QString>
#include <QVBoxLayout>
#include <QVariant>

#include "lichii/configuration/configuration_shared.h"
#include "lichii/first_run_setup_dialog.h"

namespace {

struct ThemePalette {
    QColor window;
    QColor chrome;
    QColor surface;
    QColor raised;
    QColor border;
    QColor text;
    QColor muted;
    QColor accent;
};

QColor WithAlpha(QColor color, int alpha) {
    color.setAlpha(alpha);
    return color;
}

ThemePalette PaletteFor(UISettings::Theme theme) {
    switch (theme) {
    case UISettings::Theme::Default:
        return {QColor(QStringLiteral("#f5f5f7")), QColor(QStringLiteral("#ffffff")),
                QColor(QStringLiteral("#ffffff")), QColor(QStringLiteral("#ececf0")),
                QColor(QStringLiteral("#d4d4dc")), QColor(QStringLiteral("#202124")),
                QColor(QStringLiteral("#6b6b76")), QColor(QStringLiteral("#2f7fd1"))};
    case UISettings::Theme::DefaultColorful:
        return {QColor(QStringLiteral("#f5f5f7")), QColor(QStringLiteral("#ffffff")),
                QColor(QStringLiteral("#ffffff")), QColor(QStringLiteral("#ececf0")),
                QColor(QStringLiteral("#d4d4dc")), QColor(QStringLiteral("#202124")),
                QColor(QStringLiteral("#6b6b76")), QColor(QStringLiteral("#7a5af8"))};
    case UISettings::Theme::Dark:
        return {QColor(QStringLiteral("#31363b")), QColor(QStringLiteral("#232629")),
                QColor(QStringLiteral("#3b4045")), QColor(QStringLiteral("#454a4f")),
                QColor(QStringLiteral("#4b4f54")), QColor(QStringLiteral("#eff0f1")),
                QColor(QStringLiteral("#bdc3c7")), QColor(QStringLiteral("#3daee9"))};
    case UISettings::Theme::DarkColorful:
        return {QColor(QStringLiteral("#1b1d21")), QColor(QStringLiteral("#15171a")),
                QColor(QStringLiteral("#24272b")), QColor(QStringLiteral("#2c3035")),
                QColor(QStringLiteral("#3a3f45")), QColor(QStringLiteral("#ececef")),
                QColor(QStringLiteral("#9aa0a6")), QColor(QStringLiteral("#7a5af8"))};
    case UISettings::Theme::MidnightBlue:
        return {QColor(QStringLiteral("#1a2233")), QColor(QStringLiteral("#141b29")),
                QColor(QStringLiteral("#222c40")), QColor(QStringLiteral("#2a3550")),
                QColor(QStringLiteral("#3a4765")), QColor(QStringLiteral("#e6ebf5")),
                QColor(QStringLiteral("#9aa7c2")), QColor(QStringLiteral("#3daee9"))};
    case UISettings::Theme::MidnightBlueColorful:
        return {QColor(QStringLiteral("#161e2e")), QColor(QStringLiteral("#111827")),
                QColor(QStringLiteral("#1e293b")), QColor(QStringLiteral("#273449")),
                QColor(QStringLiteral("#35435c")), QColor(QStringLiteral("#e8eef8")),
                QColor(QStringLiteral("#94a3b8")), QColor(QStringLiteral("#7a5af8"))};
    case UISettings::Theme::LichiiDark:
    case UISettings::Theme::LichiiModern:
        return {QColor(QStringLiteral("#17181b")), QColor(QStringLiteral("#131416")),
                QColor(QStringLiteral("#202125")), QColor(QStringLiteral("#232429")),
                QColor(QStringLiteral("#34363c")), QColor(QStringLiteral("#ececef")),
                QColor(QStringLiteral("#9b9da4")), QColor(QStringLiteral("#f0386b"))};
    }
    return PaletteFor(UISettings::default_theme);
}

QString ThemeName(UISettings::Theme theme) {
    return QString::fromUtf8(UISettings::themes[static_cast<size_t>(theme)].first);
}

QIcon MakeThemeSwatch(const ThemePalette& palette) {
    QPixmap pixmap(48, 32);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF frame(1, 1, 46, 30);
    QPainterPath clip;
    clip.addRoundedRect(frame, 6, 6);
    painter.setClipPath(clip);

    painter.fillRect(frame, palette.window);
    painter.fillRect(QRectF(frame.left(), frame.top(), frame.width(), 9), palette.chrome);
    painter.fillRect(QRectF(frame.left(), frame.top() + 9, 12, frame.height() - 9),
                     palette.chrome);

    painter.setPen(Qt::NoPen);
    painter.setBrush(palette.accent);
    painter.drawEllipse(QPointF(6, 5.5), 1.8, 1.8);
    painter.setBrush(palette.raised);
    painter.drawRoundedRect(QRectF(18, 14, 12, 12), 3, 3);
    painter.setBrush(palette.accent);
    painter.drawRoundedRect(QRectF(32, 14, 10, 12), 3, 3);

    painter.setClipping(false);
    painter.setPen(QPen(palette.border, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(frame, 6, 6);
    return QIcon(pixmap);
}

QIcon MakeLibraryStyleSwatch(const ThemePalette& palette, bool modern) {
    QPixmap pixmap(64, 44);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF frame(1, 1, 62, 42);
    QPainterPath clip;
    clip.addRoundedRect(frame, 7, 7);
    painter.setClipPath(clip);

    painter.fillRect(frame, palette.window);
    painter.fillRect(QRectF(frame.left(), frame.top(), frame.width(), 10), palette.chrome);

    painter.setPen(Qt::NoPen);
    if (modern) {
        for (int i = 0; i < 3; ++i) {
            const QRectF card(frame.left() + 7 + i * 18, frame.top() + 18, 14, 20);
            painter.setBrush(i == 1 ? palette.accent : palette.raised);
            painter.drawRoundedRect(card, 3, 3);
            painter.setBrush(i == 1 ? palette.text : palette.muted);
            painter.drawRoundedRect(QRectF(card.left() + 2, card.bottom() - 5, 8, 2), 1, 1);
        }
    } else {
        for (int i = 0; i < 3; ++i) {
            const QRectF row(frame.left() + 7, frame.top() + 15 + i * 9, 50, 6);
            painter.setBrush(i == 1 ? palette.accent : palette.raised);
            painter.drawRoundedRect(row, 2, 2);
            painter.setBrush(palette.text);
            painter.drawRoundedRect(QRectF(row.left() + 2, row.center().y() - 1, 18, 2), 1, 1);
        }
    }

    painter.setClipping(false);
    painter.setPen(QPen(palette.border, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(frame, 7, 7);
    return QIcon(pixmap);
}

class SetupPreviewWidget final : public QWidget {
public:
    explicit SetupPreviewWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(360, 240);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void SetPreview(const ThemePalette& new_palette, const QString& new_theme_name,
                    UISettings::GameLibraryStyle new_style) {
        palette = new_palette;
        theme_name = new_theme_name;
        style = new_style;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);

        const QRectF frame = QRectF(rect()).adjusted(1, 1, -1, -1);
        if (frame.width() < 200 || frame.height() < 140) {
            return;
        }

        QPainterPath clip;
        clip.addRoundedRect(frame, 12, 12);
        painter.setClipPath(clip);
        painter.fillRect(rect(), palette.window);

        constexpr qreal chrome_height = 34;
        painter.fillRect(QRectF(frame.left(), frame.top(), frame.width(), chrome_height),
                         palette.chrome);

        painter.setPen(Qt::NoPen);
        painter.setBrush(palette.muted);
        painter.drawEllipse(QPointF(frame.left() + 14, frame.top() + 17), 3, 3);
        painter.setBrush(palette.border);
        painter.drawEllipse(QPointF(frame.left() + 26, frame.top() + 17), 3, 3);
        painter.setBrush(palette.accent);
        painter.drawEllipse(QPointF(frame.left() + 38, frame.top() + 17), 3, 3);

        QFont chrome_font = font();
        chrome_font.setPointSizeF(9.0);
        chrome_font.setBold(true);
        painter.setFont(chrome_font);
        painter.setPen(palette.muted);
        painter.drawText(QRectF(frame.left() + 80, frame.top(), frame.width() - 160,
                                chrome_height),
                         Qt::AlignCenter, theme_name);

        constexpr qreal sidebar_width = 104;
        painter.fillRect(QRectF(frame.left(), frame.top() + chrome_height, sidebar_width,
                                frame.height() - chrome_height),
                         palette.chrome);
        painter.setPen(QPen(palette.border, 1));
        painter.drawLine(QPointF(frame.left() + sidebar_width, frame.top() + chrome_height),
                         QPointF(frame.left() + sidebar_width, frame.bottom()));

        for (int i = 0; i < 4; ++i) {
            const QRectF row(frame.left() + 12, frame.top() + chrome_height + 16 + i * 32,
                             sidebar_width - 24, 24);
            if (i == 1) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(WithAlpha(palette.accent, 38));
                painter.drawRoundedRect(row, 6, 6);
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(i == 1 ? palette.accent : WithAlpha(palette.muted, 150));
            painter.drawRoundedRect(QRectF(row.left() + 8, row.center().y() - 5, 10, 10), 3, 3);
            painter.setBrush(i == 1 ? palette.text : palette.muted);
            painter.drawRoundedRect(QRectF(row.left() + 26, row.center().y() - 3, 42, 6), 3, 3);
        }

        const QRectF content(frame.left() + sidebar_width + 1, frame.top() + chrome_height,
                             frame.width() - sidebar_width - 1,
                             frame.height() - chrome_height);
        const QRectF inner = content.adjusted(20, 18, -20, -18);

        QFont heading_font = font();
        heading_font.setPointSizeF(10.5);
        heading_font.setBold(true);
        painter.setFont(heading_font);
        painter.setPen(palette.text);
        painter.drawText(QRectF(inner.left(), inner.top(), inner.width(), 22),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QCoreApplication::translate("FirstRunSetupDialog", "Library"));

        const QRectF search(inner.left(), inner.top() + 32, inner.width(), 26);
        painter.setPen(QPen(palette.border, 1));
        painter.setBrush(palette.surface);
        painter.drawRoundedRect(search, 7, 7);
        painter.setPen(QPen(palette.muted, 1.4));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QPointF(search.left() + 14, search.center().y()), 4, 4);
        painter.drawLine(QPointF(search.left() + 17, search.center().y() + 3),
                         QPointF(search.left() + 20, search.center().y() + 6));
        QFont search_font = font();
        search_font.setPointSizeF(8.5);
        painter.setFont(search_font);
        painter.setPen(palette.muted);
        painter.drawText(search.adjusted(28, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter,
                         QCoreApplication::translate("FirstRunSetupDialog", "Search"));

        const qreal top = search.bottom() + 14;
        if (style == UISettings::GameLibraryStyle::Modern) {
            constexpr int columns = 3;
            constexpr qreal gap = 12;
            constexpr qreal card_height = 92;
            const qreal card_width = (inner.width() - gap * (columns - 1)) / columns;

            for (int i = 0; i < 6; ++i) {
                const int row = i / columns;
                const int column = i % columns;
                const QRectF card(inner.left() + column * (card_width + gap),
                                  top + row * (card_height + gap), card_width, card_height);
                if (card.bottom() > inner.bottom()) {
                    break;
                }

                const bool selected = i == 1;
                painter.setPen(QPen(selected ? palette.accent : palette.border, selected ? 2 : 1));
                painter.setBrush(selected ? WithAlpha(palette.accent, 28) : palette.raised);
                painter.drawRoundedRect(card, 9, 9);

                const QRectF cover = card.adjusted(6, 6, -6, -30);
                QLinearGradient gradient(cover.topLeft(), cover.bottomRight());
                gradient.setColorAt(0, WithAlpha(palette.accent.lighter(112), 220));
                gradient.setColorAt(1, WithAlpha(palette.accent.darker(155), 220));
                painter.setPen(Qt::NoPen);
                painter.setBrush(gradient);
                painter.drawRoundedRect(cover, 6, 6);

                painter.setBrush(palette.text);
                painter.drawRoundedRect(
                    QRectF(card.left() + 8, card.bottom() - 20, card.width() * 0.62, 5), 2.5,
                    2.5);
                painter.setBrush(palette.muted);
                painter.drawRoundedRect(
                    QRectF(card.left() + 8, card.bottom() - 11, card.width() * 0.38, 4), 2, 2);
            }
        } else {
            constexpr qreal row_height = 42;
            constexpr qreal gap = 6;

            for (int i = 0; i < 6; ++i) {
                const QRectF row(inner.left(), top + i * (row_height + gap), inner.width(),
                                 row_height);
                if (row.bottom() > inner.bottom()) {
                    break;
                }

                const bool selected = i == 2;
                painter.setPen(QPen(selected ? palette.accent : palette.border, selected ? 2 : 1));
                painter.setBrush(selected ? WithAlpha(palette.accent, 28)
                                          : (i % 2 == 0 ? palette.surface : palette.raised));
                painter.drawRoundedRect(row, 8, 8);

                painter.setPen(Qt::NoPen);
                painter.setBrush(palette.accent);
                painter.drawRoundedRect(QRectF(row.left() + 8, row.center().y() - 13, 26, 26), 6,
                                        6);
                painter.setBrush(palette.text);
                painter.drawRoundedRect(
                    QRectF(row.left() + 46, row.center().y() - 9, row.width() * 0.36, 6), 3, 3);
                painter.setBrush(palette.muted);
                painter.drawRoundedRect(
                    QRectF(row.left() + 46, row.center().y() + 1, row.width() * 0.24, 5), 2.5,
                    2.5);
            }
        }

        painter.setClipping(false);
        painter.setPen(QPen(palette.border, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(frame, 12, 12);
    }

private:
    ThemePalette palette = PaletteFor(UISettings::default_theme);
    QString theme_name;
    UISettings::GameLibraryStyle style{UISettings::GameLibraryStyle::Modern};
};

constexpr std::array<UISettings::Theme, 8> theme_order{{
    UISettings::Theme::LichiiModern,
    UISettings::Theme::LichiiDark,
    UISettings::Theme::Default,
    UISettings::Theme::Dark,
    UISettings::Theme::DefaultColorful,
    UISettings::Theme::DarkColorful,
    UISettings::Theme::MidnightBlue,
    UISettings::Theme::MidnightBlueColorful,
}};

QLabel* SectionLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    QFont font = label->font();
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() + 1.0);
    label->setFont(font);
    return label;
}

QLabel* MutedLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color: palette(mid);"));
    return label;
}

} // namespace

FirstRunSetupDialog::FirstRunSetupDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("FirstRunSetupDialog"));
    setWindowTitle(tr("Set up Lichii"));
    setMinimumSize(640, 460);

    auto current_theme = [] {
        const std::string& theme = UISettings::values.theme;
        for (const auto candidate : theme_order) {
            if (theme == UISettings::themes[static_cast<size_t>(candidate)].second) {
                return candidate;
            }
        }
        return UISettings::default_theme;
    }();
    if (UISettings::values.first_start.GetValue()) {
        current_theme = UISettings::Theme::LichiiModern;
    }

    const u8 style_value = UISettings::values.game_library_style.GetValue();
    selected_style = style_value == static_cast<u8>(UISettings::GameLibraryStyle::Classic)
                         ? UISettings::GameLibraryStyle::Classic
                         : UISettings::GameLibraryStyle::Modern;

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 18);
    root->setSpacing(16);

    auto* header = new QHBoxLayout;
    header->setSpacing(14);
    auto* logo = new QLabel(this);
    logo->setPixmap(
        QIcon(QStringLiteral(":/icons/default/256x256/lichii.png")).pixmap(44));
    header->addWidget(logo, 0, Qt::AlignTop);

    auto* titles = new QVBoxLayout;
    titles->setSpacing(3);
    auto* title = new QLabel(tr("Make Lichii yours"), this);
    QFont title_font = title->font();
    title_font.setBold(true);
    title_font.setPointSizeF(title_font.pointSizeF() * 1.35);
    title->setFont(title_font);
    auto* subtitle =
        new QLabel(tr("Pick a theme and a library layout. They are independent, so you can mix "
                      "them, and you can change both later."),
                   this);
    subtitle->setWordWrap(true);
    titles->addWidget(title);
    titles->addWidget(subtitle);
    header->addLayout(titles, 1);
    root->addLayout(header);

    auto* content = new QHBoxLayout;
    content->setSpacing(24);

    auto* left = new QVBoxLayout;
    left->setSpacing(8);
    left->addWidget(SectionLabel(tr("Theme"), this));
    theme_list = new QListWidget(this);
    theme_list->setFixedWidth(250);
    theme_list->setIconSize(QSize(48, 32));
    theme_list->setSpacing(3);
    theme_list->setUniformItemSizes(true);
    theme_list->setWordWrap(true);
    theme_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    theme_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    left->addWidget(theme_list, 1);
    left->addWidget(MutedLabel(tr("Each theme brings its own colors and icons."), this));
    content->addLayout(left);

    auto* right = new QVBoxLayout;
    right->setSpacing(12);
    auto* preview_header = new QHBoxLayout;
    preview_header->addWidget(SectionLabel(tr("Preview"), this));
    preview_header->addStretch(1);
    preview_header->addWidget(MutedLabel(tr("Theme and library style together"), this));
    right->addLayout(preview_header);

    preview = new SetupPreviewWidget(this);
    right->addWidget(preview, 1);

    auto* style_section = new QVBoxLayout;
    style_section->setSpacing(8);
    style_section->addWidget(SectionLabel(tr("Library style"), this));
    auto* style_row = new QHBoxLayout;
    style_row->setSpacing(18);

    auto* modern_column = new QVBoxLayout;
    modern_column->setSpacing(4);
    modern_style = new QRadioButton(tr("Modern library"), this);
    modern_style->setIconSize(QSize(64, 44));
    modern_column->addWidget(modern_style);
    auto* modern_description = MutedLabel(tr("Folder cards with cover art."), this);
    modern_description->setContentsMargins(26, 0, 0, 0);
    modern_column->addWidget(modern_description);
    style_row->addLayout(modern_column, 1);

    auto* classic_column = new QVBoxLayout;
    classic_column->setSpacing(4);
    classic_style = new QRadioButton(tr("Classic library"), this);
    classic_style->setIconSize(QSize(64, 44));
    classic_column->addWidget(classic_style);
    auto* classic_description = MutedLabel(tr("The familiar expandable game list."), this);
    classic_description->setContentsMargins(26, 0, 0, 0);
    classic_column->addWidget(classic_description);
    style_row->addLayout(classic_column, 1);

    style_section->addLayout(style_row);
    style_section->addWidget(MutedLabel(tr("The library style works with every theme."), this));
    right->addLayout(style_section);
    content->addLayout(right, 1);
    root->addLayout(content, 1);

    root->addWidget(MutedLabel(
        tr("You can change both later under Emulation > Configure > General > UI."), this));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
    auto* continue_button = buttons->button(QDialogButtonBox::Ok);
    continue_button->setText(tr("Continue"));
    continue_button->setDefault(true);
    continue_button->setAutoDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    root->addWidget(buttons);

    auto* style_group = new QButtonGroup(this);
    style_group->addButton(modern_style);
    style_group->addButton(classic_style);
    modern_style->setChecked(selected_style == UISettings::GameLibraryStyle::Modern);
    classic_style->setChecked(selected_style == UISettings::GameLibraryStyle::Classic);
    connect(modern_style, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked) {
            selected_style = UISettings::GameLibraryStyle::Modern;
            UpdatePreview();
        }
    });
    connect(classic_style, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked) {
            selected_style = UISettings::GameLibraryStyle::Classic;
            UpdatePreview();
        }
    });

    BuildThemeList();
    connect(theme_list, &QListWidget::currentRowChanged, this,
            [this](int) { UpdatePreview(); });

    int initial_row = 0;
    for (int row = 0; row < theme_list->count(); ++row) {
        if (static_cast<UISettings::Theme>(theme_list->item(row)->data(Qt::UserRole).toInt()) ==
            current_theme) {
            initial_row = row;
            break;
        }
    }
    theme_list->setCurrentRow(initial_row);
    UpdatePreview();
    theme_list->setFocus();

    ConfigurationShared::FitToScreen(this, QSize(940, 660));
}

FirstRunSetupDialog::~FirstRunSetupDialog() = default;

UISettings::Theme FirstRunSetupDialog::selected_theme() const {
    if (theme_list == nullptr || theme_list->currentItem() == nullptr) {
        return UISettings::default_theme;
    }
    return static_cast<UISettings::Theme>(theme_list->currentItem()->data(Qt::UserRole).toInt());
}

UISettings::GameLibraryStyle FirstRunSetupDialog::selected_library_style() const {
    return selected_style;
}

void FirstRunSetupDialog::BuildThemeList() {
    for (const auto theme : theme_order) {
        const ThemePalette palette = PaletteFor(theme);
        QString label = ThemeName(theme);
        if (theme == UISettings::Theme::LichiiModern) {
            label += tr(" (Recommended)");
        }

        auto* item = new QListWidgetItem(MakeThemeSwatch(palette), label, theme_list);
        item->setData(Qt::UserRole, static_cast<int>(theme));
        item->setSizeHint(QSize(220, 46));
        if (theme == UISettings::Theme::LichiiModern) {
            item->setToolTip(
                tr("Lichii colors with the modern library and loading screens."));
        } else if (theme == UISettings::Theme::LichiiDark) {
            item->setToolTip(tr("Lichii colors with the classic loading screens."));
        } else {
            item->setToolTip(tr("A built-in theme."));
        }
    }
}

void FirstRunSetupDialog::UpdatePreview() {
    const ThemePalette palette = PaletteFor(selected_theme());
    if (auto* setup_preview = static_cast<SetupPreviewWidget*>(preview); setup_preview != nullptr) {
        setup_preview->SetPreview(palette, ThemeName(selected_theme()), selected_style);
    }
    if (modern_style != nullptr) {
        modern_style->setIcon(MakeLibraryStyleSwatch(palette, true));
    }
    if (classic_style != nullptr) {
        classic_style->setIcon(MakeLibraryStyleSwatch(palette, false));
    }
}
