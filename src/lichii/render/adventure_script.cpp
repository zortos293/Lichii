// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <utility>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QJSValue>
#include <QSaveFile>
#include <zlib.h>

#include "core/core.h"
#include "core/debugger/nx2_debug_tools.h"
#include "core/hle/kernel/k_process.h"
#include "qt_common/qt_common.h"
#include "render/adventure_script.h"

namespace {
/// A module that never returns must not hold the emulator's window.
class Watchdog {
public:
    explicit Watchdog(QJSEngine& engine) : thread{[this, &engine] {
        std::unique_lock lock{mutex};
        if (!done.wait_for(lock, std::chrono::seconds{5}, [this] { return finished; })) {
            engine.setInterrupted(true);
        }
    }} {}

    ~Watchdog() {
        {
            std::scoped_lock lock{mutex};
            finished = true;
        }
        done.notify_all();
        thread.join();
    }

private:
    std::mutex mutex;
    std::condition_variable done;
    bool finished = false;
    std::thread thread;
};

QString Describe(const QJSValue& value) {
    const QString name = QFileInfo{value.property(QStringLiteral("fileName")).toString()}.fileName();
    return QStringLiteral("%1, line %2: %3")
        .arg(name)
        .arg(value.property(QStringLiteral("lineNumber")).toInt())
        .arg(value.property(QStringLiteral("message")).toString());
}

/// A plain name: a module cannot climb out of the folders it is given.
bool PlainName(const QString& name) {
    // No ':' either: on Windows "D:file" names a path on another drive.
    return !name.isEmpty() && name != QStringLiteral("..") && !name.contains(QLatin1Char('/')) &&
           !name.contains(QLatin1Char('\\')) && !name.contains(QLatin1Char(':'));
}

// A report is read by native loops that the watchdog cannot interrupt: its size is bounded here,
// whatever length the module claims (new Array(1e9)).
constexpr int MaxRows = 4096;
constexpr int MaxTabs = 64;
constexpr int MaxViews = 64;
constexpr int MaxTotalRows = 20000; ///< all the lines of a report together

int Length(const QJSValue& array, int limit) {
    return std::clamp(array.property(QStringLiteral("length")).toInt(), 0, limit);
}

NotesRow ParseRow(const QJSValue& object) {
    NotesRow row;
    row.title = object.property(QStringLiteral("title")).toString();
    const QJSValue value = object.property(QStringLiteral("value"));
    if (!value.isUndefined()) {
        row.value = value.toString();
    }
    const QJSValue check = object.property(QStringLiteral("check"));
    if (!check.isUndefined()) {
        row.check = check.toBool() ? 1 : 0;
    }
    const QJSValue done = object.property(QStringLiteral("done"));
    if (!done.isUndefined()) {
        row.done = done.toInt();
        row.total = object.property(QStringLiteral("total")).toInt();
    }
    return row;
}

NotesReport ParseReport(const QJSValue& result) {
    NotesReport report;
    report.title = result.property(QStringLiteral("title")).toString();
    int budget = MaxTotalRows;
    const QJSValue keys = result.property(QStringLiteral("keys"));
    const int key_count = Length(keys, std::min(MaxRows, budget));
    for (int k = 0; k < key_count; ++k) {
        report.keys.push_back(ParseRow(keys.property(static_cast<quint32>(k))));
    }
    budget -= key_count;
    const QJSValue tabs = result.property(QStringLiteral("tabs"));
    const int tab_count = Length(tabs, MaxTabs);
    for (int t = 0; t < tab_count; ++t) {
        const QJSValue tab_object = tabs.property(static_cast<quint32>(t));
        NotesTab tab{tab_object.property(QStringLiteral("name")).toString(), {}};
        const QJSValue views = tab_object.property(QStringLiteral("views"));
        const int view_count = Length(views, MaxViews);
        for (int v = 0; v < view_count; ++v) {
            const QJSValue view_object = views.property(static_cast<quint32>(v));
            NotesView view{view_object.property(QStringLiteral("name")).toString(), {}};
            const QJSValue rows = view_object.property(QStringLiteral("rows"));
            const int row_count = Length(rows, std::min(MaxRows, budget));
            for (int r = 0; r < row_count; ++r) {
                view.rows.push_back(ParseRow(rows.property(static_cast<quint32>(r))));
            }
            budget -= row_count;
            tab.views.push_back(std::move(view));
        }
        report.tabs.push_back(std::move(tab));
    }
    return report;
}

// Plain functions in front of the host: a script sees these, not the QObject and its methods.
constexpr const char* Bootstrap = R"((function (host) {
    return {
        unzip: (data, offset, end) =>
            host.unzip(data, offset === undefined ? 0 : offset, end === undefined ? -1 : end),
        utf8: (data, offset, size) =>
            host.utf8(data, offset === undefined ? 0 : offset, size === undefined ? -1 : size),
        file: (name) => host.file(name),
        include: (name) => host.include(name),
        memory: (address, size) => host.memory(String(address), size),
        search: (pattern) => host.search(pattern),
        load: (name) => host.load(name),
        store: (name, text) => host.store(name, String(text)),
    };
}))";
/// The running game while `lock` (from LockGuestMemoryReaders) is held, or null while none runs
/// or it is stopping. Its memory stays valid until the lock is released.
Kernel::KProcess* RunningGame(std::shared_lock<std::shared_mutex>& lock) {
    Core::System* const system = QtCommon::system.get();
    if (system == nullptr) {
        return nullptr;
    }
    lock = system->LockGuestMemoryReaders();
    if (system->IsShuttingDown()) {
        lock.unlock();
        return nullptr;
    }
    return system->ApplicationProcess();
}

