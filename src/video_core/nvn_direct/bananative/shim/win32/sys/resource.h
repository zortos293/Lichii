// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Windows stand-in for <sys/resource.h>: Linux's per-thread nice values (setpriority and getpriority
// with PRIO_PROCESS and a thread ID, 0 the calling thread) as thread priorities (plat_windows.c):
// 10 and above lowest, 1-9 below normal, 0 normal, below 0 above normal (Lichii's own high
// priority; Linux grants raised ones only to privileged processes)
#pragma once
#define PRIO_PROCESS 0
typedef unsigned id_t;
int setpriority(int which, int who, int nice);
int getpriority(int which, int who);
