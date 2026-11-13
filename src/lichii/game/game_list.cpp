// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2015 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <utility>

#include <QAbstractItemView>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QListView>
#include <QMenu>
#include <QScrollBar>
#include <QScroller>
#include <QScrollerProperties>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>
#include <qlayoutitem.h>

#include "common/common_types.h"
#include "common/fs/fs_util.h"
#include "core/core.h"
#include "core/file_sys/nup_writer.h"
#ifdef LICHII_NX_TOOLS
#include "lichii/nx_tools.h"
#endif
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/game_list_p.h"
#include "qt_common/game_list/model.h"
#include "qt_common/qt_common.h"
#include "qt_common/util/game.h"
#include "lichii/game/carousel.h"
#include "lichii/game/common.h"
#include "lichii/game/game_grid.h"
#include "lichii/game/game_list.h"
#include "lichii/game/game_tree.h"
#include "lichii/game/search_field.h"
#include "lichii/game/styles/library_style_filter.h"
#include "lichii/game/styles/library_style_registry.h"
#include "lichii/game/styles/library_style_view.h"
#include "lichii/main_window.h"
#include "lichii/shader_warm_dialog.h"
#include "lichii/util/controller_navigation.h"
#include "lichii/util/util.h"

namespace {

bool IsGameDirectoryType(GameListItemType type) {
    return type == GameListItemType::CustomDir || type == GameListItemType::SdmcDir ||
           type == GameListItemType::UserNandDir || type == GameListItemType::SysNandDir;
}

} // namespace

GameList::GameList(FileSys::VirtualFilesystem vfs_, FileSys::ManualContentProvider* provider_,
                   PlayTime::PlayTimeManager& play_time_manager_, Core::System& system_,
                   MainWindow* parent)
    : QWidget{parent}, vfs{std::move(vfs_)}, provider{provider_},
      play_time_manager{play_time_manager_}, system{system_} {

    this->main_window = parent;
    layout = new QVBoxLayout;
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    setLayout(layout);

    item_model = new GameListModel(vfs, provider, play_time_manager, system, this);
    style_model = new LibraryStyles::FilterModel(this);
    style_model->setSourceModel(item_model);

    SetupViews();

    folder_navigation = new QWidget(this);
    folder_navigation->setObjectName(QStringLiteral("gameFolderNavigation"));
    folder_navigation->setProperty("modernLibrary", true);
    auto* folder_layout = new QHBoxLayout(folder_navigation);
    folder_layout->setContentsMargins(8, 4, 8, 4);
    folder_layout->setSpacing(10);
    folder_back_button = new QPushButton(tr("All folders"), folder_navigation);
    folder_back_button->setObjectName(QStringLiteral("gameFolderBackButton"));
    folder_back_button->setFixedHeight(30);
    folder_back_button->setCursor(Qt::PointingHandCursor);
    folder_title_label = new QLabel(folder_navigation);
    folder_title_label->setObjectName(QStringLiteral("gameFolderTitle"));
    folder_layout->addWidget(folder_back_button);
    folder_layout->addWidget(folder_title_label, 1);
    folder_navigation->setLayout(folder_layout);
    folder_navigation->hide();
    layout->addWidget(folder_navigation);

    search_field = new GameListSearchField(this);
    layout->addWidget(search_field);
    connect(folder_back_button, &QPushButton::clicked, this,
            &GameList::ReturnToGameDirectories);

    controller_navigation = new ControllerNavigation(system.HIDCore(), this);
    controller_navigation->SetLibraryButtonsEnabled(true);
    controller_navigation->SetDirectionRepeatEnabled(true);

    SetupScrollAnimation();

    connect(tree_view, &QTreeView::activated, this, &GameList::ValidateEntry);
    connect(tree_view, &QTreeView::customContextMenuRequested, this, &GameList::PopupContextMenu);

    connect(grid_view, &QListView::doubleClicked, this, [this](const QModelIndex& index) {
        if (suppress_grid_double_click) {
            return;
        }
        ValidateEntry(index);
    });
    connect(grid_view, &QListView::clicked, this, [this](const QModelIndex& index) {
        if (suppress_grid_double_click) {
            return;
        }
        const auto type = index.data(GameListItem::TypeRole).value<GameListItemType>();
        if (IsGameDirectoryType(type)) {
            suppress_grid_double_click = true;
            QTimer::singleShot(QApplication::doubleClickInterval(), this,
                               [this] { suppress_grid_double_click = false; });
            ValidateEntry(index);
        }
    });
    connect(grid_view, &QListView::customContextMenuRequested, this, &GameList::PopupContextMenu);

    connect(carousel_view, &QListView::activated, this, &GameList::ValidateEntry);
    connect(carousel_view, &QListView::customContextMenuRequested, this, &GameList::PopupContextMenu);
    connect(tree_view->header(), &QHeaderView::sortIndicatorChanged, carousel_view,
            &GameCarousel::SetSort);

    connect(controller_navigation, &ControllerNavigation::TriggerKeyboardEvent, this,
            [this](Qt::Key key) { OnControllerKey(key, false); });
    connect(controller_navigation, &ControllerNavigation::RepeatKeyboardEvent, this,
            [this](Qt::Key key) { OnControllerKey(key, true); });

    connect(item_model, &GameListModel::PopulatingCompleted, this,
            &GameList::OnPopulatingCompleted);

    connect(item_model, &GameListModel::ShowList, this, &GameList::ShowList);
    connect(item_model, &GameListModel::SaveConfig, this, &GameList::SaveConfig);
    connect(item_model, &GameListModel::PopulatingStarted, this, &GameList::OnPopulate);

    // TODO: impl on grid/carousel
    connect(tree_view, &GameTree::FilterResultReady, search_field,
            [this](int visible, int total) { search_field->setFilterResult(visible, total); });

    qRegisterMetaType<QList<QStandardItem*>>("QList<QStandardItem*>");

    ResetViewMode();
}

void GameList::SetNupConversionRunning(bool running) {
    nup_conversion_running = running;
}

void GameList::SetNxToolRunning(bool running) {
    nx_tool_running = running;
}

GameList::~GameList() {
    UnloadController();
}

void GameList::SetupViews() {
    tree_view = new GameTree(this);
    grid_view = new GameGrid(this);
    carousel_view = new GameCarousel(this);

    tree_view->SetModel(item_model);
    grid_view->SetModel(item_model);
    carousel_view->SetModel(item_model);
}

QString GameList::GetLastFilterResultItem() const {
    if (game_list_mode == Settings::GameListMode::GridView) {
        const QModelIndex root = grid_view->rootIndex();
        if (root.isValid()) {
            for (int row = 0; row < item_model->rowCount(root); ++row) {
                const QModelIndex game = item_model->index(row, 0, root);
                if (!grid_view->isRowHidden(row) &&
                    game.data(GameListItem::TypeRole).value<GameListItemType>() ==
                        GameListItemType::Game) {
                    return game.data(GameListItemPath::FullPathRole).toString();
                }
            }
        } else {
            const QString filter = search_field->filterText().toLower();
            for (int row = 1; row < item_model->rowCount() - 1; ++row) {
                const auto* directory = item_model->item(row, 0);
                if (grid_view->isRowHidden(row)) {
                    continue;
                }
                for (int game_row = 0; game_row < directory->rowCount(); ++game_row) {
                    const auto* game = directory->child(game_row, 0);
                    if (Lichii::FilterMatches(filter, game)) {
                        return game->data(GameListItemPath::FullPathRole).toString();
                    }
                }
            }
        }
    }
    return tree_view->GetLastFilterResultItem();
}