/// The heap addresses where pattern starts, at most 64. Scans in slices so that a stopping
/// panel or game ends it within one slice.
std::vector<u64> SearchHeap(const std::vector<u8>& pattern, const std::atomic<bool>& stopping) {
    std::vector<u64> hits;
    u64 start = 0;
    u64 end = 0;
    {
        std::shared_lock<std::shared_mutex> lock;
        Kernel::KProcess* const process = RunningGame(lock);
        if (process == nullptr || pattern.empty()) {
            return hits;
        }
        start = GetInteger(process->GetPageTable().GetHeapRegionStart());
        end = start + process->GetPageTable().GetHeapRegionSize();
    }
    // Small slices: a stopping game or panel ends the search within a few milliseconds.
    constexpr u64 Slice = u64{16} << 20;
    for (u64 at = start; at < end && hits.size() < 64; at += Slice) {
        // Each slice holds the readers' lock, so the process is not torn down under it.
        std::shared_lock<std::shared_mutex> lock;
        Kernel::KProcess* const process = RunningGame(lock);
        if (stopping.load(std::memory_order_relaxed) || process == nullptr) {
            break;
        }
        const u64 slice_end = (std::min)(end, at + Slice);
        // A match may start before slice_end and end after it.
        const u64 read_end = (std::min)(end, slice_end + pattern.size() - 1);
        const auto found = Core::Nx2Debug::SearchGuestMemory(
            process->GetMemory(), pattern, at, read_end, 64,
            std::chrono::steady_clock::now() + std::chrono::seconds{10});
        for (const u64 hit : found.hits) {
            if (hit < slice_end && hits.size() < 64) {
                hits.push_back(hit);
            }
        }
    }
    return hits;
}
} // Anonymous namespace

NotesHost::NotesHost(QJSEngine* engine_, QStringList library_dirs)
    : engine{engine_}, libraries{std::move(library_dirs)} {}

NotesHost::~NotesHost() {
    stopping = true;
    if (search_job.valid()) {
        search_job.wait();
    }
}

QJSValue NotesHost::memory(const QString& address, int size) {
    bool ok = false;
    const u64 start = QString{address}.remove(QStringLiteral("0x")).toULongLong(&ok, 16);
    if (!ok || size <= 0 || size > 4096) {
        engine->throwError(QStringLiteral("memory: want a hex address and a size up to 4096"));
        return {};
    }
    std::shared_lock<std::shared_mutex> lock;
    Kernel::KProcess* const process = RunningGame(lock);
    if (process == nullptr) {
        return QJSValue{QJSValue::NullValue};
    }
    QByteArray bytes(size, '\0');
    if (Core::Nx2Debug::ReadGuestMemory(
            process->GetMemory(), start,
            std::span<u8>{reinterpret_cast<u8*>(bytes.data()), static_cast<size_t>(size)})) {
        return QJSValue{QJSValue::NullValue};
    }
    return engine->toScriptValue(bytes);
}

QJSValue NotesHost::search(const QString& pattern) {
    if (search_job.valid()) {
        if (search_job.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            return QJSValue{QJSValue::NullValue};
        }
        const std::vector<u64> hits = search_job.get();
        if (pattern != search_pattern) {
            // A finished search for another pattern: drop it and start this one.
            return search(pattern);
        }
        QJSValue result = engine->newArray(static_cast<uint>(hits.size()));
        for (size_t i = 0; i < hits.size(); ++i) {
            result.setProperty(static_cast<quint32>(i),
                               QStringLiteral("0x%1").arg(hits[i], 0, 16));
        }
        return result;
    }
    const QByteArray bytes = QByteArray::fromHex(pattern.toLatin1());
    if (bytes.isEmpty() || bytes.size() > 256) {
        engine->throwError(QStringLiteral("search: want 1 to 256 bytes in hex"));
        return {};
    }
    search_pattern = pattern;
    std::vector<u8> wanted(bytes.begin(), bytes.end());
    search_job = std::async(std::launch::async, [this, wanted = std::move(wanted)] {
        return SearchHeap(wanted, stopping);
    });
    return QJSValue{QJSValue::NullValue};
}

