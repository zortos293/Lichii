// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Windows stand-in for <strings.h>
#pragma once
#include <string.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
