// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <mutex>

#include "core/hle/kernel/k_event.h"

namespace Service {

struct Nx2VideoReadyState {
    // Publish under the input queue lock; signal a new opening after releasing that lock.
    bool UpdateRoom(bool available) {
        const bool was_available = room.exchange(available, std::memory_order_relaxed);
        return available && !was_available;
    }

    void Signal(Kernel::KernelCore& kernel) {
        std::scoped_lock lk{mutex};
        if (event != nullptr && running && room.load(std::memory_order_relaxed)) {
            event->Signal(kernel);
        }
    }

    void SetRunning(Kernel::KernelCore& kernel, bool value) {
        std::scoped_lock lk{mutex};
        running = value;
        if (event != nullptr && !running) {
            event->Clear(kernel);
        }
    }

    std::mutex mutex;
    Kernel::KEvent* event{};
    std::atomic<bool> room{true};

private:
    bool running{true};
};

} // namespace Service
