// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Windows stand-in for <unistd.h>: the C library's POSIX-named file calls (io.h, process.h), and
// the thread ID, page size, processor count and truncate (plat_windows.c)
#pragma once
#include <io.h>
#include <process.h>
#define F_OK 0
#define W_OK 2
#define R_OK 4
#include <sys/types.h>
int gettid(void);   // GetCurrentThreadId
int truncate(const char *path, long long length);   // (UTF-8 path)
int getpagesize(void);
#define _SC_CLK_TCK 2
#define _SC_NPROCESSORS_ONLN 84
long sysconf(int name);   // _SC_NPROCESSORS_ONLN; _SC_CLK_TCK fails (EINVAL): Windows has no clock ticks
