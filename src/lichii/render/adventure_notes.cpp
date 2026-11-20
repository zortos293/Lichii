// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <filesystem>
#include <fstream>
#include <optional>

#include <fmt/format.h>

#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFile>
#include <QBoxLayout>
#include <QColor>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QProgressBar>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStringList>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#include "common/common_types.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/core.h"
#include "qt_common/qt_common.h"
#include "main_window.h"
#include "render/adventure_notes.h"

namespace {
namespace fs = std::filesystem;

fs::path NotesDir() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "notes";
}

fs::path ShippedDir() {
    return fs::path{QCoreApplication::applicationDirPath().toStdString()} / "notes";
}

fs::path UserModule(u64 program_id) {
    return NotesDir() / fmt::format("{:016X}.js", program_id);
}

/// The user's module, else the one shipped next to the program.
fs::path FindModule(u64 program_id) {
    std::error_code ec;
    const fs::path user = UserModule(program_id);
    if (fs::exists(user, ec)) {
        return user;
    }
    const fs::path shipped = ShippedDir() / user.filename();
    return fs::exists(shipped, ec) ? shipped : user;
}

QStringList LibraryDirs() {
    return {QString::fromStdString((NotesDir() / "lib").string()),
            QString::fromStdString((ShippedDir() / "lib").string())};
}

/// Where the starred lines of a game are kept, next to the modules' own state.
fs::path FavoritesFile(u64 program_id) {
    return NotesDir() / "state" / fmt::format("{:016X}-favorites.json", program_id);
}

constexpr QStringView FavoritesTab = u"★ Favorites";

/// One component of a favorite's key: '\\' and '/' are escaped, so a name that contains a '/' cannot
/// be mistaken for two components.
QString EscapeKeyPart(const QString& part) {
    QString out;
    out.reserve(part.size());
    for (const QChar c : part) {
        if (c == QLatin1Char('/') || c == QLatin1Char('\\')) {
            out.append(QLatin1Char('\\'));
        }
        out.append(c);
    }
    return out;
}

QString FavoriteKey(const QString& tab, const QString& view, const QString& title) {
    return QStringLiteral("%1/%2/%3").arg(EscapeKeyPart(tab), EscapeKeyPart(view),
                                          EscapeKeyPart(title));
}

/// The components of a key, split at the '/' that are not escaped. A key saved before the
/// escaping existed has no escapes: whatever follows the second '/' is then the title.
QStringList SplitFavoriteKey(const QString& key) {
    QStringList parts;
    QString current;
    for (qsizetype i = 0; i < key.size(); ++i) {
        const QChar c = key[i];
        if (c == QLatin1Char('\\') && i + 1 < key.size() &&
            (key[i + 1] == QLatin1Char('/') || key[i + 1] == QLatin1Char('\\'))) {
            current.append(key[++i]);
        } else if (c == QLatin1Char('/') && parts.size() < 2) {
            parts.append(std::exchange(current, QString{}));
        } else {
            current.append(c);
        }
    }
    parts.append(current);
    return parts;
}

fs::path SaveRoot() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) / "user" / "save" /
           "0000000000000000";
}

/// The slot with the newest copy of the file, which may also sit directly in the title's folder.
std::optional<fs::path> NewestSave(const std::string& folder, const std::string& name) {
    std::error_code ec;
    std::optional<fs::path> newest;
    fs::file_time_type newest_time{};
    const auto consider = [&](const fs::path& file) {
        const auto time = fs::last_write_time(file, ec);
        if (!ec && (!newest || time > newest_time)) {
            newest = file;
            newest_time = time;
        }
        ec.clear();
    };
    for (const auto& profile : fs::directory_iterator{SaveRoot(), ec}) {
        const fs::path title = profile.path() / folder;
        consider(title / name);
        for (const auto& slot : fs::directory_iterator{title, ec}) {
            consider(slot.path() / name);
        }
    }
    return newest;
}

/// What the game saves, to name the right file in a new module.
QString SaveFiles(const std::string& folder) {
    std::error_code ec;
    QStringList names;
    for (const auto& profile : fs::directory_iterator{SaveRoot(), ec}) {
        for (const auto& entry :
             fs::recursive_directory_iterator{profile.path() / folder, ec}) {
            if (entry.is_regular_file(ec)) {
                const QString name = QString::fromStdString(entry.path().filename().string());
                if (!name.startsWith(QLatin1Char('.')) && !names.contains(name)) {
                    names.push_back(name);
                }
            }
        }
    }
    return names.join(QStringLiteral(", "));
}

