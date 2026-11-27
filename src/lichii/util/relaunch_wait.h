// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <charconv>
#include <chrono>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <thread>
#include <unistd.h>
#endif

namespace Lichii {

inline bool WaitForRelaunchingProcess(std::string_view value, std::chrono::milliseconds timeout) {
#ifdef _WIN32
    DWORD pid{};
#else
    pid_t pid{};
#endif
    if (value.empty()) {
        return false;
    }
    const auto end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, pid);
    if (parsed.ec != std::errc{} || parsed.ptr != end || pid <= 0) {
        return false;
    }
#ifdef _WIN32
    if (pid == GetCurrentProcessId()) {
        return false;
    }
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) {
        return GetLastError() == ERROR_INVALID_PARAMETER;
    }
    const DWORD result = WaitForSingleObject(process, static_cast<DWORD>(timeout.count()));
    CloseHandle(process);
    return result == WAIT_OBJECT_0;
#else
    if (pid == getpid()) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (kill(pid, 0) != 0 && errno != EPERM) {
            return errno == ESRCH;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
#endif
}

} // namespace Lichii