void GameList::ClearFilter() {
    search_field->clear();
}

void GameList::SetFilterFocus() {
    if (item_model->rowCount() > 0) {
        search_field->setFocus();
    }
}

void GameList::SetFilterVisible(bool visibility) {
    search_field->setVisible(visibility);
}

bool GameList::IsEmpty() const {
    return item_model->IsEmpty();
}

void GameList::OnPopulate() {
    if (game_list_mode == Settings::GameListMode::GridView && grid_view->rootIndex().isValid()) {
        grid_root_dir_index_to_restore =
            grid_view->rootIndex().data(GameListDir::GameDirRole).toInt();
    } else {
        grid_root_dir_index_to_restore = -1;
    }
    // removeRows() below runs while the grid's root is the opened folder and its current index is
    // that folder's row; Qt's single selection then walks parents looking for the root and never
    // stops. Reset both first; OnPopulatingCompleted() opens the folder again.
    grid_view->setCurrentIndex(QModelIndex{});
    grid_view->setRootIndex(QModelIndex{});
    m_populating = true;
    m_currentView->setEnabled(false);
    if (style_view != nullptr) {
        style_view->setEnabled(false);
    }

    switch (game_list_mode) {
    case Settings::GameListMode::TreeView:
        tree_view->UpdateColumnVisibility(item_model);
        break;
    case Settings::GameListMode::GridView:
        grid_view->UpdateIconSize();
        break;
    case Settings::GameListMode::CarouselView:
        carousel_view->UpdateIconSize();
        break;
    }
}

void GameList::PopulateAsync(QVector<UISettings::GameDir>& game_dirs) {
    item_model->PopulateAsync(game_dirs);
}

void GameList::SetScanEnabled(bool enabled) {
    item_model->SetScanEnabled(enabled);
}

void GameList::SaveInterfaceLayout() {
    tree_view->SaveInterfaceLayout();
}

void GameList::LoadInterfaceLayout() {
    tree_view->LoadInterfaceLayout();
}

QStandardItemModel* GameList::GetModel() const {
    return item_model;
}

void GameList::UnloadController() {
    controller_navigation->UnloadController();
}

void GameList::ResetViewMode() {
    const auto mode = UISettings::values.game_list_mode.GetValue();
    game_list_mode = mode;

    if (m_currentView)
        layout->removeWidget(m_currentView);

    const bool newTreeMode = mode == Settings::GameListMode::TreeView;

    switch (mode) {
    case Settings::GameListMode::TreeView:
        m_currentView = tree_view;
        break;
    case Settings::GameListMode::GridView:
        m_currentView = grid_view;
        break;
    case Settings::GameListMode::CarouselView:
        m_currentView = carousel_view;
        break;
    default:
        UNREACHABLE();
    }

    tree_view->setVisible(false);
    grid_view->setVisible(false);
    carousel_view->setVisible(false);

    tree_view->setEnabled(false);
    grid_view->setEnabled(false);
    carousel_view->setEnabled(false);

    m_currentView->setVisible(true);
    // MainWindow switches the view right after starting the first populate; keep it disabled
    // until that finishes, as OnPopulate does for every other populate.
    m_currentView->setEnabled(!m_populating);
    search_field->SetModernMode(mode == Settings::GameListMode::GridView);
    layout->insertWidget(mode == Settings::GameListMode::GridView ? 2 : 0, m_currentView);

    m_currentView->installEventFilter(this);
    auto view = m_currentView->viewport();
    view->installEventFilter(this);

    view->grabGesture(Qt::SwipeGesture);
    view->grabGesture(Qt::PanGesture);

    QScroller::grabGesture(view, QScroller::LeftMouseButtonGesture);

    auto scroller = QScroller::scroller(view);
    QScrollerProperties props;
    props.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy,
                          QScrollerProperties::OvershootAlwaysOff);
    props.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy,
                          QScrollerProperties::OvershootAlwaysOff);
    scroller->setScrollerProperties(props);

    // Every view reads the loaded library as it is (the Carousel through its own flat row), so
    // switching views never scans the library again.
    m_isTreeMode = newTreeMode;

    if (mode == Settings::GameListMode::GridView) {
        grid_view->setRootIndex(QModelIndex{});
        if (!m_populating && item_model->rowCount() > 0) {
            grid_view->setRowHidden(0, true);
        }
        UpdateFolderNavigation();
    } else {
        folder_navigation->hide();
        if (mode == Settings::GameListMode::TreeView && !m_populating &&
            item_model->rowCount() > 0) {
            tree_view->setRowHidden(0, item_model->invisibleRootItem()->index(),
                                    UISettings::values.favorited_ids.isEmpty() ||
                                        !search_field->filterText().isEmpty());
        }
    }

    // Refresh the placeholder after a View menu mode change: with an empty library only the
    // grid view shows the empty state, the other views rely on the placeholder widget. Harmless
    // during construction, before anything is connected to ShowList. During a populate the
    // callback re-emits the correct state, so only surface an update when idle.
    if (!m_populating) {
        emit ShowList(game_list_mode == Settings::GameListMode::GridView ||
                      !item_model->IsEmpty());
    }

    // A library style replaces the Modern grid: the grid keeps its state underneath, hidden, so
    // going back to it needs nothing else.
    ActivateStyle(LibraryStyles::Active());
}

void GameList::UpdateStyleToggles() {
    if (style_view == nullptr) {
        return;
    }
    style_view->SetShowNames(UISettings::values.show_game_name.GetValue());
    style_view->SetShowInfo(Lichii::ShowAnyGameInfo());
}

void GameList::ActivateStyle(const LibraryStyles::StyleInfo* info) {
    if (style_view != nullptr) {
        layout->removeWidget(style_view);
    }
    if (info == nullptr || (style_view != nullptr && style_id != info->id)) {
        if (style_view != nullptr) {
            // The old view lives until the event loop deletes it: cut it off now so a rebuild it
            // still has queued neither reports a selection nor works on the model.
            style_view->disconnect(this);
            style_model->disconnect(style_view);
            style_view->hide();
            style_view->deleteLater();
            style_view = nullptr;
        }
        style_id.clear();
    }
    if (info == nullptr) {
        return;
    }

    if (style_view == nullptr) {
        if (style_selected_path.isEmpty() && grid_view->currentIndex().isValid()) {
            style_selected_path =
                grid_view->currentIndex().data(GameListItemPath::FullPathRole).toString();
        }
        style_view = info->factory(this);
        if (style_view == nullptr) {
            return;
        }
        style_id = info->id;
        style_view->setObjectName(QStringLiteral("libraryStyleView"));
        style_view->setProperty("modernLibrary", true);
        style_view->setProperty("libraryStyle", QString::fromUtf8(info->id));
        style_view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        style_view->SetModel(style_model);
        // A view may pick its first entry in SetModel without emitting CurrentChanged.
        if (style_selected_path.isEmpty()) {
            style_selected_path =
                style_view->CurrentIndex().data(GameListItemPath::FullPathRole).toString();
        }

        connect(style_view, &LibraryStyles::LibraryStyleView::LaunchRequested, this,
                &GameList::OnStyleLaunch);
        connect(style_view, &LibraryStyles::LibraryStyleView::ContextMenuRequested, this,
                &GameList::OnStyleContextMenu);
        connect(style_view, &LibraryStyles::LibraryStyleView::CurrentChanged, this,
                [this](const QModelIndex& index) {
                    // While populating, views fall back to their first entry: keep the choice.
                    if (m_populating) {
                        return;
                    }
                    if (index.data(GameListItem::TypeRole).value<GameListItemType>() ==
                        GameListItemType::Game) {
                        style_selected_path =
                            index.data(GameListItemPath::FullPathRole).toString();
                    }
                });

        // The shared keys are read before the view sees them, on the view and on every widget
        // inside it (children added later get the filter through ChildAdded).
        style_view->installEventFilter(this);
        for (QWidget* child : style_view->findChildren<QWidget*>()) {
            child->installEventFilter(this);
        }
    }

    grid_view->setVisible(false);
    grid_view->setEnabled(false);
    folder_navigation->hide();
    layout->insertWidget(layout->indexOf(m_currentView) + 1, style_view);
    style_view->setVisible(true);
    style_view->setEnabled(!m_populating);
    UpdateStyleToggles();
    RestoreStyleSelection();
    // The grid that had focus is disabled now; without this, keys go nowhere until a click.
    FocusStyleView();
}