QSettings PanelSettings() {
    return {QString::fromStdString((NotesDir() / "panel.ini").string()), QSettings::IniFormat};
}

constexpr int RailWidth = 48;
const QSize RailSize{300, 460};
const QSize StripSize{700, 300};
const QString DefaultAccent = QStringLiteral("#9fd36a");

// A module that works as it is: it shows the save's size, and read() is the place to go on.
constexpr const char* ModuleTemplate = R"(// Adventure notes module. Saving this file reloads the panel.
var game = { title: "My game", folder: "%1", file: "progress.sav", accent: "#9fd36a" };

// save.data is the file as an ArrayBuffer. Read it with DataView or typed arrays.
function read(save) {
    return {
        title: game.title,
        keys: [{ title: "Save size", value: save.data.byteLength }],
        tabs: [
            { name: "Stats", views: [
                { name: "Summary", rows: [
                    { title: "Save size", value: save.data.byteLength },
                ] },
            ] },
        ],
    };
}
)";
} // Anonymous namespace

AdventureNotes::~AdventureNotes() {
    SaveSettings();
}

AdventureNotes::AdventureNotes(MainWindow* parent, u64 program_id_)
    : QWidget(parent), program_id{program_id_} {
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);

    {
        QSettings settings = PanelSettings();
        strip = settings.value(QStringLiteral("strip"), false).toBool();
        sizes[0] = settings.value(QStringLiteral("rail_size"), RailSize).toSize();
        sizes[1] = settings.value(QStringLiteral("strip_size"), StripSize).toSize();
    }

    subtitle = new QLabel(this);
    subtitle->setObjectName(QStringLiteral("subtitle"));
    subtitle->installEventFilter(this);
    header = new QLabel(this);
    header->setObjectName(QStringLiteral("header"));
    header->installEventFilter(this);
    auto* titles = new QVBoxLayout;
    titles->setContentsMargins(0, 0, 0, 0);
    titles->setSpacing(0);
    titles->addWidget(subtitle);
    titles->addWidget(header);

    tab_box = new QWidget(this);
    tab_layout = new QBoxLayout(QBoxLayout::TopToBottom, tab_box);
    tab_layout->setContentsMargins(0, 0, 0, 0);
    tab_layout->setSpacing(5);

    key_box = new QWidget(this);
    key_layout = new QHBoxLayout(key_box);
    key_layout->setContentsMargins(0, 0, 0, 0);
    key_layout->setSpacing(16);

    flip = new QToolButton(this);
    flip->setText(QStringLiteral("⇄"));
    flip->setToolTip(tr("Switch between the rail and the strip"));
    connect(flip, &QToolButton::clicked, this, &AdventureNotes::SwitchLayout);
    reload = new QToolButton(this);
    reload->setText(QStringLiteral("↻"));
    reload->setToolTip(tr("Read the module and the save again"));
    connect(reload, &QToolButton::clicked, this, &AdventureNotes::LoadGame);
    collapse = new QToolButton(this);
    collapse->setText(QStringLiteral("–"));
    connect(collapse, &QToolButton::clicked, this, &AdventureNotes::ToggleFold);
    tools = new QWidget(this);
    tools_layout = new QBoxLayout(QBoxLayout::TopToBottom, tools);
    tools_layout->setContentsMargins(0, 0, 0, 0);
    tools_layout->setSpacing(2);
    tools_layout->addWidget(reload);
    tools_layout->addWidget(flip);
    tools_layout->addWidget(collapse);

    rail = new QWidget(this);
    rail->setFixedWidth(RailWidth);
    rail_layout = new QVBoxLayout(rail);
    rail_layout->setContentsMargins(7, 12, 7, 10);
    rail_layout->setSpacing(6);
    rail_layout->addStretch(1);

    top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(16);
    top->addLayout(titles);

    views = new QComboBox(this);
    create = new QToolButton(this);
    create->setText(tr("Create a module for this game"));
    create->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connect(create, &QToolButton::clicked, this, &AdventureNotes::CreateModule);

    body = new QWidget;
    rows = new QGridLayout(body);
    scroll = new QScrollArea(this);
    scroll->setWidget(body);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    grip = new QLabel(QStringLiteral("⌟"), this);
    grip->setObjectName(QStringLiteral("subtitle"));
    grip->setCursor(Qt::SizeFDiagCursor);
    grip->installEventFilter(this);

    content = new QWidget(this);
    column = new QVBoxLayout(content);
    column->setContentsMargins(14, 11, 12, 6);
    column->setSpacing(7);
    column->addLayout(top);
    column->addWidget(views);
    column->addWidget(scroll, 1);
    column->addWidget(create);
    column->addWidget(grip, 0, Qt::AlignRight);

    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(rail);
    outer->addWidget(content, 1);

    connect(views, &QComboBox::currentIndexChanged, this, &AdventureNotes::Refresh);
    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &AdventureNotes::CheckFiles);
    timer->start(1000);
    // A module that follows the running game (tick) runs ten times a second, apart from the file
    // checks: room changes are timed to a tenth of a second.
    ticker = new QTimer(this);
    connect(ticker, &QTimer::timeout, this, &AdventureNotes::TickModule);
    ticker->start(100);
    SetAccent(DefaultAccent);
    ApplyLayout();
    resize(sizes[strip ? 1 : 0]);
    LoadGame();
}

