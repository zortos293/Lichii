// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <utility>

#include "common/common_types.h"

namespace Service {

/// NVN direct mode builds memory pools from Switch 2 native image handles
/// (nvnMemoryPoolBuilderSetNativeHandleNVX): the live graphics session's (nx2:gsv) lookup of a
/// handle's guest range, address and size, as gsv 428 answers it. Empty while no session lives.
struct Nx2NativeImageLookup {
    std::mutex mutex;
    const void* owner{};
    std::function<std::optional<std::pair<u64, u64>>(u32 handle)> range;
};
inline Nx2NativeImageLookup g_nx2_native_image_lookup;

} // namespace Service