QString NotesHost::StatePath(const QString& name) const {
    if (!PlainName(name) || libraries.isEmpty()) {
        return {};
    }
    // libraries[0] is notes/lib: the state sits next to it. Built from the path, not by cdUp():
    // notes/ may not exist yet, and cdUp() would then stay in notes/lib.
    const QString state = QDir::cleanPath(libraries.front() + QStringLiteral("/../state"));
    QDir{}.mkpath(state);
    return QDir{state}.filePath(name);
}

QString NotesHost::load(const QString& name) {
    QFile input{StatePath(name)};
    if (!input.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(input.readAll());
}

void NotesHost::store(const QString& name, const QString& text) {
    const QString path = StatePath(name);
    if (path.isEmpty()) {
        engine->throwError(QStringLiteral("store: want a plain file name"));
        return;
    }
    QSaveFile output{path};
    if (output.open(QIODevice::WriteOnly)) {
        output.write(text.toUtf8());
        static_cast<void>(output.commit());
    }
}

void NotesHost::SetSlot(const QString& directory) {
    slot = directory;
}

QByteArray NotesHost::unzip(const QByteArray& data, int offset, int end) {
    const qsizetype stop = end < 0 ? data.size() : std::min<qsizetype>(end, data.size());
    if (offset < 0 || offset >= stop) {
        engine->throwError(QStringLiteral("unzip: offset outside the data"));
        return {};
    }
    // Inflated in steps with a ceiling: a stream that expands without end (a crafted save) stops
    // at the limit, or when the watchdog interrupts the module, instead of filling the memory.
    constexpr qsizetype Limit = qsizetype{256} << 20;
    constexpr qsizetype Step = qsizetype{1} << 20;
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) {
        engine->throwError(QStringLiteral("unzip: not a zlib stream"));
        return {};
    }
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.constData() + offset));
    stream.avail_in = static_cast<uInt>(stop - offset);
    QByteArray result;
    int status = Z_OK;
    while (status == Z_OK) {
        if (result.size() >= Limit || engine->isInterrupted()) {
            inflateEnd(&stream);
            engine->throwError(QStringLiteral("unzip: the data expands past 256 MiB"));
            return {};
        }
        const qsizetype used = result.size();
        result.resize(used + Step);
        stream.next_out = reinterpret_cast<Bytef*>(result.data() + used);
        stream.avail_out = static_cast<uInt>(Step);
        status = inflate(&stream, Z_NO_FLUSH);
        result.resize(used + Step - static_cast<qsizetype>(stream.avail_out));
        if (status == Z_BUF_ERROR && stream.avail_in == 0) {
            break; // The input ends before the stream does: keep what it held.
        }
    }
    inflateEnd(&stream);
    if ((status != Z_STREAM_END && status != Z_BUF_ERROR) || result.isEmpty()) {
        engine->throwError(QStringLiteral("unzip: not a zlib stream"));
        return {};
    }
    return result;
}

QString NotesHost::utf8(const QByteArray& data, int offset, int size) {
    if (offset < 0 || offset >= data.size()) {
        return {};
    }
    const qsizetype room = data.size() - offset;
    const qsizetype limit = size < 0 ? room : std::min<qsizetype>(size, room);
    const char* text = data.constData() + offset;
    return QString::fromUtf8(text, std::find(text, text + limit, '\0') - text);
}

QByteArray NotesHost::file(const QString& name) {
    // Until a save is assigned, slot is empty and QDir{slot} would be the working directory.
    if (slot.isEmpty() || !PlainName(name)) {
        engine->throwError(QStringLiteral("file: no %1 in this save").arg(name));
        return {};
    }
    QFile input{QDir{slot}.filePath(name)};
    if (!input.open(QIODevice::ReadOnly)) {
        engine->throwError(QStringLiteral("file: no %1 in this save").arg(name));
        return {};
    }
    return input.readAll();
}

void NotesHost::include(const QString& name) {
    for (const QString& directory : libraries) {
        QFile input{QDir{directory}.filePath(name)};
        if (PlainName(name) && input.open(QIODevice::ReadOnly)) {
            const QJSValue result =
                engine->evaluate(QString::fromUtf8(input.readAll()), input.fileName());
            if (result.isError()) {
                engine->throwError(Describe(result));
            }
            return;
        }
    }
    engine->throwError(QStringLiteral("include: no library named %1").arg(name));
}

