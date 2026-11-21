// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The front end's and the replay's log lines (dm.h LOG, VLOG, VRED, die) in Lichii's log, class
// Render.NvnDirect, with their text as upstream writes it ("[dm] ...").

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
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

#include "common/settings.h"
#include "video_core/oot_network.h"

// Ocarina of Time's "No sharpening" anti-aliasing setting for the Direct NVN replay (vka_draw.c oot_bypass)
extern "C" int fritchii_oot_no_sharpening(void) {
    return Settings::GetCurrentProgramID() == VideoCommon::OotNetwork::ProgramId &&
           Settings::values.oot_anti_aliasing.GetValue() == Settings::OotAntiAliasing::NoSharpening;
}

// Ocarina of Time's movies: the bilinear copy in place of the game's movie network, opt-in with
// EDEN_NX2_OOT_MOVIE_NETWORK=0 (Lichii has no movie upscale setting; the game's network by default)
extern "C" int fritchii_oot_movie_bilinear(void) {
    if (Settings::GetCurrentProgramID() != VideoCommon::OotNetwork::ProgramId) return 0;
    const char* const env = std::getenv("EDEN_NX2_OOT_MOVIE_NETWORK");
    return env != nullptr && env[0] == '0';
}

// Ocarina of Time's fog: its readers' light colours as the game submitted them (vka_draw.c fog_bind);
// EDEN_NX2_OOT_FOG_SNAPSHOT=0 turns it off
extern "C" int fritchii_oot_fog_snapshot(void) {
    if (Settings::GetCurrentProgramID() != VideoCommon::OotNetwork::ProgramId) return 0;
    const char* const env = std::getenv("EDEN_NX2_OOT_FOG_SNAPSHOT");
    return env == nullptr || env[0] != '0';
}

// Stall attribution hook of the replay (Fritchii's Hitch line); Lichii has no Hitch logger, so it is unused
extern "C" void fritchii_hitch_add(int, unsigned long long) {}
