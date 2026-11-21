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

#ifdef __MINGW32__
// mingw-w64's headers have these already (winpthreads): use the plat_windows.c ones under other names
#include <pthread_time.h>
#define clock_gettime plat_win32_clock_gettime
#define nanosleep plat_win32_nanosleep
#define usleep plat_win32_usleep
// The shim's pthreads, sched, dirent, dlfcn and mman (shim/win32) have the names winpthreads and
// mingwex export, whose callers (libstdc++'s std::mutex and the rest of the program) use other
// layouts: the shim's are renamed, so they never replace those
#define pthread_attr_destroy plat_win32_pthread_attr_destroy
#define pthread_attr_init plat_win32_pthread_attr_init
#define pthread_attr_setstacksize plat_win32_pthread_attr_setstacksize
#define pthread_cond_broadcast plat_win32_pthread_cond_broadcast
#define pthread_cond_destroy plat_win32_pthread_cond_destroy
#define pthread_cond_init plat_win32_pthread_cond_init
#define pthread_cond_signal plat_win32_pthread_cond_signal
#define pthread_cond_timedwait plat_win32_pthread_cond_timedwait
#define pthread_cond_wait plat_win32_pthread_cond_wait
#define pthread_create plat_win32_pthread_create
#define pthread_detach plat_win32_pthread_detach
#define pthread_join plat_win32_pthread_join
#define pthread_mutex_destroy plat_win32_pthread_mutex_destroy
#define pthread_mutex_init plat_win32_pthread_mutex_init
#define pthread_mutex_lock plat_win32_pthread_mutex_lock
#define pthread_mutex_trylock plat_win32_pthread_mutex_trylock
#define pthread_mutex_unlock plat_win32_pthread_mutex_unlock
#define pthread_once plat_win32_pthread_once
#define pthread_rwlock_rdlock plat_win32_pthread_rwlock_rdlock
#define pthread_rwlock_tryrdlock plat_win32_pthread_rwlock_tryrdlock
#define pthread_rwlock_unlock plat_win32_pthread_rwlock_unlock
#define pthread_rwlock_wrlock plat_win32_pthread_rwlock_wrlock
#define pthread_self plat_win32_pthread_self
#define pthread_setname_np plat_win32_pthread_setname_np
#define sched_setaffinity plat_win32_sched_setaffinity
#define sched_yield plat_win32_sched_yield
#define opendir plat_win32_opendir
#define readdir plat_win32_readdir
#define closedir plat_win32_closedir
#define dlopen plat_win32_dlopen
#define dlsym plat_win32_dlsym
#define dlclose plat_win32_dlclose
#define mmap plat_win32_mmap
#define munmap plat_win32_munmap
#define mprotect plat_win32_mprotect
#define sysconf plat_win32_sysconf
#else
typedef int clockid_t;
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_THREAD_CPUTIME_ID 3
#endif
int clock_gettime(clockid_t c, struct timespec *ts);   // CLOCK_MONOTONIC: QueryPerformanceCounter; CLOCK_THREAD_CPUTIME_ID: GetThreadTimes (100 ns steps)
int nanosleep(const struct timespec *req, struct timespec *rem);
int usleep(unsigned usec);

int setenv(const char *name, const char *value, int overwrite);   // the C library's environment (_putenv_s), the one getenv reads
int unsetenv(const char *name);

size_t strlcpy(char *dst, const char *src, size_t size);
char *strcasestr(const char *s, const char *find);

#define fseeko _fseeki64
#define ftello _ftelli64