void AdventureNotes::SetAccent(const QString& color) {
    accent = QColor::isValidColorName(color) ? color : DefaultAccent;
    QString style = QStringLiteral(
        "QLabel { color: #e8eaed; font-size: 13px; background: transparent; }"
        "QLabel#header { color: ACCENT; font-size: 22px; font-weight: 700; }"
        "QLabel#subtitle { color: #a7afb8; font-size: 10px; font-weight: 600; }"
        "QLabel#value { color: #ffffff; font-weight: 600; }"
        "QLabel#keyValue { color: #ffffff; font-size: 15px; font-weight: 700; }"
        "QLabel#keyName { color: #a7afb8; font-size: 10px; }"
        "QWidget#line { border-bottom: 1px solid #262b31; }"
        "QComboBox { color: #e8eaed; background: #22272c; border: none; border-radius: 7px; "
        "padding: 4px 9px; font-size: 12px; }"
        "QComboBox::drop-down { border: none; width: 16px; }"
        "QComboBox QAbstractItemView { color: #e8eaed; background: #1c2126; border: 1px solid "
        "#2c3238; selection-background-color: #343b43; outline: none; }"
        "QProgressBar { background: #2c3238; border: none; border-radius: 2px; "
        "max-height: 4px; min-height: 4px; }"
        "QProgressBar::chunk { background: ACCENT; border-radius: 2px; }"
        "QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; border: none; }"
        "QScrollBar:vertical { background: transparent; width: 6px; margin: 0; }"
        "QScrollBar::handle:vertical { background: #3a4148; border-radius: 3px; "
        "min-height: 20px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QToolButton { color: #a7afb8; background: transparent; border: none; font-size: 15px; "
        "padding: 0 4px; }"
        "QToolButton:hover { color: #ffffff; }"
        "QToolButton#tab { color: #c9ced4; font-size: 12px; font-weight: 600; "
        "border-radius: 8px; padding: 0 10px; }"
        "QToolButton#tab:checked { color: #15181c; background: ACCENT; }"
        "QToolButton#star { color: #5a636d; font-size: 14px; padding: 0; }"
        "QToolButton#star:checked { color: ACCENT; }"
        "QToolButton#star:hover { color: #ffffff; }");
    setStyleSheet(style.replace(QStringLiteral("ACCENT"), accent));
}

void AdventureNotes::SaveSettings() {
    if (!collapsed) {
        sizes[strip ? 1 : 0] = size();
    }
    QSettings settings = PanelSettings();
    settings.setValue(QStringLiteral("strip"), strip);
    settings.setValue(QStringLiteral("rail_size"), sizes[0]);
    settings.setValue(QStringLiteral("strip_size"), sizes[1]);
}