void GameList::FocusStyleView() {
    if (style_view == nullptr || !style_view->isEnabled() || !style_view->isVisible()) {
        return;
    }
    const QWidget* focus = QApplication::focusWidget();
    if (focus != nullptr && search_field->isAncestorOf(focus)) {
        return; // Typing a search; don't steal the cursor.
    }
    style_view->setFocus(Qt::OtherFocusReason);
}

bool GameList::IsStyleObject(const QObject* object) const {
    if (style_view == nullptr || object == nullptr) {
        return false;
    }
    if (object == style_view) {
        return true;
    }
    return object->isWidgetType() &&
           style_view->isAncestorOf(static_cast<const QWidget*>(object));
}

void GameList::OnControllerKey(Qt::Key key, bool auto_repeat) {
    if (system.IsPoweredOn()) {
        return;
    }
    if (!this->isActiveWindow()) {
        return;
    }
    if (style_view != nullptr && style_view->isVisible()) {
        // B toggles the names in the styles; there is no folder level to leave.
        const Qt::Key style_key = key == Qt::Key_Escape ? Qt::Key_B : key;
        QWidget* target = style_view->focusWidget();
        if (target == nullptr || !IsStyleObject(target)) {
            target = style_view;
        }
        QCoreApplication::postEvent(target, new QKeyEvent(QEvent::KeyPress, style_key,
                                                          Qt::NoModifier, QString(), auto_repeat));
        return;
    }
    if (key == Qt::Key_BracketLeft || key == Qt::Key_BracketRight) {
        // L / R switch the view here too, so they cycle through every view.
        emit CycleStyleRequested(key == Qt::Key_BracketLeft ? -1 : 1);
        return;
    }
    if (key == Qt::Key_X || key == Qt::Key_Y) {
        // Library style buttons only.
        return;
    }
    QKeyEvent* event =
        new QKeyEvent(QEvent::KeyPress, key, Qt::NoModifier, QString(), auto_repeat);
    QCoreApplication::postEvent(m_currentView, event);
}

bool GameList::HandleStyleKey(QObject* target, const QKeyEvent* event) {
    const int key = event->key();
    const Qt::KeyboardModifiers modifiers =
        event->modifiers() & ~Qt::KeyboardModifiers(Qt::KeypadModifier);
    if (modifiers == Qt::ControlModifier) {
        if (key == Qt::Key_BracketLeft || key == Qt::Key_BracketRight) {
            emit CycleStyleRequested(key == Qt::Key_BracketLeft ? -1 : 1);
            return true;
        }
        return false;
    }
    if (modifiers != Qt::NoModifier) {
        return false;
    }

    // Handled right away, as the arrow keys are, and still marked as a repeat while held:
    // the views step held keys differently.
    const auto send_arrow = [target, event](Qt::Key arrow) {
        QKeyEvent arrow_event(QEvent::KeyPress, arrow, Qt::NoModifier, QString(),
                              event->isAutoRepeat());
        QCoreApplication::sendEvent(target, &arrow_event);
    };
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        OnStyleLaunch(style_view->CurrentIndex());
        return true;
    case Qt::Key_W:
        send_arrow(Qt::Key_Up);
        return true;
    case Qt::Key_A:
        send_arrow(Qt::Key_Left);
        return true;
    case Qt::Key_S:
        send_arrow(Qt::Key_Down);
        return true;
    case Qt::Key_D:
        send_arrow(Qt::Key_Right);
        return true;
    case Qt::Key_I:
    case Qt::Key_Y:
        emit ToggleInfoRequested();
        return true;
    case Qt::Key_B:
        emit ToggleNamesRequested();
        return true;
    case Qt::Key_R:
    case Qt::Key_X: {
        const QString label = style_view->CycleOption();
        if (!label.isEmpty()) {
            emit StyleOptionChanged(label);
        }
        return true;
    }
    case Qt::Key_BracketLeft:
        emit CycleStyleRequested(-1);
        return true;
    case Qt::Key_BracketRight:
        emit CycleStyleRequested(1);
        return true;
    default:
        return false;
    }
}

void GameList::OnStyleLaunch(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }
    QModelIndex source = index.model() == style_model ? style_model->mapToSource(index) : index;
    source = source.sibling(source.row(), 0);
    // Folder rows: a style opens or flattens them itself, launching a folder does nothing.
    if (IsGameDirectoryType(source.data(GameListItem::TypeRole).value<GameListItemType>())) {
        return;
    }
    ValidateEntry(source);
}

void GameList::OnStyleContextMenu(const QModelIndex& index, const QPoint& global_pos) {
    QModelIndex source;
    if (index.isValid()) {
        source = index.model() == style_model ? style_model->mapToSource(index) : index;
    }
    PopupContextMenuAt(source, global_pos);
}

void GameList::RestoreStyleSelection() {
    if (style_view == nullptr || style_selected_path.isEmpty() || style_model->rowCount() == 0) {
        return;
    }
    const QModelIndexList matches = style_model->match(
        style_model->index(0, 0), GameListItemPath::FullPathRole, style_selected_path, -1,
        Qt::MatchExactly | Qt::MatchRecursive);
    // The favorites row holds copies: prefer the entry inside its folder.
    QModelIndex found;
    for (const QModelIndex& match : matches) {
        if (!found.isValid()) {
            found = match;
        }
        if (match.parent().data(GameListItem::TypeRole).value<GameListItemType>() !=
            GameListItemType::Favorites) {
            found = match;
            break;
        }
    }
    // The view answers SetCurrentIndex with CurrentChanged; never re-enter from there.
    if (found.isValid() && !restoring_style_selection &&
        style_view->CurrentIndex() != found) {
        restoring_style_selection = true;
        style_view->SetCurrentIndex(found);
        restoring_style_selection = false;
    }
}

void GameList::OnTextChanged(const QString& new_text) {
    const QString edit_filter_text = new_text.toLower();
    style_model->SetFilterText(edit_filter_text);

    if (m_isTreeMode) {
        tree_view->ApplyFilter(edit_filter_text, item_model);
    } else {
        grid_view->ApplyFilter(edit_filter_text, item_model);
    }

    if (game_list_mode == Settings::GameListMode::GridView) {
        const QModelIndex root = grid_view->rootIndex();
        int matches = 0;
        int total = 0;
        if (root.isValid()) {
            total = item_model->rowCount(root);
            for (int row = 0; row < total; ++row) {
                matches += !grid_view->isRowHidden(row);
            }
        } else {
            for (int row = 1; row < item_model->rowCount() - 1; ++row) {
                const auto* directory = item_model->item(row, 0);
                const int game_count = directory->rowCount();
                total += game_count;
                for (int game_row = 0; game_row < game_count; ++game_row) {
                    const auto* game = directory->child(game_row, 0);
                    matches += Lichii::FilterMatches(edit_filter_text, game);
                }
            }
        }
        search_field->setFilterResult(matches, total);
    }
}