NotesScript::NotesScript() = default;
NotesScript::~NotesScript() = default;

bool NotesScript::Load(const QString& path, const QStringList& library_dirs, QString& error) {
    QFile input{path};
    if (!input.open(QIODevice::ReadOnly)) {
        return false;
    }
    host.reset();
    tick_stopped = false;
    engine = std::make_unique<QJSEngine>();
    host = std::make_unique<NotesHost>(engine.get(), library_dirs);
    QJSEngine::setObjectOwnership(host.get(), QJSEngine::CppOwnership);
    const QJSValue make = engine->evaluate(QString::fromLatin1(Bootstrap));
    engine->globalObject().setProperty(QStringLiteral("notes"),
                                       make.call({engine->newQObject(host.get())}));
    // Everything the module's code can run (its body, game's getters, the read lookup) stays
    // under the watchdog.
    bool game_error = false;
    bool has_read = false;
    QString load_error;
    {
        Watchdog watchdog{*engine};
        const QJSValue result = engine->evaluate(QString::fromUtf8(input.readAll()), path);
        if (result.isError()) {
            load_error = Describe(result);
        } else {
            const QJSValue game = engine->evaluate(QStringLiteral("game"));
            game_error = game.isError();
            title = game.property(QStringLiteral("title")).toString();
            accent = game.property(QStringLiteral("accent")).toString();
            folder = game.property(QStringLiteral("folder")).toString();
            file = game.property(QStringLiteral("file")).toString();
            has_read = engine->globalObject().property(QStringLiteral("read")).isCallable();
        }
    }
    const bool interrupted = engine->isInterrupted();
    engine->setInterrupted(false);
    if (interrupted) {
        error = QStringLiteral("The module was stopped: loading took over 5 seconds");
        return false;
    }
    if (!load_error.isEmpty()) {
        error = load_error;
        return false;
    }
    if (game_error || !has_read || folder.isEmpty() || file.isEmpty()) {
        error = QStringLiteral("A module needs game, with folder and file, and a read function");
        return false;
    }
    // Plain names only: a module reads its own game's save and nothing else.
    if (!PlainName(folder) || folder == QStringLiteral(".") || !PlainName(file) ||
        file == QStringLiteral(".")) {
        error = QStringLiteral("A module's folder and file must be plain names");
        return false;
    }
    // A module reads its own game's saves: notes/<program id>.js names folder <program id>.
    if (QFileInfo{path}.completeBaseName().compare(folder, Qt::CaseInsensitive) != 0) {
        error = QStringLiteral("A module's folder must be its game's program id");
        return false;
    }
    return true;
}

std::optional<NotesReport> NotesScript::Read(const QString& path, QString& error) {
    QFile input{path};
    if (!engine || !input.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Unreadable save");
        return std::nullopt;
    }
    const QFileInfo info{path};
    host->SetSlot(info.absolutePath());
    QJSValue save = engine->newObject();
    save.setProperty(QStringLiteral("data"), engine->toScriptValue(input.readAll()));
    save.setProperty(QStringLiteral("slot"), info.dir().dirName());
    // The report is parsed, and an error described, under the watchdog too: both run the
    // module's getters.
    std::optional<NotesReport> report;
    {
        Watchdog watchdog{*engine};
        const QJSValue result =
            engine->globalObject().property(QStringLiteral("read")).call({save});
        if (result.isError()) {
            error = Describe(result);
        } else {
            report = ParseReport(result);
        }
    }
    const bool interrupted = engine->isInterrupted();
    engine->setInterrupted(false);
    if (interrupted) {
        error = QStringLiteral("The module was stopped: read() took over 5 seconds");
        return std::nullopt;
    }
    return report;
}

bool NotesScript::Tick(QString& error) {
    if (!engine || tick_stopped) {
        return false;
    }
    // The lookup too: tick may be a getter.
    bool redraw = false;
    {
        Watchdog watchdog{*engine};
        const QJSValue tick = engine->globalObject().property(QStringLiteral("tick"));
        if (tick.isCallable()) {
            const QJSValue result = tick.call();
            if (result.isError()) {
                error = Describe(result);
            } else {
                redraw = result.toBool();
            }
        }
    }
    const bool interrupted = engine->isInterrupted();
    engine->setInterrupted(false);
    if (interrupted) {
        // Not called again until the module is loaded again: a tick that never ends would
        // otherwise block the interface for five seconds, ten times a second.
        tick_stopped = true;
        error = QStringLiteral("The module was stopped: tick() took over 5 seconds");
        return false;
    }
    return redraw;
}