void AdventureNotes::ApplyLayout() {
    // The same widgets, placed again: a layout only decides where the tabs and tools sit.
    rail_layout->removeWidget(tab_box);
    rail_layout->removeWidget(tools);
    column->removeWidget(tab_box);
    top->removeWidget(key_box);
    top->removeWidget(tools);
    if (strip) {
        tab_layout->setDirection(QBoxLayout::LeftToRight);
        tools_layout->setDirection(QBoxLayout::LeftToRight);
        // A row of its own: games can have many tabs, and their names must fit.
        column->insertWidget(1, tab_box);
        top->addWidget(key_box, 1, Qt::AlignRight | Qt::AlignVCenter);
        top->addWidget(tools, 0, Qt::AlignTop);
    } else {
        tab_layout->setDirection(QBoxLayout::TopToBottom);
        tools_layout->setDirection(QBoxLayout::TopToBottom);
        rail_layout->insertWidget(0, tab_box);
        rail_layout->addWidget(tools, 0, Qt::AlignHCenter);
    }
    rail->setVisible(!strip);
    setMinimumWidth(strip ? 460 : 230);
    BuildTabs();
    BuildKeys();
}

void AdventureNotes::SwitchLayout() {
    if (collapsed) {
        ToggleFold();
    }
    sizes[strip ? 1 : 0] = size();
    strip = !strip;
    ApplyLayout();
    resize(sizes[strip ? 1 : 0]);
    Refresh();
    update();
    SaveSettings();
}

void AdventureNotes::ToggleFold() {
    collapsed = !collapsed;
    collapse->setText(collapsed ? QStringLiteral("+") : QStringLiteral("–"));
    if (collapsed) {
        open_size = size();
    }
    const bool visible = !collapsed;
    views->setVisible(visible && views->count() > 1);
    scroll->setVisible(visible);
    grip->setVisible(visible);
    create->setVisible(visible && !script && create->isEnabled());
    if (strip) {
        tab_box->setVisible(visible);
        if (collapsed) {
            // What is left once the rest is hidden: the title and the key numbers.
            column->invalidate();
            column->activate();
            layout()->invalidate();
            layout()->activate();
            resize(width(), layout()->sizeHint().height());
        }
    } else {
        // Folded, only the rail stays.
        content->setVisible(visible);
        setMinimumWidth(collapsed ? RailWidth : 230);
        if (collapsed) {
            resize(RailWidth, height());
        }
    }
    if (!collapsed) {
        resize(open_size);
    }
    update();
}

void AdventureNotes::BuildTabs() {
    while (QLayoutItem* item = tab_layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (int index = 0; index < static_cast<int>(report.tabs.size()); ++index) {
        const QString& name = report.tabs[static_cast<size_t>(index)].name;
        auto* button = new QToolButton(tab_box);
        button->setObjectName(QStringLiteral("tab"));
        button->setCheckable(true);
        button->setToolTip(name);
        button->setText(strip ? name : name.left(1));
        if (strip) {
            button->setFixedHeight(28);
            button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
        } else {
            button->setFixedSize(34, 34);
        }
        connect(button, &QToolButton::clicked, this, [this, index] { SelectTab(index); });
        tab_layout->addWidget(button);
    }
    if (strip) {
        tab_layout->addStretch(1);
        tab_layout->activate();
        setMinimumWidth(std::max(460, tab_layout->sizeHint().width() + 26));
    }
    MarkTabs();
}

void AdventureNotes::MarkTabs() {
    for (int index = 0; index < tab_layout->count(); ++index) {
        if (auto* button = qobject_cast<QToolButton*>(tab_layout->itemAt(index)->widget())) {
            button->setChecked(index == current_tab);
        }
    }
}

void AdventureNotes::BuildKeys() {
    while (QLayoutItem* item = key_layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (const NotesRow& key : report.keys) {
        auto* box = new QWidget(key_box);
        auto* lines = new QVBoxLayout(box);
        lines->setContentsMargins(0, 0, 0, 0);
        lines->setSpacing(0);
        auto* value = new QLabel(key.value, box);
        value->setObjectName(QStringLiteral("keyValue"));
        value->setAlignment(Qt::AlignRight);
        auto* name = new QLabel(key.title, box);
        name->setObjectName(QStringLiteral("keyName"));
        name->setAlignment(Qt::AlignRight);
        lines->addWidget(value);
        lines->addWidget(name);
        key_layout->addWidget(box);
    }
    key_box->setVisible(strip && !report.keys.empty());
}

void AdventureNotes::SelectTab(int index) {
    if (collapsed) {
        ToggleFold();
    }
    current_tab = index;
    tab_name = report.tabs[static_cast<size_t>(index)].name;
    MarkTabs();
    FillViews();
}

void AdventureNotes::LoadGame() {
    const fs::path path = FindModule(program_id);
    std::error_code ec;
    module_time = fs::last_write_time(path, ec);
    save_time = {};
    report = {};
    ClearTabs();
    create->setEnabled(!fs::exists(path, ec));
    create->setVisible(create->isEnabled() && !collapsed);
    script = std::make_unique<NotesScript>();
    QString error;
    if (!script->Load(QString::fromStdString(path.string()), LibraryDirs(), error)) {
        script.reset();
        SetAccent(DefaultAccent);
        ShowMessage(error.isEmpty() ? tr("No module for this game yet") : error);
        return;
    }
    SetAccent(script->Accent());
    LoadFavorites();
    ReadSave();
}

void AdventureNotes::LoadFavorites() {
    favorites.clear();
    QFile file{QString::fromStdString(FavoritesFile(program_id).string())};
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    for (const QJsonValue& value : QJsonDocument::fromJson(file.readAll()).array()) {
        if (value.isString() && !favorites.contains(value.toString())) {
            favorites.append(value.toString());
        }
    }
}

void AdventureNotes::ToggleFavorite(const QString& key) {
    if (!favorites.removeOne(key)) {
        favorites.append(key);
    }
    std::error_code ec;
    const fs::path path = FavoritesFile(program_id);
    fs::create_directories(path.parent_path(), ec);
    QFile file{QString::fromStdString(path.string())};
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(QJsonArray::fromStringList(favorites)).toJson());
    }
    // The module's report is read again: the Favorites tab and the stars follow.
    ReadSave();
}

