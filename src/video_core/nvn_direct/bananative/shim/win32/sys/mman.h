// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Windows stand-in for <sys/mman.h>: the mappings Banana Native's code makes outside the Linux
// memory paths (plat_windows.c):
// - MAP_PRIVATE | MAP_ANONYMOUS: VirtualAlloc, reserved and, unless PROT_NONE, committed;
// - MAP_SHARED | MAP_ANONYMOUS: a view of a section of its own backed by the page file;
// - MAP_SHARED of a file opened read-only: a read-only view of the file (PROT_READ only).
// MAP_FIXED is accepted over a PROT_NONE reservation this made, with PROT_NONE only: the pages
// are decommitted and stay reserved. munmap takes back a whole mapping (address and length as
// mmap returned them); mprotect is VirtualProtect. A call outside this subset fails with EINVAL.
#pragma once
#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_FAILED ((void *)-1)

void *mmap(void *addr, size_t len, int prot, int flags, int fd, long long off);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
