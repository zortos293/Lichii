// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Windows: the POSIX additions to the C library's own headers that Banana Native's code calls
// (clocks, sleeps, the environment, strlcpy and strcasestr, close-on-exec), included ahead of every file of the
// target (CMakeLists.txt). The functions are in plat_windows.c.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>

typedef ptrdiff_t ssize_t;
#ifndef _PID_T_   // (the Windows SDK has none; the define is mingw-w64's, whose headers have one)
#define _PID_T_
typedef int pid_t;
#endif

// Files by UTF-8 paths (Lichii's cache directory, DM_TEMP), and as POSIX has them: fopen's "e"
// (close-on-exec, which the C library's fopen refuses as an invalid mode) is its "N", and rename
// replaces an existing file (the C library's fails)
#include <io.h>
#define O_CLOEXEC _O_NOINHERIT
FILE *plat_win32_fopen(const char *path, const char *mode);
int plat_win32_rename(const char *from, const char *to);
int plat_win32_unlink(const char *path);
#define fopen plat_win32_fopen
#define rename plat_win32_rename
#define unlink plat_win32_unlink

typedef int clockid_t;
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_THREAD_CPUTIME_ID 3
int clock_gettime(clockid_t c, struct timespec *ts);   // CLOCK_MONOTONIC: QueryPerformanceCounter; CLOCK_THREAD_CPUTIME_ID: GetThreadTimes (100 ns steps)
int nanosleep(const struct timespec *req, struct timespec *rem);
int usleep(unsigned usec);

int setenv(const char *name, const char *value, int overwrite);   // the C library's environment (_putenv_s), the one getenv reads
int unsetenv(const char *name);

size_t strlcpy(char *dst, const char *src, size_t size);
char *strcasestr(const char *s, const char *find);

#define fseeko _fseeki64
#define ftello _ftelli64
