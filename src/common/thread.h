// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include "common/common_types.h"
#include "common/polyfill_thread.h"

namespace Common {

class Event {
public:
#ifdef _WIN32
    ~Event();
#endif

    void Set() {
        std::scoped_lock lk{mutex};
        if (!is_set) {
            is_set = true;
#ifdef _WIN32
            if (sleepers != 0) {
                WakeSleeper();
            }
#endif
            condvar.notify_one();
        }
    }

    void Wait() {
        std::unique_lock lk{mutex};
        condvar.wait(lk, [&] { return is_set.load(); });
        is_set = false;
    }

    bool WaitFor(const std::chrono::nanoseconds time);

    /// Like WaitFor, but always blocks in the OS. On Windows, WaitFor keeps the thread running
    /// in a hardware wait; this sleeps on a high-resolution waitable timer instead.
    bool SleepFor(const std::chrono::nanoseconds time);

    /// Like WaitFor, but never blocks in the OS: waits in WaitForFlag, so a wake ends within
    /// microseconds of the deadline. For short waits; the thread counts as running throughout.
    bool SpinFor(const std::chrono::nanoseconds time);

    template<class Clock, class Duration>
    bool WaitUntil(const std::chrono::time_point<Clock, Duration> time) {
        std::unique_lock lk{mutex};
        if (!condvar.wait_until(lk, time, [this] { return is_set.load(); }))
            return false;
        is_set = false;
        return true;
    }

    void Reset() {
        std::unique_lock lk{mutex};
        // no other action required, since wait loops on the predicate and any lingering signal will
        // get cleared on the first iteration
        is_set = false;
    }

    [[nodiscard]] bool IsSet() const {
        return is_set;
    }

private:
#ifdef _WIN32
    void WakeSleeper();
#endif

    alignas(64) std::atomic<bool> is_set{false};
    std::condition_variable condvar;
    std::mutex mutex;
#ifdef _WIN32
    void* sleep_wake{};
    u32 sleepers{};
#endif
};

class Barrier {
public:
    explicit Barrier(std::size_t count_) : count(count_) {}