void GameList::OnFilterCloseClicked() {
    main_window->filterBarSetChecked(false);
}

void GameList::OnPopulatingCompleted(const QStringList& watch_list) {
    emit ShowList(game_list_mode == Settings::GameListMode::GridView ||
                  !item_model->IsEmpty());

    // favorites row
    if (m_isTreeMode) {
        tree_view->setRowHidden(0, item_model->invisibleRootItem()->index(),
                                UISettings::values.favorited_ids.isEmpty());
        tree_view->setExpanded(item_model->invisibleRootItem()->child(0)->index(),
                               UISettings::values.favorites_expanded.GetValue());
    }

    const int root_dir_index = grid_root_dir_index_to_restore;
    grid_root_dir_index_to_restore = -1;
    if (game_list_mode == Settings::GameListMode::GridView) {
        QModelIndex root_index;
        if (root_dir_index >= 0) {
            for (int row = 1; row < item_model->rowCount() - 1; ++row) {
                const QModelIndex directory = item_model->index(row, 0);
                if (directory.data(GameListDir::GameDirRole).toInt() == root_dir_index) {
                    root_index = directory;
                    break;
                }
            }
        }
        // First load after start: open the folder holding the last played game (or the first
        // folder) and select that game, instead of the folder overview.
        QModelIndex start_game;
        if (!root_index.isValid() && !start_folder_opened) {
            start_folder_opened = true;
            for (const QString& recent : std::as_const(UISettings::values.recent_files)) {
                // All hits: the Favorites row holds copies and comes first.
                const QModelIndexList matches = item_model->match(
                    item_model->index(0, 0), GameListItemPath::FullPathRole, recent, -1,
                    Qt::MatchExactly | Qt::MatchRecursive);
                for (const QModelIndex& match : matches) {
                    if (match.parent().isValid() &&
                        match.parent().data(GameListItem::TypeRole).value<GameListItemType>() !=
                            GameListItemType::Favorites) {
                        start_game = match;
                        break;
                    }
                }
                if (start_game.isValid()) {
                    break;
                }
            }
            if (start_game.isValid()) {
                root_index = start_game.parent();
            } else if (item_model->rowCount() > 2) {
                const QModelIndex first = item_model->index(1, 0);
                const auto type = first.data(GameListItem::TypeRole).value<GameListItemType>();
                if (type == GameListItemType::CustomDir || type == GameListItemType::SdmcDir ||
                    type == GameListItemType::UserNandDir || type == GameListItemType::SysNandDir) {
                    root_index = first;
                }
            }
            if (start_game.isValid() && style_selected_path.isEmpty()) {
                style_selected_path = start_game.data(GameListItemPath::FullPathRole).toString();
            }
        }
        grid_view->setRootIndex(root_index);
        if (root_index.isValid()) {
            grid_view->UpdateIconSize();
            grid_view->scrollToTop();
            if (start_game.isValid()) {
                grid_view->setCurrentIndex(start_game);
                grid_view->scrollTo(start_game);
            }
        } else {
            grid_view->setRowHidden(0, true);
        }
        UpdateFolderNavigation();
        for (int i = 1; i < item_model->rowCount() - 1; ++i) {
            if (auto* directory = item_model->item(i, 0)) {
                directory->sortChildren(GameListModel::COLUMN_NAME, Qt::AscendingOrder);
            }
        }
    }

    // restore collapsed/expanded flags
    if (m_isTreeMode) {
        const auto* root = item_model->invisibleRootItem();
        for (int i = 1; i < root->rowCount() - 1; ++i) {
            const auto* dir_item = root->child(i);
            const auto type = dir_item->data(GameListItem::TypeRole).value<GameListItemType>();
            if (type == GameListItemType::CustomDir || type == GameListItemType::SdmcDir ||
                type == GameListItemType::UserNandDir || type == GameListItemType::SysNandDir) {
                const int dir_index = dir_item->data(GameListDir::GameDirRole).toInt();
                if (dir_index >= 0 && dir_index < UISettings::values.game_dirs.size()) {
                    tree_view->setExpanded(dir_item->index(),
                                           UISettings::values.game_dirs[dir_index].expanded);
                }
            }
        }
    }

    // Watcher updates
    auto* watcher = item_model->GetWatcher();
    auto current_watch_list = watcher->directories();

    constexpr qsizetype LIMIT_WATCH_DIRECTORIES = 5000;
    constexpr int SLICE_SIZE = 25;

    QStringList to_remove, to_add;

    const auto slice = [&](const QStringList& list, std::function<void(const QStringList&)> callback) {
        const int len = (std::min)(list.size(), LIMIT_WATCH_DIRECTORIES);
        for (int i = 0; i < len; i += SLICE_SIZE) {
            auto chunk = list.mid(i, SLICE_SIZE);
            if (!chunk.isEmpty()) {
                callback(chunk);
            }
        }
    };

    // remove any paths not in the new watch list
    for (const auto& path : std::as_const(current_watch_list)) {
        if (!watch_list.contains(path)) {
            to_remove.emplaceBack(path);
        }
    }

    slice(to_remove, [watcher](const QStringList& chunk) { watcher->removePaths(chunk); });

    // add any paths not in the old watch list
    for (const auto& path : std::as_const(watch_list)) {
        if (!current_watch_list.contains(path)) {
            to_add.emplaceBack(path);
        }
    }

    slice(to_add, [watcher](const QStringList& chunk) { watcher->addPaths(chunk); });

    m_populating = false;
    m_currentView->setEnabled(true);

    int children_total = 0;
    for (int i = 1; i < item_model->rowCount() - 1; ++i) {
        children_total += item_model->item(i, 0)->rowCount();
    }

    search_field->setFilterResult(children_total, children_total);
    if (children_total > 0) {
        search_field->setFocus();
    }

    const int sort_column = tree_view->header()->sortIndicatorSection();
    const auto sort_order = tree_view->header()->sortIndicatorOrder();
    if (m_isTreeMode) {
        item_model->sort(sort_column, sort_order);
    }
    // The Carousel sorts its row of all games itself.
    carousel_view->SetSort(sort_column, sort_order);

    // A repopulate (a directory removed, deep scan toggled) replaces the rows: apply the search
    // text the user still has typed to them.
    if (!search_field->filterText().isEmpty()) {
        OnTextChanged(search_field->filterText());
    }

    if (style_view != nullptr) {
        style_view->setEnabled(true);
        RestoreStyleSelection();
        FocusStyleView();
    }

    emit PopulatingCompleted();
}

void GameList::RefreshGameDirectory() {
    item_model->RefreshGameDirectory();
}

void GameList::RefreshExternalContent() {
    item_model->RefreshExternalContent();
}