void AdventureNotes::AddFavoritesTab() {
    // The starred lines of every tab, with their current values, as one more tab after Global.
    NotesView view{QStringLiteral("Favorites"), {}};
    favorite_rows.clear();
    for (const QString& key : favorites) {
        const QStringList parts = SplitFavoriteKey(key);
        if (parts.size() != 3) {
            continue;
        }
        const QString& title = parts[2];
        for (const NotesTab& tab : report.tabs) {
            if (tab.name != parts[0]) {
                continue;
            }
            for (const NotesView& source : tab.views) {
                if (source.name != parts[1]) {
                    continue;
                }
                for (const NotesRow& line : source.rows) {
                    if (line.title == title) {
                        NotesRow copy = line;
                        if (source.name != tab.name) {
                            copy.title = QStringLiteral("%1 · %2").arg(source.name, line.title);
                        }
                        view.rows.push_back(copy);
                        favorite_rows.append(key);
                        break;
                    }
                }
            }
        }
    }
    if (view.rows.empty()) {
        view.rows.push_back({tr("Click ☆ beside any line to keep it here"), {}, -1, -1, 0});
        favorite_rows.append(QString{});
    }
    const auto at = !report.tabs.empty() && report.tabs.front().name == QStringLiteral("Global")
                        ? report.tabs.begin() + 1
                        : report.tabs.begin();
    report.tabs.insert(at, NotesTab{FavoritesTab.toString(), {std::move(view)}});
}

void AdventureNotes::ReadSave() {
    const std::string folder = script->Folder().toStdString();
    const auto path = NewestSave(folder, script->File().toStdString());
    if (!path) {
        report = {};
        ClearTabs();
        const QString files = SaveFiles(folder);
        ShowMessage(files.isEmpty()
                        ? tr("No save found in %1").arg(script->Folder())
                        : tr("No %1 in this game's save. It holds: %2").arg(script->File(), files));
        return;
    }
    std::error_code ec;
    save_time = fs::last_write_time(*path, ec);
    QString error;
    auto result = script->Read(QString::fromStdString(path->string()), error);
    if (!result) {
        report = {};
        ClearTabs();
        ShowMessage(error);
        return;
    }
    // A new save, or a reload, keeps the page in view.
    const QString tab = tab_name;
    const QString view = view_name;
    report = std::move(*result);
    AddFavoritesTab();
    slot = QString::fromStdString(path->parent_path().filename().string());
    current_tab = 0;
    for (size_t index = 0; index < report.tabs.size(); ++index) {
        if (report.tabs[index].name == tab) {
            current_tab = static_cast<int>(index);
        }
    }
    BuildTabs();
    BuildKeys();
    FillViews();
    if (const int index = views->findText(view); index > 0) {
        views->setCurrentIndex(index);
    }
    if (static_cast<size_t>(current_tab) < report.tabs.size()) {
        tab_name = report.tabs[static_cast<size_t>(current_tab)].name;
    }
}

