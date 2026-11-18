// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Linux stand-in for the NDK header. There is no Android window: the replay presents through Lichii,
// so its present path never has an ANativeWindow and these are never called with one
#pragma once
#include <stdint.h>
typedef struct ANativeWindow ANativeWindow;
#define ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE 1
static inline void ANativeWindow_acquire(ANativeWindow *w) { (void)w; }
static inline void ANativeWindow_release(ANativeWindow *w) { (void)w; }
static inline int32_t ANativeWindow_getWidth(ANativeWindow *w) { (void)w; return 0; }
static inline int32_t ANativeWindow_getHeight(ANativeWindow *w) { (void)w; return 0; }