void GameList::UpdateIconSizes() {
    UpdateStyleToggles();
    if (style_view != nullptr) {
        style_view->UpdateIconSize();
    }
    // The cards read the icon size and the names setting when they paint, so a repaint is enough
    // even when the cell size stays the same.
    switch (game_list_mode) {
    case Settings::GameListMode::GridView:
        grid_view->UpdateIconSize();
        grid_view->viewport()->update();
        break;
    case Settings::GameListMode::CarouselView:
        carousel_view->UpdateIconSize();
        carousel_view->viewport()->update();
        break;
    case Settings::GameListMode::TreeView:
        // View > Game Info picks the columns here.
        tree_view->UpdateColumnVisibility(item_model);
        break;
    }
}

/// Gives a folder, Add New Game Directory or Favorites row the icon of the current theme again,
/// so a theme change doesn't need a new scan of the library.
static void RefreshThemeIcon(QStandardItem* item) {
    const char* icon_name = nullptr;
    switch (static_cast<GameListItemType>(item->type())) {
    case GameListItemType::SdmcDir:
        icon_name = "sd_card";
        break;
    case GameListItemType::UserNandDir:
    case GameListItemType::SysNandDir:
        icon_name = "chip";
        break;
    case GameListItemType::CustomDir:
        icon_name =
            QFileInfo::exists(item->data(Qt::DisplayRole).toString()) ? "folder" : "bad_folder";
        break;
    case GameListItemType::AddDir:
        icon_name = "list-add";
        break;
    case GameListItemType::Favorites:
        icon_name = "star";
        break;
    default:
        return;
    }
    item->setData(ThemeIcon(icon_name), Qt::DecorationRole);
}

void GameList::RefreshThemeIcons() {
    // Folder rows carry their theme icon from the scan; swap it in place instead of scanning again.
    for (int row = 0; row < item_model->rowCount(); ++row) {
        RefreshThemeIcon(item_model->item(row, 0));
    }
}

void GameList::ValidateEntry(const QModelIndex& item) {
    const auto selected = item.sibling(item.row(), 0);

    switch (selected.data(GameListItem::TypeRole).value<GameListItemType>()) {
    case GameListItemType::CustomDir:
    case GameListItemType::SdmcDir:
    case GameListItemType::UserNandDir:
    case GameListItemType::SysNandDir:
        if (game_list_mode == Settings::GameListMode::GridView &&
            !grid_view->rootIndex().isValid()) {
            grid_view->setRootIndex(selected);
            grid_view->UpdateIconSize();
            UpdateFolderNavigation();
            grid_view->scrollToTop();
            if (!search_field->filterText().isEmpty()) {
                OnTextChanged(search_field->filterText());
            }
        }
        break;
    case GameListItemType::Game: {
        const QString file_path = selected.data(GameListItemPath::FullPathRole).toString();
        if (file_path.isEmpty())
            return;
        const QFileInfo file_info(file_path);
        if (!file_info.exists())
            return;

        if (file_info.isDir()) {
            const QDir dir{file_path};
            const QStringList matching_main = dir.entryList({QStringLiteral("main")}, QDir::Files);
            if (matching_main.size() == 1) {
                emit GameChosen(dir.path() + QDir::separator() + matching_main[0]);
            }
            return;
        }

        const auto title_id = selected.data(GameListItemPath::ProgramIdRole).toULongLong();

        search_field->clear();
        emit GameChosen(file_path, title_id);
        break;
    }
    case GameListItemType::AddDir:
        emit AddDirectory();
        break;
    default:
        break;
    }
}

void GameList::ToggleFavorite(u64 program_id) {
    item_model->ToggleFavorite(program_id);

    if (UISettings::values.favorited_ids.contains(program_id)) {
        tree_view->setRowHidden(0, item_model->invisibleRootItem()->index(),
                                !search_field->filterText().isEmpty());
        item_model->sort(tree_view->header()->sortIndicatorSection(),
                         tree_view->header()->sortIndicatorOrder());
    } else if (UISettings::values.favorited_ids.isEmpty()) {
        tree_view->setRowHidden(0, item_model->invisibleRootItem()->index(), true);
    }
}

void GameList::ReturnToGameDirectories() {
    if (game_list_mode != Settings::GameListMode::GridView) {
        return;
    }
    grid_view->setRootIndex(QModelIndex{});
    grid_view->UpdateIconSize();
    grid_view->clearSelection();
    // Hide the favorites row again; it is only added when a folder root is displayed.
    grid_view->setRowHidden(0, true);
    UpdateFolderNavigation();
    grid_view->scrollToTop();
    if (!search_field->filterText().isEmpty()) {
        OnTextChanged(search_field->filterText());
    }
}

void GameList::UpdateFolderNavigation() {
    if (game_list_mode != Settings::GameListMode::GridView || !grid_view->rootIndex().isValid()) {
        folder_navigation->hide();
        return;
    }

    const QModelIndex directory = grid_view->rootIndex();
    QString title = directory.data(Qt::DisplayRole).toString();
    const auto type = directory.data(GameListItem::TypeRole).value<GameListItemType>();
    if (type == GameListItemType::CustomDir) {
        const int dir_index = directory.data(GameListDir::GameDirRole).toInt();
        if (dir_index >= 0 && dir_index < UISettings::values.game_dirs.size()) {
            const QString path =
                QString::fromStdString(UISettings::values.game_dirs[dir_index].path);
            const QString name = QFileInfo(path).fileName();
            if (!name.isEmpty()) {
                title = name;
            }
            folder_title_label->setToolTip(path);
        }
    } else {
        folder_title_label->setToolTip(QString{});
    }
    folder_title_label->setText(title);
    folder_navigation->show();
}

void GameList::PopupContextMenu(const QPoint& menu_location) {
    PopupContextMenuAt(m_currentView->indexAt(menu_location),
                       m_currentView->viewport()->mapToGlobal(menu_location));
}

void GameList::PopupContextMenuAt(const QModelIndex& item, const QPoint& global_pos) {
    if (!item.isValid()) {
        if (m_isTreeMode)
            return;

        QMenu blank_menu;
        QAction* addGameDirAction = blank_menu.addAction(tr("&Add New Game Directory"));
        addGameDirAction->setIcon(MenuIcon("plus"));

        connect(addGameDirAction, &QAction::triggered, this, &GameList::AddDirectory);
        blank_menu.exec(global_pos);
        return;
    }

    const auto selected = item.sibling(item.row(), 0);
    QMenu context_menu;
    switch (selected.data(GameListItem::TypeRole).value<GameListItemType>()) {
    case GameListItemType::Game:
        AddGamePopup(context_menu, selected.data(GameListItemPath::ProgramIdRole).toULongLong(),
                     selected.data(GameListItemPath::FullPathRole).toString().toStdString());
        break;
    case GameListItemType::CustomDir:
        AddPermDirPopup(context_menu, selected);
        AddCustomDirPopup(context_menu, selected);
        break;
    case GameListItemType::SdmcDir:
    case GameListItemType::UserNandDir:
    case GameListItemType::SysNandDir:
        AddPermDirPopup(context_menu, selected);
        break;
    case GameListItemType::Favorites:
        AddFavoritesPopup(context_menu);
        break;
    default:
        break;
    }
    context_menu.exec(global_pos);
}

void GameList::SetReportableProgramId(u64 program_id) {
    reportable_program_id = program_id;
}