void AdventureNotes::ClearTabs() {
    current_tab = 0;
    BuildTabs();
    BuildKeys();
    const QSignalBlocker blocker{views};
    views->clear();
    views->setVisible(false);
}

void AdventureNotes::CheckFiles() {
    // Only what changed is shown again: the list keeps its scroll position otherwise.
    std::error_code ec;
    const auto time = fs::last_write_time(FindModule(program_id), ec);
    if (!ec && time != module_time) {
        LoadGame();
        return;
    }
    if (!script) {
        return;
    }
    const auto path = NewestSave(script->Folder().toStdString(), script->File().toStdString());
    if (path) {
        const auto saved = fs::last_write_time(*path, ec);
        if (!ec && saved != save_time) {
            ReadSave();
            return;
        }
    }
}

void AdventureNotes::TickModule() {
    // Paused time is not counted: tick() is not called while the emulator is paused.
    if (!script || !QtCommon::system || QtCommon::system->IsPaused()) {
        return;
    }
    QString error;
    if (script->Tick(error)) {
        tick_error.clear();
        ReadSave();
    } else if (!error.isEmpty()) {
        // Shown once in the panel, like a read() error, rather than ten times a second.
        if (error != tick_error) {
            tick_error = error;
            LOG_WARNING(Frontend, "Adventure Notes tick: {}", error.toStdString());
            ShowMessage(error);
        }
    } else {
        tick_error.clear();
    }
}

void AdventureNotes::ClearRows() {
    // A QGridLayout keeps its rows once they exist, and with them the height of the longest view.
    while (QLayoutItem* item = rows->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    delete rows;
    rows = new QGridLayout(body);
    rows->setContentsMargins(0, 2, 8, 2);
    rows->setHorizontalSpacing(26);
    rows->setVerticalSpacing(0);
}

void AdventureNotes::ShowMessage(const QString& text) {
    ClearRows();
    subtitle->setText(QStringLiteral("ADVENTURE NOTES"));
    header->setText(QString{});
    auto* label = new QLabel(text, body);
    label->setWordWrap(true);
    rows->addWidget(label, 0, 0);
    rows->setRowStretch(1, 1);
}

void AdventureNotes::CreateModule() {
    const fs::path path = UserModule(program_id);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream file{path};
    file << QString::fromLatin1(ModuleTemplate)
                .arg(QString::fromStdString(fmt::format("{:016X}", program_id)))
                .toStdString();
    file.close();
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QString::fromStdString(path.parent_path().string())));
    LoadGame();
}

void AdventureNotes::FillViews() {
    QStringList names;
    if (static_cast<size_t>(current_tab) < report.tabs.size()) {
        for (const NotesView& view : report.tabs[static_cast<size_t>(current_tab)].views) {
            names.append(view.name);
        }
    }
    // A module that redraws every second (timers) would otherwise clear the list each time,
    // closing it or losing the choice while it is open: it is rebuilt only when the views change.
    QStringList shown;
    for (int index = 0; index < views->count(); ++index) {
        shown.append(views->itemText(index));
    }
    if (names != shown) {
        const QSignalBlocker blocker{views};
        views->clear();
        views->addItems(names);
    }
    views->setVisible(!collapsed && views->count() > 1);
    Refresh();
}

