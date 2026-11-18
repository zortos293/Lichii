// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The front end's and the replay's log lines (dm.h LOG, VLOG, VRED, die) in Lichii's log, class
// Render.NvnDirect, with their text as upstream writes it ("[dm] ...").

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "common/logging.h"

extern "C" void dm_log(const char* file, int line, const char* func, const char* fmt, ...) {
    // Upstream's prefixes: "[dm] [RED] ..." marks what does not work as the game expects,
    // "[dm] FATAL ..." precedes an abort.
    const char* tag = fmt + (std::strncmp(fmt, "[dm] ", 5) == 0 ? 5 : 0);
    const auto level = std::strncmp(tag, "FATAL", 5) == 0   ? Common::Log::Level::Critical
                       : std::strncmp(tag, "[RED]", 5) == 0 ? Common::Log::Level::Error
                                                            : Common::Log::Level::Info;
    if (!Common::Log::IsLevelEnabled(Common::Log::Class::Render_NvnDirect, level))
        return;
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    va_list again;
    va_copy(again, args);
    const int length = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    std::string text;
    const char* message = buffer;
    if (length >= static_cast<int>(sizeof(buffer))) {
        text.resize(static_cast<std::size_t>(length));
        std::vsnprintf(text.data(), text.size() + 1, fmt, again);
        message = text.c_str();
    }
    va_end(again);
    Common::Log::FmtLogMessage(Common::Log::Class::Render_NvnDirect, level, file,
                               static_cast<unsigned>(line), func, "{}", message);
}