void GameList::AddGamePopup(QMenu& context_menu, u64 program_id, const std::string& path) {
    // TODO(crueter): Refactor this and make it less bad
    QAction* favorite = context_menu.addAction(tr("Favorite"));
    context_menu.addSeparator();
    QAction* start_game = context_menu.addAction(tr("Start Game"));
    QAction* start_game_global =
        context_menu.addAction(tr("Start Game without Custom Configuration"));
    context_menu.addSeparator();
    QAction* open_save_location = context_menu.addAction(tr("Open Save Data Location"));
    QAction* open_mod_location = context_menu.addAction(tr("Open Mod Data Location"));
    QAction* open_transferable_shader_cache =
        context_menu.addAction(tr("Open Transferable Pipeline Cache"));
    const bool has_shader_warm = ShaderWarmFilesExist(program_id);
    QAction* prepare_shaders = context_menu.addAction(
        has_shader_warm ? tr("Rebuild Shader Precache") : tr("Build Shader Precache"));
    QAction* delete_shader_warm = nullptr;
    if (has_shader_warm) {
        delete_shader_warm = context_menu.addAction(tr("Delete Shader Precache"));
    }
    QAction* ryujinx = context_menu.addAction(tr("Link to Ryujinx"));
    context_menu.addSeparator();
    QMenu* remove_menu = context_menu.addMenu(tr("Remove"));
    QAction* remove_update = remove_menu->addAction(tr("Remove Installed Update"));
    QAction* remove_dlc = remove_menu->addAction(tr("Remove All Installed DLC"));
    QAction* remove_custom_config = remove_menu->addAction(tr("Remove Custom Configuration"));
    QAction* remove_cache_storage = remove_menu->addAction(tr("Remove Cache Storage"));
    QAction* remove_gl_shader_cache = remove_menu->addAction(tr("Remove OpenGL Pipeline Cache"));
    QAction* remove_vk_shader_cache = remove_menu->addAction(tr("Remove Vulkan Pipeline Cache"));
    remove_menu->addSeparator();
    QAction* remove_shader_cache = remove_menu->addAction(tr("Remove All Pipeline Caches"));
    QAction* remove_all_content = remove_menu->addAction(tr("Remove All Installed Contents"));
    QMenu* play_time_menu = context_menu.addMenu(tr("Manage Play Time"));
    QAction* set_play_time = play_time_menu->addAction(tr("Edit Play Time Data"));
    QAction* remove_play_time_data = play_time_menu->addAction(tr("Remove Play Time Data"));
    const bool is_loose_dump =
        FileSys::Nup::DumpRootFromMain(std::filesystem::path{Common::FS::ToU8String(path)})
            .has_value();
    const bool is_nup_archive =
        !is_loose_dump &&
        QString::fromStdString(path).endsWith(QStringLiteral(".nup"), Qt::CaseInsensitive);
    QAction* extract_nup = nullptr;
    if (is_nup_archive) {
        extract_nup = context_menu.addAction(tr("Dump NUP..."));
        extract_nup->setEnabled(!nup_conversion_running);
    }
    QMenu* dump_romfs_menu = context_menu.addMenu(tr("Dump RomFS"));
    QAction* dump_romfs = dump_romfs_menu->addAction(tr("Dump RomFS"));
    QAction* dump_romfs_sdmc = dump_romfs_menu->addAction(tr("Dump RomFS to SDMC"));
    QAction* verify_integrity = context_menu.addAction(tr("Verify Integrity"));
    QAction* copy_tid = context_menu.addAction(tr("Copy Title ID to Clipboard"));
    QAction* convert_nup = nullptr;
    if (is_loose_dump) {
        convert_nup = context_menu.addAction(tr("Convert to NUP..."));
        convert_nup->setEnabled(!nup_conversion_running);
    }
    QAction* convert_nx = nullptr;
    QAction* add_to_nx = nullptr;
    QAction* remove_from_nx = nullptr;
    QAction* edit_nx_metadata = nullptr;
#ifdef LICHII_NX_TOOLS
    if (NxTools::CanConvert(path)) {
        convert_nx = context_menu.addAction(tr("Convert to NX..."));
        convert_nx->setEnabled(!nx_tool_running);
    }
    if (NxTools::IsNxArchive(path)) {
        QMenu* nx_menu = context_menu.addMenu(tr("Manage NX Archive"));
        add_to_nx = nx_menu->addAction(tr("Add Update or DLC..."));
        remove_from_nx = nx_menu->addAction(tr("Remove Update or DLC..."));
        nx_menu->addSeparator();
        edit_nx_metadata = nx_menu->addAction(tr("Edit Title and Icon..."));
        nx_menu->setEnabled(!nx_tool_running);
        nx_menu->menuAction()->setIcon(MenuIcon("archive"));
    }
#endif
#if !defined(__APPLE__)
    QMenu* shortcut_menu = context_menu.addMenu(tr("Create Shortcut"));
    shortcut_menu->menuAction()->setIcon(MenuIcon("share"));
    QAction* create_desktop_shortcut = shortcut_menu->addAction(tr("Add to Desktop"));
    QAction* create_applications_menu_shortcut =
        shortcut_menu->addAction(tr("Add to Applications Menu"));
#endif
    context_menu.addSeparator();
    QAction* edit_nup = nullptr;
    if (is_nup_archive) {
        edit_nup = context_menu.addAction(tr("Edit NUP Metadata..."));
        edit_nup->setEnabled(!nup_conversion_running);
    }
    QAction* properties = context_menu.addAction(tr("Configure Game"));
    if (program_id != 0 && program_id == reportable_program_id) {
        auto* report = context_menu.addAction(tr("Submit compatibility report…"));
        connect(report, &QAction::triggered, this,
                [this, program_id] { emit ReportCompatibilityRequested(program_id); });
    }

    const std::array<std::pair<QAction*, const char*>, 20> icons{{
        {favorite, "star"},
        {start_game, "play"},
        {start_game_global, "play"},
        {open_save_location, "folder"},
        {open_mod_location, "puzzle"},
        {open_transferable_shader_cache, "layers"},
        {prepare_shaders, "cpu"},
        {delete_shader_warm, "trash"},
        {ryujinx, "link"},
        {remove_menu->menuAction(), "trash"},
        {play_time_menu->menuAction(), "clock"},
        {extract_nup, "archive"},
        {dump_romfs_menu->menuAction(), "hdd"},
        {verify_integrity, "check"},
        {copy_tid, "copy"},
        {convert_nup, "archive"},
        {convert_nx, "archive"},
        {edit_nx_metadata, "edit"},
        {edit_nup, "edit"},
        {properties, "sliders"},
    }};
    for (const auto& [action, name] : icons) {
        if (action != nullptr) {
            action->setIcon(MenuIcon(name));
        }
    }

    favorite->setVisible(program_id != 0);
    favorite->setCheckable(true);
    favorite->setChecked(UISettings::values.favorited_ids.contains(program_id));
    open_save_location->setVisible(program_id != 0);
    open_mod_location->setVisible(program_id != 0);
    open_transferable_shader_cache->setVisible(program_id != 0);
    prepare_shaders->setVisible(program_id != 0);
    remove_update->setVisible(program_id != 0);
    remove_dlc->setVisible(program_id != 0);
    remove_gl_shader_cache->setVisible(program_id != 0);
    remove_vk_shader_cache->setVisible(program_id != 0);
    remove_shader_cache->setVisible(program_id != 0);
    remove_all_content->setVisible(program_id != 0);

    connect(favorite, &QAction::triggered, this,
            [this, program_id]() { ToggleFavorite(program_id); });
    connect(open_save_location, &QAction::triggered, this, [this, program_id, path]() {
        emit OpenFolderRequested(program_id, GameListOpenTarget::SaveData, path);
    });
    connect(start_game, &QAction::triggered, this,
            [this, path]() { emit BootGame(QString::fromStdString(path), StartGameType::Normal); });
    connect(start_game_global, &QAction::triggered, this,
            [this, path]() { emit BootGame(QString::fromStdString(path), StartGameType::Global); });
    connect(open_mod_location, &QAction::triggered, this, [this, program_id, path]() {
        emit OpenFolderRequested(program_id, GameListOpenTarget::ModData, path);
    });
    connect(open_transferable_shader_cache, &QAction::triggered, this,
            [this, program_id]() { emit OpenTransferableShaderCacheRequested(program_id); });
    connect(prepare_shaders, &QAction::triggered, this,
            [this, program_id, path]() { emit PrepareShaderWarmRequested(program_id, path); });
    if (delete_shader_warm) {
        connect(delete_shader_warm, &QAction::triggered, this,
                [this, program_id]() { emit DeleteShaderWarmRequested(program_id); });
    }
    connect(remove_all_content, &QAction::triggered, this, [this, program_id]() {
        emit RemoveInstalledEntryRequested(program_id, QtCommon::Game::InstalledEntryType::Game);
    });
    connect(remove_update, &QAction::triggered, this, [this, program_id]() {
        emit RemoveInstalledEntryRequested(program_id, QtCommon::Game::InstalledEntryType::Update);
    });
    connect(remove_dlc, &QAction::triggered, this, [this, program_id]() {
        emit RemoveInstalledEntryRequested(program_id,
                                           QtCommon::Game::InstalledEntryType::AddOnContent);
    });
    connect(remove_gl_shader_cache, &QAction::triggered, this, [this, program_id, path]() {
        emit RemoveFileRequested(program_id, QtCommon::Game::GameListRemoveTarget::GlShaderCache,
                                 path);
    });
    connect(remove_vk_shader_cache, &QAction::triggered, this, [this, program_id, path]() {
        emit RemoveFileRequested(program_id, QtCommon::Game::GameListRemoveTarget::VkShaderCache,
                                 path);
    });
    connect(remove_shader_cache, &QAction::triggered, this, [this, program_id, path]() {
        emit RemoveFileRequested(program_id, QtCommon::Game::GameListRemoveTarget::AllShaderCache,
                                 path);
    });
    connect(remove_custom_config, &QAction::triggered, this, [this, program_id, path]() {
        emit RemoveFileRequested(program_id,
                                 QtCommon::Game::GameListRemoveTarget::CustomConfiguration, path);
    });
    connect(set_play_time, &QAction::triggered, this,
            [this, program_id]() { emit SetPlayTimeRequested(program_id); });
    connect(remove_play_time_data, &QAction::triggered, this,
            [this, program_id]() { emit RemovePlayTimeRequested(program_id); });
    connect(remove_cache_storage, &QAction::triggered, this, [this, program_id, path] {
        emit RemoveFileRequested(program_id, QtCommon::Game::GameListRemoveTarget::CacheStorage,
                                 path);
    });
    connect(dump_romfs, &QAction::triggered, this, [this, program_id, path]() {
        emit DumpRomFSRequested(program_id, path, DumpRomFSTarget::Normal);
    });
    connect(dump_romfs_sdmc, &QAction::triggered, this, [this, program_id, path]() {
        emit DumpRomFSRequested(program_id, path, DumpRomFSTarget::SDMC);
    });
    connect(verify_integrity, &QAction::triggered, this,
            [this, path]() { emit VerifyIntegrityRequested(path); });
    connect(copy_tid, &QAction::triggered, this,
            [this, program_id]() { emit CopyTIDRequested(program_id); });
    if (convert_nup) {
        connect(convert_nup, &QAction::triggered, this,
                [this, path]() { emit ConvertToNupRequested(path); });
    }
    if (convert_nx) {
        connect(convert_nx, &QAction::triggered, this,
                [this, path]() { emit ConvertToNxRequested(path); });
    }
    if (add_to_nx) {
        connect(add_to_nx, &QAction::triggered, this,
                [this, path]() { emit AddToNxRequested(path); });
        connect(remove_from_nx, &QAction::triggered, this,
                [this, path]() { emit RemoveFromNxRequested(path); });
        connect(edit_nx_metadata, &QAction::triggered, this,
                [this, path]() { emit EditNxMetadataRequested(path); });
    }
    if (extract_nup) {
        connect(extract_nup, &QAction::triggered, this,
                [this, path]() { emit ExtractNupRequested(path); });
    }
    if (edit_nup) {
        connect(edit_nup, &QAction::triggered, this,
                [this, path]() { emit EditNupMetadataRequested(path); });
    }
#if !defined(__APPLE__)
    connect(create_desktop_shortcut, &QAction::triggered, this, [this, program_id, path]() {
        emit CreateShortcut(program_id, path, QtCommon::Game::ShortcutTarget::Desktop);
    });
    connect(create_applications_menu_shortcut, &QAction::triggered, this,
            [this, program_id, path]() {
                emit CreateShortcut(program_id, path, QtCommon::Game::ShortcutTarget::Applications);
            });
#endif
    connect(properties, &QAction::triggered, this,
            [this, path]() { emit OpenPerGameGeneralRequested(path); });

    connect(ryujinx, &QAction::triggered, this,
            [this, program_id]() { emit LinkToRyujinxRequested(program_id); });
};