QWidget* AdventureNotes::MakeRow(const NotesRow& line, std::vector<QLabel*>& values,
                                 const QString& key) {
    auto* row = new QWidget(body);
    row->setObjectName(QStringLiteral("line"));
    row->setAttribute(Qt::WA_StyledBackground);
    row->setMinimumHeight(30);
    auto* lines = new QVBoxLayout(row);
    lines->setContentsMargins(0, 5, 0, 5);
    lines->setSpacing(4);
    auto* text_row = new QHBoxLayout;
    text_row->setContentsMargins(0, 0, 0, 0);
    text_row->setSpacing(10);
    lines->addLayout(text_row);

    if (!key.isEmpty()) {
        auto* star = new QToolButton(row);
        star->setObjectName(QStringLiteral("star"));
        star->setCheckable(true);
        star->setChecked(favorites.contains(key));
        star->setText(star->isChecked() ? QStringLiteral("★") : QStringLiteral("☆"));
        star->setToolTip(star->isChecked() ? tr("Remove from Favorites") : tr("Add to Favorites"));
        star->setFixedWidth(18);
        // Queued: the redraw deletes this button, which must not happen inside its own click.
        connect(
            star, &QToolButton::clicked, this, [this, key] { ToggleFavorite(key); },
            Qt::QueuedConnection);
        text_row->addWidget(star);
    }
    auto* name = new QLabel(line.title, row);
    name->setWordWrap(true);
    text_row->addWidget(name, 1);

    QString shown = line.value;
    if (line.done >= 0) {
        shown = QStringLiteral("%1 / %2").arg(line.done).arg(line.total);
        auto* bar = new QProgressBar(row);
        bar->setRange(0, std::max(line.total, 1));
        bar->setValue(line.done);
        bar->setTextVisible(false);
        // A narrow list has no room for it beside the name.
        if (strip) {
            bar->setFixedWidth(54);
            text_row->addWidget(bar);
        } else {
            lines->addWidget(bar);
        }
    } else if (line.check >= 0) {
        shown = line.check != 0
                    ? QStringLiteral("<span style='color:%1'>✓</span>").arg(accent)
                    : QStringLiteral("<span style='color:#a7afb8'>—</span>");
    }
    auto* value = new QLabel(shown, row);
    value->setObjectName(QStringLiteral("value"));
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    text_row->addWidget(value);
    values.push_back(value);
    return row;
}

void AdventureNotes::Refresh() {
    const int view = views->currentIndex();
    if (view < 0 || !script || static_cast<size_t>(current_tab) >= report.tabs.size()) {
        return;
    }
    view_name = views->currentText();
    ClearRows();
    subtitle->setText(
        QStringLiteral("%1  ·  %2").arg(script->Title().toUpper().left(26)).arg(slot));
    header->setText(report.title);
    const int columns = strip ? 2 : 1;
    int count = 0;
    std::vector<QLabel*> values;
    const NotesTab& tab = report.tabs[static_cast<size_t>(current_tab)];
    const bool favorite_tab = tab.name == FavoritesTab;
    for (const NotesRow& line : tab.views[static_cast<size_t>(view)].rows) {
        // A line is known by its tab, view and title; the Favorites tab keeps its source's key.
        const QString key =
            favorite_tab ? favorite_rows.value(count)
                         : FavoriteKey(tab.name, view_name, line.title);
        rows->addWidget(MakeRow(line, values, key), count / columns, count % columns,
                        Qt::AlignTop);
        ++count;
    }
    // One width for every value of the view: numbers and bars line up in columns.
    int widest = 0;
    for (QLabel* value : values) {
        value->ensurePolished();
        widest = std::max(widest, value->sizeHint().width());
    }
    for (QLabel* value : values) {
        value->setFixedWidth(widest);
    }
    for (int track = 0; track < columns; ++track) {
        rows->setColumnStretch(track, 1);
    }
    // The spare height goes below the list: lines keep their spacing whatever the window's size.
    rows->setRowStretch((count + columns - 1) / columns, 1);
}

void AdventureNotes::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath outline;
    outline.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12.0, 12.0);
    painter.fillPath(outline, QColor{21, 24, 28, 238});
    if (!strip) {
        painter.save();
        painter.setClipPath(outline);
        painter.fillRect(QRect{0, 0, RailWidth, height()}, QColor{15, 18, 21, 245});
        painter.restore();
    }
    painter.setPen(QPen{QColor{44, 50, 56}, 1.0});
    painter.drawPath(outline);
}

void AdventureNotes::StartDrag(QPoint position) {
    // The compositor moves the window where it can: Wayland ignores move() on top-level windows.
    if (windowHandle() && windowHandle()->startSystemMove()) {
        return;
    }
    dragging = true;
    drag_start = position;
}

void AdventureNotes::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        StartDrag(event->pos());
    }
}

void AdventureNotes::mouseMoveEvent(QMouseEvent* event) {
    if (dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - drag_start);
    }
}

bool AdventureNotes::eventFilter(QObject* object, QEvent* event) {
    if (object == grip && event->type() == QEvent::MouseButtonPress) {
        if (windowHandle()) {
            windowHandle()->startSystemResize(Qt::RightEdge | Qt::BottomEdge);
        }
        return true;
    }
    if ((object == header || object == subtitle) && event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            StartDrag(static_cast<QWidget*>(object)->mapTo(this, mouse->pos()));
            return true;
        }
    }
    return QWidget::eventFilter(object, event);
}

void AdventureNotes::closeEvent(QCloseEvent*) {
    emit closed();
}
