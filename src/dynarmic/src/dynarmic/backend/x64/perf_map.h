// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <string_view>

#include <bit>

#ifdef _WIN32
#    include "common/jit_symbols.h"
#endif

namespace Dynarmic::Backend::X64 {

#if defined(__linux__) && !defined(__ANDROID__)
namespace detail {
void PerfMapRegister(const void* start, const void* end, std::string_view friendly_name);
}  // namespace detail
template<typename T>
void PerfMapRegister(T start, const void* end, std::string_view friendly_name) noexcept {
    detail::PerfMapRegister(std::bit_cast<const void*>(start), end, friendly_name);
}
void PerfMapClear();
inline void PerfMapClear(const void*, const void*) noexcept {
    PerfMapClear();
}
/// PERF_BUILDID_DIR is set (read once): blocks need a perf map name.
bool PerfMapEnabled() noexcept;
#elif defined(_WIN32)
// Windows has no perf; the names go to the nx2 sampler (common/jit_symbols.h) and, with
// EDEN_NX2_JIT_MAP=<folder>, to a map file like perf's (perf_map.cpp).
namespace detail {
void JitMapRegister(const void* start, const void* end, std::string_view friendly_name);
void JitMapClear(const void* begin, const void* end);
/// EDEN_NX2_JIT_MAP is set (read once).
bool JitMapEnabled() noexcept;
}  // namespace detail
template<typename T>
void PerfMapRegister(T start, const void* end, std::string_view friendly_name) noexcept {
    ::Common::JitSymbols::Register(std::bit_cast<const void*>(start), end, friendly_name);
    if (detail::JitMapEnabled()) {
        detail::JitMapRegister(std::bit_cast<const void*>(start), end, friendly_name);
    }
}
/// Forgets the names of the blocks in one JIT's code buffer, [begin, end).
inline void PerfMapClear(const void* begin, const void* end) noexcept {
    ::Common::JitSymbols::Clear(begin, end);
    if (detail::JitMapEnabled()) {
        detail::JitMapClear(begin, end);
    }
}
/// EDEN_NX2_JIT_SYMBOLS=1 (the sampler's names) or EDEN_NX2_JIT_MAP: blocks need a name.
inline bool PerfMapEnabled() noexcept {
    return ::Common::JitSymbols::Enabled() || detail::JitMapEnabled();
}
#else
// Resolve to no-op (compiler thinks fmt has side effects)
template<typename T> inline void PerfMapRegister(T, const void*, std::string_view) noexcept {}
inline void PerfMapClear(const void*, const void*) noexcept {}
inline bool PerfMapEnabled() noexcept { return false; }
#endif

}  // namespace Dynarmic::Backend::X64
