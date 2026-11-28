// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Internal to common/thread.cpp and its tests: the CPU reservation with its thread listing and
// mask reads passed in, so that tests can make them fail.

#pragma once

#if defined(__linux__) && !defined(__ANDROID__)

#include <cstddef>
#include <optional>
#include <span>

#include "common/common_types.h"

namespace Common::detail {

/// How a reservation lists the process's threads and reads their masks. The public functions use
/// the process's own (/proc/self/task through opendir and readdir, and sched_getaffinity), which
/// make no C++ allocation; tests pass one that fails or throws on purpose. Plain function pointers
/// and a context, so that nothing allocates to call them. Preconditions: `list` and `read` are not
/// null (they are not checked); `context` is passed to both as is, and may be null.
enum class AffinityRead : u8 { Read, Exited, Failed };
struct ThreadAccess {
    /// Calls `visit(visit_context, tid)` with every thread id; false if the listing failed.
    bool (*list)(void* context, void (*visit)(void*, int), void* visit_context);
    /// Reads a thread's mask into `mask` (a cpu_set_t of `size` bytes).
    AffinityRead (*read)(void* context, int tid, void* mask, std::size_t size);
    void* context;
};

[[nodiscard]] ThreadAccess ProcessThreadAccess() noexcept;
/// As the public functions; neither throws. A listing that fails is tried again (up to three
/// times); an exception from `access` ends that walk at once, without a retry, and is handled as
/// a failure of the walk: before any thread is narrowed nothing is changed, after it the
/// reservation is undone (see ReserveCpusForCurrentThread).
std::optional<size_t> ReserveCpusForCurrentThread(std::span<const u32> cpus,
                                                  const ThreadAccess& access) noexcept;
void ReleaseReservedCpus(const ThreadAccess& access) noexcept;

} // namespace Common::detail

#endif
