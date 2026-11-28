// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/hle/service/nvdrv/devices/nvhost_ctrl.h"
#include "video_core/host1x/syncpoint_manager.h"

using Service::Nvidia::Devices::WaitHostStalled;
using Tegra::Host1x::SyncpointManager;

namespace {

/// A lockable that knows whether it is held, for "un-stalled before the lock is released".
struct TrackedLock {
    std::mutex mutex;
    std::atomic<bool> held{};
    void lock() {
        mutex.lock();
        held = true;
    }
    void unlock() {
        held = false;
        mutex.unlock();
    }
};

/// A model of System's stall and shutdown, in core.cpp's order: StallApplication takes
/// suspend_guard and, unless the stop is requested by then, suspends the guest and pauses core
/// timing; UnstallApplication resumes both as Core::ResumesAfterStall says; ShutdownMainProcess
/// requests the stop, takes and releases suspend_guard, then resumes core timing and suspends the
/// guest. `before_stall` runs between the helper's look at the stop and the stall's lock.
struct ModelSystem {
    TrackedLock suspend_guard;
    std::atomic<bool> paused{};
    std::atomic<bool> suspended{};
    std::atomic<int> stalls{};
    std::atomic<int> unstalls{};
    std::atomic<bool> unstalled_with_lock_held{};
    std::atomic<bool> timing_paused{};
    /// Un-stalls that resumed the guest with the stop already requested: must stay 0.
    std::atomic<int> resumed_after_stop{};
    /// Stalls that paused core timing after shutdown had resumed it: must stay 0.
    std::atomic<int> paused_after_shutdown{};
    std::atomic<bool> shutdown_done{};
    std::stop_source stop_event;
    std::function<void()> before_stall;

    std::unique_lock<TrackedLock> Stall() {
        if (before_stall) {
            before_stall();
        }
        std::unique_lock lock{suspend_guard};
        if (stop_event.stop_requested()) {
            return lock;
        }
        if (shutdown_done) {
            ++paused_after_shutdown;
        }
        suspended = true;
        timing_paused = true;
        ++stalls;
        return lock;
    }
    void Unstall() {
        unstalled_with_lock_held = suspend_guard.held.load();
        ++unstalls;
        if (Core::ResumesAfterStall(paused, stop_event.stop_requested())) {
            if (stop_event.stop_requested()) {
                ++resumed_after_stop;
            }
            suspended = false;
            timing_paused = false;
        }
    }
    void Shutdown() {
        stop_event.request_stop();
        {
            std::scoped_lock lock{suspend_guard};
        }
        timing_paused = false;
        suspended = true;
        shutdown_done = true;
    }
    bool Wait(SyncpointManager& manager, u32 id, u32 value) {
        return WaitHostStalled(
            manager, id, value, stop_event.get_token(), [this] { return Stall(); },
            [this] { Unstall(); });
    }
};

} // Anonymous namespace

TEST_CASE("An escalated wait returns when the value is reached and resumes the guest",
          "[core][nvdrv][escalated_wait]") {
    SyncpointManager manager;
    ModelSystem system;
    std::jthread incrementer{[&] {
        while (system.stalls == 0) {
            std::this_thread::yield();
        }
        manager.IncrementHost(3);
        manager.IncrementHost(3);
    }};
    REQUIRE(system.Wait(manager, 3, 2));
    REQUIRE(system.stalls == 1);
    REQUIRE(system.unstalls == 1);
    REQUIRE(system.unstalled_with_lock_held);
    REQUIRE_FALSE(system.suspended);
}

TEST_CASE("An escalated wait for a value never reached ends at shutdown, guest left suspended",
          "[core][nvdrv][escalated_wait]") {
    // As after three cleared waits on a slot whose fence lost its increments (a closed
    // channel's bank left by CompleteSyncpoint): before, this wait never returned, and the
    // services' close at shutdown waited for it forever.
    SyncpointManager manager;
    ModelSystem system;
    std::jthread shutdown{[&] {
        while (system.stalls == 0) {
            std::this_thread::yield();
        }
        system.Shutdown();
    }};
    REQUIRE_FALSE(system.Wait(manager, 5, 1000));
    shutdown.join();
    REQUIRE(system.unstalls == 1);
    REQUIRE(system.unstalled_with_lock_held);
    // Shutdown suspended the guest; the woken wait did not resume it.
    REQUIRE(system.suspended);
    REQUIRE(system.resumed_after_stop == 0);
    REQUIRE(manager.GetHostSyncpointValue(5) == 0);
}

TEST_CASE(
    "An escalated wait reached before a shutdown resumes the guest, then shutdown suspends it",
    "[core][nvdrv][escalated_wait]") {
    SyncpointManager manager;
    ModelSystem system;
    manager.IncrementHost(4);
    REQUIRE(system.Wait(manager, 4, 1));
    REQUIRE_FALSE(system.suspended);
    system.Shutdown();
    REQUIRE(system.suspended);
    REQUIRE(system.resumed_after_stop == 0);
}

TEST_CASE("An escalated wait during a shutdown already under way neither stalls nor resumes",
          "[core][nvdrv][escalated_wait]") {
    SyncpointManager manager;
    ModelSystem system;
    system.Shutdown();
    REQUIRE_FALSE(system.Wait(manager, 5, 1000));
    REQUIRE(system.stalls == 0);
    REQUIRE(system.unstalls == 0);
    REQUIRE(system.suspended);
    REQUIRE(system.resumed_after_stop == 0);
}

TEST_CASE("Shutdown returns only after a stall under way has un-stalled",
          "[core][nvdrv][escalated_wait]") {
    // The un-stall decides under suspend_guard, and shutdown takes it after the stop: once
    // Shutdown returns, no un-stall can still resume the guest.
    SyncpointManager manager;
    ModelSystem system;
    std::jthread waiter{[&] { (void)system.Wait(manager, 6, 1000); }};
    while (system.stalls == 0) {
        std::this_thread::yield();
    }
    system.Shutdown();
    CHECK(system.unstalls == 1);
    CHECK(system.suspended);
    CHECK(system.resumed_after_stop == 0);
}

TEST_CASE("A stall that starts after the stop neither suspends nor pauses",
          "[core][nvdrv][escalated_wait]") {
    // The helper saw no stop, then the whole shutdown ran before the stall took the lock: the
    // stall must not suspend the guest or pause core timing after shutdown resumed it.
    SyncpointManager manager;
    ModelSystem system;
    system.before_stall = [&] { system.Shutdown(); };
    REQUIRE_FALSE(system.Wait(manager, 7, 1000));
    CHECK(system.stalls == 0);
    CHECK(system.paused_after_shutdown == 0);
    CHECK_FALSE(system.timing_paused);
    CHECK(system.resumed_after_stop == 0);
    CHECK(system.suspended);
}

TEST_CASE("The guest is resumed after a stall only when not paused and not stopping",
          "[core][nvdrv][escalated_wait]") {
    STATIC_REQUIRE(Core::ResumesAfterStall(false, false));
    STATIC_REQUIRE_FALSE(Core::ResumesAfterStall(true, false));
    STATIC_REQUIRE_FALSE(Core::ResumesAfterStall(false, true));
    STATIC_REQUIRE_FALSE(Core::ResumesAfterStall(true, true));
}
