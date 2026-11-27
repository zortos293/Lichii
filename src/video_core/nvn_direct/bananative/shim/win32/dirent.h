// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Windows stand-in for <dirent.h>: a directory's entries by name (FindFirstFileW, plat_windows.c).
// Linux's /proc and /sys directories do not exist here: opendir fails on them as on any missing one
#pragma once
typedef struct plat_win32_dir DIR;
struct dirent { char d_name[260]; };   // UTF-8
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
