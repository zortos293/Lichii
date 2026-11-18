// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Windows stand-in for <dlfcn.h>: LoadLibrary and GetProcAddress (plat_windows.c). The libraries the
// code asks for by their Linux and Android names (libandroid.so, libvulkan.so) are not found
#pragma once
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_LOCAL 0
void *dlopen(const char *name, int flags);
void *dlsym(void *lib, const char *name);
int dlclose(void *lib);