    /// Blocks until all "count" threads have called Sync()
    bool Sync(std::stop_token token = {}) {
        std::unique_lock lk{mutex};
        const std::size_t current_generation = generation;

        if (++waiting == count) {
            generation++;
            waiting = 0;
            condvar.notify_all();
            return true;
        } else {
            condvar.wait(lk, token,
                        [this, current_generation] { return current_generation != generation; });
            return !token.stop_requested();
        }
    }

private:
    std::condition_variable_any condvar;
    std::mutex mutex;
    std::size_t count;
    std::size_t waiting = 0;
    std::size_t generation = 0; // Incremented once each time the barrier is used
};

enum class ThreadPriority : u32 {
    Low = 0,
    Normal = 1,
    High = 2,
    VeryHigh = 3,
    Critical = 4,
};

enum class ThreadPlacement : u32 {
    Default = 0,
    Background = 1,
    Efficiency = 2,
};

void SetCurrentThreadPriority(ThreadPriority new_priority);
void SetCurrentThreadName(const char* name);
void SetCurrentThreadToPerformanceCores();
void SetCurrentThreadToEfficiencyCores();
void SetCurrentThreadToBackgroundWork();
void SetCurrentThreadToAllCores();

enum class PinnedRole : u32 { GpuFront, GpuBack, GuestCore };

/// The experimental pin_gpu_threads setting (Windows and Linux): the split GPU thread's front and
/// back each get a physical performance core of their own and the guest cores keep off those two.
/// On a CPU that gets the four-core plan (CoresForGpuBackCorePinning cores with SMT; Linux only)
/// only the back is pinned, after ReserveGpuBackCore; the front and the guest cores are left as
/// they are. Skipped, with one log line, on other CPUs with fewer than MinCoresForGpuThreadPinning
/// performance cores, with EDEN_HYBRID_CORES=0 (Windows), and on other hosts.
void PinCurrentThread(PinnedRole role);

/// Linux: takes `cpus` out of the affinity of every other thread of the process, each keeping the
/// rest of its own mask (a thread whose mask has only those CPUs is left as it is), so that the
/// calling thread can then be pinned to them alone. A thread starts with its creator's mask, so
/// the threads created during the reservation keep off them too, except those the caller creates
/// after pinning itself, which start on them. Every thread's mask is listed first, changing
/// nothing; the threads are then narrowed, and listed again for those created since. Returns how
/// many threads were moved. Returns nothing, with nothing reserved, while a reservation is active,
/// for no CPUs, when the caller's mask can't be read, when the first listing fails or a live
/// thread's mask can't be read in it (anything but its having exited: nothing has been changed),
/// when the second listing or a mask read in it fails, when a live thread that needed narrowing
/// could not be moved (its write failed, in either pass), and on other hosts. A write can fail
/// every time for a thread the kernel refuses to move (its allowed set holds only CPUs being
/// reserved, for example): then every reservation fails, and the back stays unpinned, with a
/// warning naming the thread and the reason. A listing that fails is tried up to three times. After
/// a failed second listing or a failed write, the reservation is undone exactly as
/// ReleaseReservedCpus undoes one (see its limits): the saved masks go back, and every thread with
/// none, created since the first listing, gets the reserved CPUs added to its mask, or the
/// caller's mask from before if its mask has only reserved CPUs. So such a thread does not get its
/// exact mask back: one the second walk narrowed gets the reserved CPUs added, and one pinned to
/// reserved CPUs alone gets the caller's whole mask. What can remain narrowed, each logged: a
/// thread created since the first listing by a narrowed thread, or narrowed by a failed second
/// attempt, if the undo's own listing fails too; and one whose mask still can't be read during the
/// undo. It does not throw: the process's listing and reads allocate nothing, and its logs catch
/// their own exceptions. An exception once narrowing has begun (only an injected lister's or
/// reader's, in tests) takes the same undo and returns nothing, so the caller stays unpinned; one
/// before it returns nothing with nothing changed.
std::optional<size_t> ReserveCpusForCurrentThread(std::span<const u32> cpus) noexcept;
/// Gives every thread that existed at the reservation, the caller included, its own mask from
/// before it. A thread created during it gets the reserved CPUs back, or the caller's mask from
/// before if its mask has only reserved CPUs (one the pinned caller created). Nothing while no
/// reservation is active. A saved mask goes back only to an id that is still a thread of this
/// process.
///
/// Limits: threads are told apart by id, so a thread created during the reservation under an id
/// that an exited thread had gets that thread's mask; a thread created during it that someone
/// pinned to a narrow set gets the reserved CPUs added, or, if that set lies inside the reserved
/// CPUs, the caller's whole mask; a thread that changed its own mask during it gets its old one
/// back; a thread created while Release walks the threads may keep its creator's narrowed mask;
/// a process started during the reservation keeps the narrowed mask of the thread that started it;
/// and if the threads can't be listed at release, those created during the reservation stay as
/// they are, as does one created during it whose mask can't be read (both logged). It does not
/// throw.
void ReleaseReservedCpus() noexcept;

/// With pin_gpu_threads, on a CPU that gets the four-core plan: reserves the back's physical core
/// with ReserveCpusForCurrentThread. The back calls it as it starts, then pins itself to that
/// core, so threads it creates from then on start on that core until the release. Returns
/// whether the back may pin itself: false only on the four-core plan when nothing was reserved
/// (logged), true when it was or nothing needs reserving.
[[nodiscard]] bool ReserveGpuBackCore() noexcept;
/// ReleaseReservedCpus; the back calls it when it stops.
void ReleaseGpuBackCore() noexcept;

/// Whether ReserveGpuBackCore reserves a core on this machine: the four-core plan (Linux; false
/// elsewhere).
[[nodiscard]] bool ReservesGpuBackCore();
/// Whether PinCurrentThread pins any role on this machine (Linux; false elsewhere).
[[nodiscard]] bool PlansGpuThreadPinning();

void RefreshThreadPolicies();
void PollThreadPolicies();

/// How long to poll before blocking next time, after a wait of `waited` that polled `poll`, with
/// `limit` the longest poll. This is the cpuidle-haltpoll governor's rule (Linux,
/// drivers/cpuidle/governors/haltpoll.c): a wait a longer poll would have caught doubles the poll,
/// and one longer than the limit, which no poll catches, halves it. The poll never drops below an
/// eighth of the limit, so the quickest wake-ups stay cheap.
constexpr std::chrono::nanoseconds NextPollWindow(std::chrono::nanoseconds poll,
                                                  std::chrono::nanoseconds waited,
                                                  std::chrono::nanoseconds limit) {
    if (waited <= poll) {
        return poll;
    }
    if (waited <= limit) {
        return std::min(poll * 2, limit);
    }
    return std::max(poll / 2, limit / 8);
}

/// Waits until flag is true or the deadline passes, and returns the flag. Where the CPU has a
/// user-mode wait on an address (MWAITX on AMD, UMWAIT on Intel) the thread waits in it: a store to
/// the flag's cache line ends the wait within a few hundred nanoseconds, and the core neither
/// issues instructions nor enters the OS meanwhile. Other CPUs yield until then.
bool WaitForFlag(const std::atomic<bool>& flag, std::chrono::steady_clock::time_point deadline);

} // namespace Common
