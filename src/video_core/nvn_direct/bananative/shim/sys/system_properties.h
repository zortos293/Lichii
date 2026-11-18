// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Linux stand-in for Bionic's system properties (none)
#pragma once
#define PROP_VALUE_MAX 92
static inline int __system_property_get(const char *name, char *value) { (void)name; value[0] = 0; return 0; }
