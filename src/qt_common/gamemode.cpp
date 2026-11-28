// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

// While technically available on al *NIX platforms, Linux is only available
// as the primary target of libgamemode.so - so warnings are suppressed
#ifdef __unix__
#include <gamemode_client.h>
#endif
#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/gamemode.h"

namespace Common::FeralGamemode {

/// @brief Start the gamemode client
void Start() noexcept {
    if (UISettings::values.enable_gamemode) {
#ifdef __unix__
        if (gamemode_request_start() < 0) {
#ifdef __linux__
            LOG_WARNING(Frontend, "{}", gamemode_error_string());
#else
            LOG_INFO(Frontend, "{}", gamemode_error_string());
#endif
        } else {
            LOG_INFO(Frontend, "Done");
#ifdef __linux__
            // Once: Start runs again each time a paused game resumes.
            static bool warned = false;
            if (Settings::values.pin_gpu_threads.GetValue() && Common::PlansGpuThreadPinning() &&
                !warned) {
                warned = true;
                LOG_WARNING(Frontend,
                            "GameMode 1.8 and later sets every thread of a game to a CPU mask it "
                            "chooses when the game registers and again every reaper_freq seconds "
                            "(5 by default), every online CPU on a CPU it finds uniform, which "
                            "undoes GPU thread pinning. Setting both pin_cores=no and "
                            "park_cores=no under [cpu] in gamemode.ini stops it (on hybrid CPUs "
                            "that also turns off GameMode's own pinning), as does turning off "
                            "the emulator's GameMode setting (enable_gamemode)");
            }
#endif
        }
#endif
    }
}

/// @brief Stop the gmemode client
void Stop() noexcept {
    if (UISettings::values.enable_gamemode) {
#ifdef __unix__
        if (gamemode_request_end() < 0) {
#ifdef __linux__
            LOG_WARNING(Frontend, "{}", gamemode_error_string());
#else
            LOG_INFO(Frontend, "{}", gamemode_error_string());
#endif
        } else {
            LOG_INFO(Frontend, "Done");
        }
#endif
    }
}

} // namespace Common::FeralGamemode