void GameList::AddCustomDirPopup(QMenu& context_menu, QModelIndex selected) {
    UISettings::GameDir& game_dir =
        UISettings::values.game_dirs[selected.data(GameListDir::GameDirRole).toInt()];

    QAction* deep_scan = context_menu.addAction(tr("Scan Subfolders"));
    QAction* delete_dir = context_menu.addAction(tr("Remove Game Directory"));
    delete_dir->setIcon(MenuIcon("x"));

    deep_scan->setCheckable(true);
    deep_scan->setChecked(game_dir.deep_scan);

    connect(deep_scan, &QAction::triggered, this, [this, &game_dir] {
        game_dir.deep_scan = !game_dir.deep_scan;
        PopulateAsync(UISettings::values.game_dirs);
    });
    connect(delete_dir, &QAction::triggered, this, [this, &game_dir] {
        UISettings::values.game_dirs.removeOne(game_dir);
        // Repopulate instead of removing the row: a running worker adds games under the
        // directory rows it created, and the other rows' GameDirRole indices would shift.
        PopulateAsync(UISettings::values.game_dirs);
    });
}

void GameList::AddPermDirPopup(QMenu& context_menu, QModelIndex selected) {
    const int game_dir_index = selected.data(GameListDir::GameDirRole).toInt();

    QAction* move_up = context_menu.addAction(tr("\u25B2 Move Up"));
    QAction* move_down = context_menu.addAction(tr("\u25bc Move Down"));
    QAction* open_directory_location = context_menu.addAction(tr("Open Directory Location"));
    open_directory_location->setIcon(MenuIcon("folder"));

    const int row = selected.row();

    move_up->setEnabled(row > 1);
    move_down->setEnabled(row < item_model->rowCount() - 2);

    connect(move_up, &QAction::triggered, this, [this, selected, row, game_dir_index] {
        const int other_index = selected.sibling(row - 1, 0).data(GameListDir::GameDirRole).toInt();
        std::swap(UISettings::values.game_dirs[game_dir_index],
                  UISettings::values.game_dirs[other_index]);
        item_model->setData(selected, QVariant(other_index), GameListDir::GameDirRole);
        item_model->setData(selected.sibling(row - 1, 0), QVariant(game_dir_index),
                            GameListDir::GameDirRole);
        QList<QStandardItem*> items = item_model->takeRow(row);
        item_model->invisibleRootItem()->insertRow(row - 1, items);
        tree_view->setExpanded(selected.sibling(row - 1, 0),
                               UISettings::values.game_dirs[other_index].expanded);
    });

    connect(move_down, &QAction::triggered, this, [this, selected, row, game_dir_index] {
        const int other_index = selected.sibling(row + 1, 0).data(GameListDir::GameDirRole).toInt();
        std::swap(UISettings::values.game_dirs[game_dir_index],
                  UISettings::values.game_dirs[other_index]);
        item_model->setData(selected, QVariant(other_index), GameListDir::GameDirRole);
        item_model->setData(selected.sibling(row + 1, 0), QVariant(game_dir_index),
                            GameListDir::GameDirRole);
        const QList<QStandardItem*> items = item_model->takeRow(row);
        item_model->invisibleRootItem()->insertRow(row + 1, items);
        tree_view->setExpanded(selected.sibling(row + 1, 0),
                               UISettings::values.game_dirs[other_index].expanded);
    });

    connect(open_directory_location, &QAction::triggered, this, [this, game_dir_index] {
        emit OpenDirectory(
            QString::fromStdString(UISettings::values.game_dirs[game_dir_index].path));
    });
}

