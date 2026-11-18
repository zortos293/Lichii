// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The front end and the replay as a new process has them, for the next game in the process.
// Upstream's code keeps its state in static variables for the life of its process (about two
// thousand of them, with the call sites' cached switches and once flags); nvn_direct_state.h puts
// all of them in sections of their own, whose initial bytes are kept here when the library loads
// and copied back by nvn_direct_reset. Its threads are joined and its Vulkan objects, pool imports
// and views of guest memory are destroyed before that (nvn_direct_park, nvn_direct_destroy).

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bananative/dm/gfx/vk_android/vka.h"
#include "nvn_direct.h"

// This file's own variables stay out of those sections: they outlive the reset.
#if defined(_WIN32)
#pragma clang section bss = "nvnb$a" data = "nvnd$a"
static char g_bss_begin, g_data_begin = 1;
#pragma clang section bss = "nvnb$z" data = "nvnd$z"
static char g_bss_end, g_data_end = 1;
#pragma clang section bss = "" data = ""
#define BSS_BEGIN (&g_bss_begin + 1)
#define BSS_END (&g_bss_end)
#define DATA_BEGIN (&g_data_begin + 1)
#define DATA_END (&g_data_end)
#else
#pragma clang section bss = "" data = ""
// (weak: a section that holds nothing has no bounds)
extern char __start_nvn_direct_bss[] __attribute__((weak)), __stop_nvn_direct_bss[] __attribute__((weak));
extern char __start_nvn_direct_data[] __attribute__((weak)), __stop_nvn_direct_data[] __attribute__((weak));
#define BSS_BEGIN __start_nvn_direct_bss
#define BSS_END __stop_nvn_direct_bss
#define DATA_BEGIN __start_nvn_direct_data
#define DATA_END __stop_nvn_direct_data
#endif

int nvn_direct_threads_left(void); // nvn_shutdown.c

static unsigned char* g_initial_data;

__attribute__((constructor)) static void keep_initial_data(void) {
    const size_t size = (size_t)(DATA_END - DATA_BEGIN);
    g_initial_data = malloc(size ? size : 1);
    if (g_initial_data) memcpy(g_initial_data, DATA_BEGIN, size);
}

int nvn_direct_reset(void) {
    // A compiler that ignores "#pragma clang section" (GCC) leaves the sections empty: nothing can
    // be reset, and the next boot in the process falls back (Core::NvnHook, RendererChoice::NotReset)
    if (!g_initial_data || (DATA_END == DATA_BEGIN && BSS_END == BSS_BEGIN)) return 0;
    if (!nvn_direct_threads_left()) {
        VRED("a thread of the replay did not leave: its state is not reset");
        return 0;
    }
    memcpy(DATA_BEGIN, g_initial_data, (size_t)(DATA_END - DATA_BEGIN));
    memset(BSS_BEGIN, 0, (size_t)(BSS_END - BSS_BEGIN));
    g_t0_ns = now_ns(); // (the game's clock: nvnDeviceGetCurrentTimestampInNanoseconds)
    return 1;
}
