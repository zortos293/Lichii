// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Windows stand-in for <sched.h>: yielding, and a thread's processors (the first 64; plat_windows.c)
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct { uint64_t bits; } cpu_set_t;
#define CPU_ZERO(s) ((s)->bits = 0)
#define CPU_SET(c, s) ((s)->bits |= (uint64_t)1 << (c))
int sched_setaffinity(int tid, size_t size, const cpu_set_t *set);   // tid 0: the calling thread
int sched_yield(void);
