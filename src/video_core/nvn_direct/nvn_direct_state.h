// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Comes ahead of every C file of the NVN direct library (CMakeLists.txt): their variables of
// static storage (file scope and function scope, the front end's and the replay's alike: some
// two thousand, most of them upstream's) go to sections of their own, so nvn_direct_reset
// (nvn_reset.c) returns every one of them to its initial value when a game stops. Constant and
// thread-local variables stay where the compiler puts them.

#pragma once

#if defined(_WIN32)
// COFF: the linker sorts "<name>$<suffix>" by suffix into one section <name>, between the
// markers of nvn_reset.c ($a and $z).
#pragma clang section bss = "nvnb$m" data = "nvnd$m"
#else
// ELF: the linker brackets a section named as a C identifier with __start_ and __stop_ symbols.
#pragma clang section bss = "nvn_direct_bss" data = "nvn_direct_data"
#endif
