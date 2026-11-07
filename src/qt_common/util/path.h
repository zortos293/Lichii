// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>

#include <QObject>
#include "common/common_types.h"

namespace QtCommon::Path {
/// <shader dir>/<title id as lowercase hex>, where the transferable pipeline caches live.
std::filesystem::path ShaderCacheFolder(u64 program_id);

bool OpenShaderCache(u64 program_id, QObject* parent);
}