void GameList::AddFavoritesPopup(QMenu& context_menu) {
    QAction* clear = context_menu.addAction(tr("Clear"));

    connect(clear, &QAction::triggered, this, [this] {
        UISettings::values.favorited_ids.clear();
        item_model->invisibleRootItem()->child(0)->removeRows(
            0, item_model->invisibleRootItem()->child(0)->rowCount());
        tree_view->setRowHidden(0, item_model->invisibleRootItem()->index(), true);
    });
}

void GameList::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void GameList::RetranslateUI() {
    item_model->RetranslateUI();
}

void GameList::SetupScrollAnimation() {
    auto setup = [this](QVariantAnimation* anim, QScrollBar* bar) {
        anim->setEasingCurve(QEasingCurve::OutCubic);
        anim->setDuration(200);
        connect(anim, &QVariantAnimation::valueChanged, this,
                [bar](const QVariant& value) { bar->setValue(value.toInt()); });
    };

    vertical_scroll = new QVariantAnimation(this);
    horizontal_scroll = new QVariantAnimation(this);

    setup(vertical_scroll, tree_view->verticalScrollBar());
    setup(horizontal_scroll, tree_view->horizontalScrollBar());

    setup(vertical_scroll, grid_view->verticalScrollBar());
    setup(horizontal_scroll, grid_view->horizontalScrollBar());
}

bool GameList::eventFilter(QObject* obj, QEvent* event) {
    if (style_view != nullptr && IsStyleObject(obj)) {
        if (event->type() == QEvent::KeyPress &&
            HandleStyleKey(obj, static_cast<QKeyEvent*>(event))) {
            return true;
        }
        if (event->type() == QEvent::ChildAdded) {
            QObject* child = static_cast<QChildEvent*>(event)->child();
            if (child != nullptr && child->isWidgetType()) {
                child->installEventFilter(this);
            }
        }
        return QWidget::eventFilter(obj, event);
    }

    if (obj == m_currentView && event->type() == QEvent::KeyPress &&
        game_list_mode == Settings::GameListMode::GridView) {
        const auto* key_event = static_cast<QKeyEvent*>(event);
        if (grid_view->rootIndex().isValid() &&
            (key_event->key() == Qt::Key_Escape || key_event->key() == Qt::Key_Backspace)) {
            ReturnToGameDirectories();
            return true;
        }
        if (key_event->key() == Qt::Key_Return || key_event->key() == Qt::Key_Enter) {
            ValidateEntry(grid_view->currentIndex());
            return true;
        }
    }

    if (obj == m_currentView->viewport() && event->type() == QEvent::Wheel) {
        QWheelEvent* wheelEvent = static_cast<QWheelEvent*>(event);

        bool horizontal = wheelEvent->modifiers() & Qt::ShiftModifier;

        int deltaX = wheelEvent->angleDelta().x();
        int deltaY = wheelEvent->angleDelta().y();

        if (horizontal && deltaY != 0 && deltaX == 0) {
            deltaX = deltaY;
            deltaY = 0;
        }

        if (deltaY != 0) {
            if (vertical_scroll->state() == QAbstractAnimation::Stopped)
                vertical_scroll_target = m_currentView->verticalScrollBar()->value();

            vertical_scroll_target -= deltaY;
            vertical_scroll_target =
                qBound(0, vertical_scroll_target, m_currentView->verticalScrollBar()->maximum());

            vertical_scroll->stop();
            vertical_scroll->setStartValue(m_currentView->verticalScrollBar()->value());
            vertical_scroll->setEndValue(vertical_scroll_target);
            vertical_scroll->start();
        }

        if (deltaX != 0) {
            if (horizontal_scroll->state() == QAbstractAnimation::Stopped)
                horizontal_scroll_target = m_currentView->horizontalScrollBar()->value();

            horizontal_scroll_target -= deltaX;
            horizontal_scroll_target = qBound(0, horizontal_scroll_target,
                                              m_currentView->horizontalScrollBar()->maximum());

            horizontal_scroll->stop();
            horizontal_scroll->setStartValue(m_currentView->horizontalScrollBar()->value());
            horizontal_scroll->setEndValue(horizontal_scroll_target);
            horizontal_scroll->start();
        }

        return true;
    }

    if (obj == m_currentView->viewport() && event->type() == QEvent::MouseButtonPress) {
        QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);

        QModelIndex index = m_currentView->indexAt(mouseEvent->pos());
        if (!index.isValid()) {
            m_currentView->selectionModel()->clearSelection();
            m_currentView->setCurrentIndex(QModelIndex());
        }
    }

    if (obj == grid_view->viewport() && event->type() == QEvent::Resize) {
        grid_view->UpdateIconSize();
        return true;
    }

    if (obj == carousel_view->viewport() && event->type() == QEvent::Resize) {
        carousel_view->UpdateIconSize();
        return true;
    }

    return QWidget::eventFilter(obj, event);
}

GameListPlaceholder::GameListPlaceholder(MainWindow* parent) : QWidget{parent} {
    layout = new QVBoxLayout;
    image = new QLabel;
    text = new QLabel;
    layout->setAlignment(Qt::AlignCenter);
    RefreshThemeIcon();

    RetranslateUI();
    QFont font = text->font();
    font.setPointSize(20);
    text->setFont(font);
    text->setAlignment(Qt::AlignHCenter);
    image->setAlignment(Qt::AlignHCenter);

    layout->addWidget(image);
    layout->addWidget(text);
    setLayout(layout);
}

GameListPlaceholder::~GameListPlaceholder() = default;

void GameListPlaceholder::mouseDoubleClickEvent(QMouseEvent* event) {
    emit GameListPlaceholder::AddDirectory();
}

void GameListPlaceholder::RefreshThemeIcon() {
    image->setPixmap(QIcon::fromTheme(QStringLiteral("plus_folder")).pixmap(200));
}

void GameListPlaceholder::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void GameListPlaceholder::RetranslateUI() {
    text->setText(tr("Double-click to add a new folder to the game list"));
}
