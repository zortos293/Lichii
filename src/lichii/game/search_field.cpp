// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "game/game_list.h"
#include "game/search_field.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>

// TODO: Remove GameList dependence?
GameListSearchField::KeyReleaseEater::KeyReleaseEater(GameList* gamelist_, QObject* parent)
    : QObject(parent), gamelist{gamelist_} {}

// EventFilter in order to process systemkeys while editing the searchfield
bool GameListSearchField::KeyReleaseEater::eventFilter(QObject* obj, QEvent* event) {
    // If it isn't a KeyRelease event then continue with standard event processing
    if (event->type() != QEvent::KeyRelease)
        return QObject::eventFilter(obj, event);

    QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
    QString edit_filter_text = gamelist->search_field->edit_filter->text().toLower();

    // If the searchfield's text hasn't changed special function keys get checked
    // If no function key changes the searchfield's text the filter doesn't need to get reloaded
    if (edit_filter_text == edit_filter_text_old) {
        switch (keyEvent->key()) {
        // Escape: Resets the searchfield
        case Qt::Key_Escape: {
            if (edit_filter_text_old.isEmpty()) {
                return QObject::eventFilter(obj, event);
            } else {
                gamelist->search_field->edit_filter->clear();
                edit_filter_text.clear();
            }
            break;
        }
        // Return and Enter
        // If the enter key gets pressed first checks how many and which entry is visible
        // If there is only one result launch this game
        case Qt::Key_Return:
        case Qt::Key_Enter: {
            if (gamelist->search_field->visible == 1) {
                const QString file_path = gamelist->GetLastFilterResultItem();

                // To avoid loading error dialog loops while confirming them using enter
                // Also users usually want to run a different game after closing one
                gamelist->search_field->edit_filter->clear();
                edit_filter_text.clear();
                emit gamelist->GameChosen(file_path);
            } else {
                return QObject::eventFilter(obj, event);
            }
            break;
        }
        default:
            return QObject::eventFilter(obj, event);
        }
    }
    edit_filter_text_old = edit_filter_text;
    return QObject::eventFilter(obj, event);
}

void GameListSearchField::setFilterResult(int visible_, int total_) {
    visible = visible_;
    total = total_;

    label_filter_result->setText(tr("%1 of %n result(s)", "", total).arg(visible));
}

QString GameListSearchField::filterText() const {
    return edit_filter->text();
}

void GameListSearchField::clear() {
    edit_filter->clear();
}

void GameListSearchField::setFocus() {
    if (edit_filter->isVisible()) {
        edit_filter->setFocus();
    }
}

GameListSearchField::GameListSearchField(GameList* parent) : QWidget{parent} {
    auto* const key_release_eater = new KeyReleaseEater(parent, this);
    layout_filter = new QHBoxLayout;
    layout_filter->setContentsMargins(8, 8, 8, 8);
    label_filter = new QLabel;
    edit_filter = new QLineEdit;
    edit_filter->clear();
    edit_filter->installEventFilter(key_release_eater);
    edit_filter->setClearButtonEnabled(true);
    connect(edit_filter, &QLineEdit::textChanged, parent, &GameList::OnTextChanged);
    label_filter_result = new QLabel;
    button_filter_close = new QToolButton(this);
    button_filter_close->setText(QStringLiteral("X"));
    button_filter_close->setCursor(Qt::ArrowCursor);
    button_filter_close->setStyleSheet(
        QStringLiteral("QToolButton{ border: none; padding: 0px; color: "
                       "#000000; font-weight: bold; background: #F0F0F0; }"
                       "QToolButton:hover{ border: none; padding: 0px; color: "
                       "#EEEEEE; font-weight: bold; background: #E81123}"));
    connect(button_filter_close, &QToolButton::clicked, parent, &GameList::OnFilterCloseClicked);
    layout_filter->setSpacing(10);
    layout_filter->addWidget(label_filter);
    layout_filter->addWidget(edit_filter);
    layout_filter->addWidget(label_filter_result);
    layout_filter->addWidget(button_filter_close);
    setLayout(layout_filter);
    RetranslateUI();
}

void GameListSearchField::SetModernMode(bool modern) {
    setProperty("modernLibrary", modern);
    if (!modern) {
        layout_filter->setContentsMargins(8, 8, 8, 8);
        layout_filter->setSpacing(10);
        edit_filter->setMinimumHeight(0);
        label_filter->setObjectName(QString{});
        edit_filter->setObjectName(QString{});
        label_filter_result->setObjectName(QString{});
        button_filter_close->setObjectName(QString{});
        button_filter_close->setText(QStringLiteral("X"));
        button_filter_close->setCursor(Qt::ArrowCursor);
        button_filter_close->setAutoRaise(false);
        button_filter_close->setMinimumSize(0, 0);
        button_filter_close->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        button_filter_close->setToolTip(QString{});
        setObjectName(QString{});
        setStyleSheet(QString{});
        button_filter_close->setStyleSheet(
            QStringLiteral("QToolButton{ border: none; padding: 0px; color: "
                           "#000000; font-weight: bold; background: #F0F0F0; }"
                           "QToolButton:hover{ border: none; padding: 0px; color: "
                           "#EEEEEE; font-weight: bold; background: #E81123}"));
        return;
    }

    // Compact modern search bar: keep the whole field around 36px tall.
    layout_filter->setContentsMargins(8, 3, 8, 3);
    layout_filter->setSpacing(8);
    label_filter->setObjectName(QStringLiteral("gameListFilterLabel"));
    edit_filter->setMinimumHeight(26);
    edit_filter->setObjectName(QStringLiteral("gameListFilterEdit"));
    label_filter_result->setObjectName(QStringLiteral("gameListFilterResult"));
    button_filter_close->setObjectName(QStringLiteral("gameListFilterClose"));
    button_filter_close->setText(QStringLiteral("×"));
    button_filter_close->setCursor(Qt::PointingHandCursor);
    button_filter_close->setAutoRaise(true);
    button_filter_close->setFixedSize(26, 26);
    button_filter_close->setToolTip(tr("Close filter"));
    button_filter_close->setStyleSheet({});
    setObjectName(QStringLiteral("gameListSearchField"));
    setStyleSheet(QStringLiteral(
        "QLineEdit#gameListFilterEdit {"
        "  border: 1px solid palette(mid);"
        "  border-radius: 10px;"
        "  padding: 4px 10px;"
        "  selection-background-color: palette(highlight);"
        "}"
        "QLineEdit#gameListFilterEdit:focus {"
        "  border: 1px solid palette(highlight);"
        "}"
        "QToolButton#gameListFilterClose {"
        "  border: 1px solid transparent;"
        "  border-radius: 8px;"
        "  font-size: 16px;"
        "  font-weight: 600;"
        "  padding: 0;"
        "  color: palette(text);"
        "}"
        "QToolButton#gameListFilterClose:hover {"
        "  background-color: palette(alternate-base);"
        "  border-color: palette(mid);"
        "}"
        "QToolButton#gameListFilterClose:pressed {"
        "  background-color: palette(midlight);"
        "}"));
}

void GameListSearchField::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void GameListSearchField::RetranslateUI() {
    label_filter->setText(tr("Filter:"));
    edit_filter->setPlaceholderText(tr("Enter pattern to filter"));
}
