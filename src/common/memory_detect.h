// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string_view>

#include "common/common_types.h"

namespace Common {

struct MemoryInfo {
    u64 TotalPhysicalMemory{};
    u64 TotalSwapMemory{};
};

/**
 * Gets the memory info of the host system
 * @return Reference to a MemoryInfo struct with the physical and swap memory sizes in bytes
 */
[[nodiscard]] const MemoryInfo& GetMemInfo();

/// This process's memory in bytes. Zero where the host does not report a value.
struct ProcessMemoryUsage {
    u64 commit{};              ///< Private commit (Windows PrivateUsage, Linux VmData).
    u64 working_set{};         ///< Resident memory.
    u64 private_working_set{}; ///< Resident memory not shared with other mappings.
    /// Commit charge of the whole system (Windows CommitTotal, Linux Committed_AS).
    u64 system_commit{};
    /// Commit limit of the whole system (CommitLimit on both).
    u64 system_commit_limit{};
};

[[nodiscard]] ProcessMemoryUsage GetProcessMemoryUsage();

/// Commit the system can still hand out, in bytes: its commit limit (RAM plus the page files)
/// minus what is committed. No value where the host reports no such limit (anything but Windows).
[[nodiscard]] std::optional<u64> GetSystemCommitRoom();

/// The value of a "Key:   1234 kB" line of /proc/self/status or /proc/meminfo, in bytes. `key`
/// includes the colon and must start a line. 0 when the field is missing or malformed.
[[nodiscard]] u64 ProcKilobytesField(std::string_view text, std::string_view key);

/// This process's resident set around one TrimHeap call, in bytes (0 where it does nothing).
struct HeapTrimResult {
    u64 resident_before{};
    u64 resident_after{};
};

/// Linux with glibc: malloc_trim(0), which hands the free pages inside every malloc arena back to
/// the system, and one log line with the resident set before and after. glibc only gives back
/// free memory at the top of an arena by itself, so what a burst of frees leaves between live
/// blocks (thousands of destroyed pipelines, the boot builds' compiler scratch) stays resident.
/// It locks one arena at a time, so a thread that allocates from the arena being walked waits
/// for that slice: call it off the GPU thread. Does nothing elsewhere.
HeapTrimResult TrimHeap(std::string_view reason);

} // namespace Common
