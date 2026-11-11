// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "qt_common/util/open.h"

#include <QDesktopServices>
#include <QDir>

#include <optional>

#ifdef __unix__
#include <QProcess>
#include <QProcessEnvironment>
#endif

namespace QtCommon {

#ifdef __unix__

namespace {

/// Unescapes one systemd shell-escaped environment value, i.e. the $'...'
/// ANSI-C quoting used by `systemctl show-environment`. Plain values pass
/// through unchanged so the comparison below also works where no shell is
/// involved. Escape bytes (\xHH, octal) are collected as raw bytes and
/// decoded together as UTF-8 at the end, so a multi-byte sequence split
/// across escapes keeps its intended code points.
QString UnescapeSystemdValue(const QString& value) {
    static const QString prefix = QStringLiteral("$'");
    if (value.size() < 4 || !value.startsWith(prefix) ||
        !value.endsWith(QLatin1Char('\''))) {
        return value;
    }
    const QString inner = value.mid(2, value.size() - 3);
    auto hex_value = [](QChar c) -> int {
        const uint u = c.unicode();
        if (u >= '0' && u <= '9') {
            return int(u - '0');
        }
        if (u >= 'a' && u <= 'f') {
            return int(u - 'a' + 10);
        }
        if (u >= 'A' && u <= 'F') {
            return int(u - 'A' + 10);
        }
        return -1;
    };
    QByteArray bytes;
    bytes.reserve(inner.size());
    const auto append_code_point = [&](uint v) {
        if (v < 0x80) {
            bytes.append(char(v));
        } else if (v < 0x800) {
            bytes.append(char(0xC0 | (v >> 6)));
            bytes.append(char(0x80 | (v & 0x3F)));
        } else if (v < 0x10000) {
            bytes.append(char(0xE0 | (v >> 12)));
            bytes.append(char(0x80 | ((v >> 6) & 0x3F)));
            bytes.append(char(0x80 | (v & 0x3F)));
        } else if (v <= 0x10FFFF) {
            bytes.append(char(0xF0 | (v >> 18)));
            bytes.append(char(0x80 | ((v >> 12) & 0x3F)));
            bytes.append(char(0x80 | ((v >> 6) & 0x3F)));
            bytes.append(char(0x80 | (v & 0x3F)));
        }
    };
    const auto append_byte = [&](uint b) { bytes.append(char(b & 0xFF)); };
    for (qsizetype i = 0; i < inner.size();) {
        const QChar c = inner[i];
        if (c != QLatin1Char('\\') || i + 1 >= inner.size()) {
            bytes.append(QString(c).toUtf8());
            ++i;
            continue;
        }
        const QChar e = inner[i + 1];
        const auto take_byte = [&](char b) {
            bytes.append(b);
            i += 2;
        };
        // A lone backslash is kept literally; parsing resumes after it.
        const auto take_literally = [&]() {
            bytes.append('\\');
            ++i;
        };
        switch (e.unicode()) {
        case 'a':
            take_byte('\a');
            break;
        case 'b':
            take_byte('\b');
            break;
        case 'f':
            take_byte('\f');
            break;
        case 'n':
            take_byte('\n');
            break;
        case 'r':
            take_byte('\r');
            break;
        case 't':
            take_byte('\t');
            break;
        case 'v':
            take_byte('\v');
            break;
        case 's':
            take_byte(' ');
            break;
        case '\\':
            take_byte('\\');
            break;
        case '\'':
            take_byte('\'');
            break;
        case '"':
            take_byte('"');
            break;
        case 'x': {
            const int hi = (i + 2 < inner.size()) ? hex_value(inner[i + 2]) : -1;
            const int lo = (i + 3 < inner.size()) ? hex_value(inner[i + 3]) : -1;
            if (hi >= 0 && lo >= 0) {
                append_byte(uint(hi * 16 + lo));
                i += 4;
            } else {
                take_literally();
            }
            break;
        }
        case 'u':
        case 'U': {
            const int digits = (e.unicode() == 'u') ? 4 : 8;
            uint v = 0;
            bool ok = (i + 2 + digits <= inner.size());
            for (int k = 0; ok && k < digits; ++k) {
                const int h = hex_value(inner[i + 2 + k]);
                if (h < 0) {
                    ok = false;
                } else {
                    v = v * 16 + uint(h);
                }
            }
            // Lone surrogates are not scalar values; keep them literally.
            if (ok && v >= 0xD800 && v <= 0xDFFF) {
                ok = false;
            }
            if (ok) {
                append_code_point(v);
                i += 2 + digits;
            } else {
                take_literally();
            }
            break;
        }
        default: {
            // Octal \ooo (up to 3 digits); anything else passes through.
            uint v = 0;
            int k = 0;
            while (k < 3 && i + 1 + k < inner.size()) {
                const uint d = inner[i + 1 + k].unicode();
                if (d < '0' || d > '7') {
                    break;
                }
                v = v * 8 + (d - '0');
                ++k;
            }
            if (k == 0) {
                take_literally();
            } else if (v <= 0xFF) {
                append_byte(v);
                i += 1 + k;
            } else {
                // Not a byte; keep the previous code-point semantics.
                bytes.append(QString(QChar(char16_t(v))).toUtf8());
                i += 1 + k;
            }
            break;
        }
        }
    }
    return QString::fromUtf8(bytes);
}

/// Queries the systemd user manager for its session-wide XDG_CONFIG_HOME.
/// Returns nullopt when the manager is unreachable (non-systemd session,
/// sandbox, query failure). The result is cached: the manager environment
/// does not change for the lifetime of this process, so the subprocess
/// cost is paid at most once (a fast local D-Bus call, ~20ms).
std::optional<QString> SessionConfigHome() {
    static const std::optional<QString> cached = []() -> std::optional<QString> {
        QProcess proc;
        proc.setProgram(QStringLiteral("systemctl"));
        proc.setArguments({QStringLiteral("--user"), QStringLiteral("show-environment")});
        proc.start();
        if (!proc.waitForFinished(2000) || proc.exitCode() != 0) {
            return std::nullopt;
        }
        static const QString prefix = QStringLiteral("XDG_CONFIG_HOME=");
        const QString output = QString::fromLocal8Bit(proc.readAllStandardOutput());
        for (const QString& line : output.split(QLatin1Char('\n'))) {
            if (line.startsWith(prefix)) {
                return UnescapeSystemdValue(line.mid(prefix.size()));
            }
        }
        return QString{};
    }();
    return cached;
}

/// Builds the environment for spawned external handlers (browsers, file
/// managers). Starts from the current process environment and drops
/// variables that must not leak into programs outside the emulator.
QProcessEnvironment CleanedEnvironmentForExternalProcess() {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    // External handlers are not part of the emulator, so never hand them
    // a config home that was overridden for this process (e.g. per-source
    // homes set by launchers). A file manager started with a foreign
    // config home reads the wrong dolphinrc without the user's kdeglobals
    // and falls back to a broken default theme. A value matching the
    // session manager's is the user's own and is preserved; with no
    // manager to compare against, anything but the default (~/.config)
    // is treated as an override. Session default applies when unset.
    const QString config_home = env.value(QStringLiteral("XDG_CONFIG_HOME"));
    if (!config_home.isEmpty()) {
        const std::optional<QString> session_home = SessionConfigHome();
        if (!session_home.has_value()) {
            if (config_home != QDir::homePath() + QStringLiteral("/.config")) {
                env.remove(QStringLiteral("XDG_CONFIG_HOME"));
            }
        } else if (config_home != session_home.value()) {
            env.remove(QStringLiteral("XDG_CONFIG_HOME"));
        }
    }

    return env;
}

/// Opens a local path or URL string with the host default handler via
/// xdg-open. Returns true when the handler process was started.
bool OpenWithXdgOpen(const QString& target) {
    QProcess proc;
    proc.setProgram(QStringLiteral("xdg-open"));
    proc.setArguments({target});
    proc.setProcessEnvironment(CleanedEnvironmentForExternalProcess());
    proc.setWorkingDirectory(QDir::homePath());
    qint64 pid = 0;
    if (proc.startDetached(&pid)) {
        return true;
    }
    // Fallback if xdg-open cannot be started.
    return QDesktopServices::openUrl(QUrl::fromUserInput(target));
}

} // namespace

bool OpenUrl(const QUrl& url) {
    if (url.isLocalFile()) {
        return OpenWithXdgOpen(url.toLocalFile());
    }
    return OpenWithXdgOpen(url.toString(QUrl::FullyEncoded));
}

bool OpenLocalFolder(const QString& path) {
    return OpenWithXdgOpen(path);
}

bool OpenLocalFolder(const std::filesystem::path& path) {
    return OpenWithXdgOpen(
        QString::fromStdString(std::filesystem::path(path).string()));
}

#else

bool OpenUrl(const QUrl& url) {
    return QDesktopServices::openUrl(url);
}

bool OpenLocalFolder(const QString& path) {
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

bool OpenLocalFolder(const std::filesystem::path& path) {
    return QDesktopServices::openUrl(
        QUrl::fromLocalFile(QString::fromStdString(std::filesystem::path(path).string())));
}

#endif

} // namespace QtCommon
