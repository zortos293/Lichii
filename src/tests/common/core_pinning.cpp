// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <iterator>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "common/core_pinning.h"
#include "common/thread.h"
#include "common/thread_reservation.h"
#if defined(__linux__) && !defined(__ANDROID__)
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "common/scope_exit.h"
#endif

namespace {

using Common::LogicalProcessor;

// Windows numbers CPU sets from 256 in logical processor order, and every SMT sibling of a core
// reports the logical processor index of the core's first sibling as its CoreIndex.
constexpr u32 FirstCpuSetId = 256;

void AddCores(std::vector<LogicalProcessor>& processors, u32 cores, u32 threads_per_core,
              u8 efficiency_class) {
    for (u32 core = 0; core < cores; ++core) {
        const u32 first = static_cast<u32>(processors.size());
        for (u32 thread = 0; thread < threads_per_core; ++thread) {
            processors.push_back({FirstCpuSetId + first + thread, first, efficiency_class});
        }
    }
}

std::vector<LogicalProcessor> Uniform(u32 cores, u32 threads_per_core) {
    std::vector<LogicalProcessor> processors;
    AddCores(processors, cores, threads_per_core, 0);
    return processors;
}

std::vector<u32> Ids(u32 first, u32 count) {
    std::vector<u32> ids(count);
    for (u32 i = 0; i < count; ++i) {
        ids[i] = FirstCpuSetId + first + i;
    }
    return ids;
}

std::vector<u32> Concat(std::vector<u32> head, const std::vector<u32>& tail) {
    std::ranges::copy(tail, std::back_inserter(head));
    return head;
}

std::vector<u32> Sorted(std::vector<u32> ids) {
    std::ranges::sort(ids);
    return ids;
}

u32 CoreOf(const std::vector<LogicalProcessor>& processors, u32 id) {
    return std::ranges::find(processors, id, &LogicalProcessor::id)->core;
}

void CheckSkipped(const Common::GpuThreadPinning& plan) {
    CHECK(plan.front.empty());
    CHECK(plan.back.empty());
    CHECK(plan.guest.empty());
}

// Front and back each own one whole core, and no guest processor is an SMT sibling of either.
void CheckCoresAreDedicated(const std::vector<LogicalProcessor>& processors,
                            const Common::GpuThreadPinning& plan) {
    REQUIRE(!plan.front.empty());
    REQUIRE(!plan.back.empty());
    const u32 front_core = CoreOf(processors, plan.front.front());
    const u32 back_core = CoreOf(processors, plan.back.front());
    CHECK(front_core != back_core);
    for (const u32 id : plan.front) {
        CHECK(CoreOf(processors, id) == front_core);
    }
    for (const u32 id : plan.back) {
        CHECK(CoreOf(processors, id) == back_core);
    }
    for (const u32 id : plan.guest) {
        CHECK(CoreOf(processors, id) != front_core);
        CHECK(CoreOf(processors, id) != back_core);
    }
}

} // Anonymous namespace

TEST_CASE("Core pinning: 8 cores, 16 threads", "[common][core_pinning]") {
    const auto processors = Uniform(8, 2);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.performance_cores == 8);
    CHECK(plan.front == Ids(2, 2));
    CHECK(plan.back == Ids(4, 2));
    CHECK(plan.guest == Concat(Ids(0, 2), Ids(6, 10)));
    CheckCoresAreDedicated(processors, plan);
}

TEST_CASE("Core pinning: 6 cores, 12 threads", "[common][core_pinning]") {
    const auto processors = Uniform(6, 2);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.performance_cores == 6);
    CHECK(plan.front == Ids(2, 2));
    CHECK(plan.back == Ids(4, 2));
    CHECK(plan.guest == Concat(Ids(0, 2), Ids(6, 6)));
    CheckCoresAreDedicated(processors, plan);
}

TEST_CASE("Core pinning: 6 cores without SMT", "[common][core_pinning]") {
    const auto processors = Uniform(6, 1);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.front == Ids(1, 1));
    CHECK(plan.back == Ids(2, 1));
    CHECK(plan.guest == Concat(Ids(0, 1), Ids(3, 3)));
    CheckCoresAreDedicated(processors, plan);
}

TEST_CASE("Core pinning: hybrid 8 P-cores with SMT and 16 E-cores", "[common][core_pinning]") {
    std::vector<LogicalProcessor> processors;
    AddCores(processors, 8, 2, 1);
    AddCores(processors, 16, 1, 0);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.performance_cores == 8);
    CHECK(plan.front == Ids(2, 2));
    CHECK(plan.back == Ids(4, 2));
    CHECK(plan.guest == Concat(Ids(0, 2), Ids(6, 10)));
    CheckCoresAreDedicated(processors, plan);
}

TEST_CASE("Core pinning: E-cores enumerated first leave the P-cores to the pinned threads",
          "[common][core_pinning]") {
    std::vector<LogicalProcessor> processors;
    AddCores(processors, 4, 1, 0);
    AddCores(processors, 6, 2, 1);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.performance_cores == 6);
    CHECK(plan.front == Ids(6, 2));
    CHECK(plan.back == Ids(8, 2));
    CHECK(plan.guest == Concat(Ids(4, 2), Ids(10, 6)));
    CheckCoresAreDedicated(processors, plan);
}

TEST_CASE("Core pinning: hybrid 4 P-cores and 8 E-cores is skipped", "[common][core_pinning]") {
    std::vector<LogicalProcessor> processors;
    AddCores(processors, 4, 2, 1);
    AddCores(processors, 8, 1, 0);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.performance_cores == 4);
    CheckSkipped(plan);
}

TEST_CASE("Core pinning: 4 cores, 8 threads gives only the back a core", "[common][core_pinning]") {
    const auto processors = Uniform(4, 2);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.performance_cores == 4);
    CHECK(plan.back_core_reserved);
    CHECK(plan.back == Ids(2, 2));
    CHECK(plan.front == Concat(Ids(0, 2), Ids(4, 4)));
    CHECK(plan.guest == plan.front);
    for (const u32 id : plan.guest) {
        CHECK(CoreOf(processors, id) != CoreOf(processors, plan.back.front()));
    }
}

TEST_CASE("Core pinning: other plans leave the back's core unreserved",
          "[common][core_pinning]") {
    CHECK(!Common::PlanGpuThreadPinning(Uniform(6, 2)).back_core_reserved);
    CHECK(!Common::PlanGpuThreadPinning(Uniform(4, 1)).back_core_reserved);
}

TEST_CASE("Core pinning: 4 cores of which one lacks SMT is skipped", "[common][core_pinning]") {
    std::vector<LogicalProcessor> processors;
    AddCores(processors, 3, 2, 0);
    AddCores(processors, 1, 1, 0);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(!plan.back_core_reserved);
    CheckSkipped(plan);
}

TEST_CASE("Core pinning: 4 cores, 4 threads is skipped", "[common][core_pinning]") {
    const auto plan = Common::PlanGpuThreadPinning(Uniform(4, 1));
    CHECK(plan.performance_cores == 4);
    CheckSkipped(plan);
}

TEST_CASE("Core pinning: 2 cores is skipped", "[common][core_pinning]") {
    for (const u32 threads : {1U, 2U}) {
        const auto plan = Common::PlanGpuThreadPinning(Uniform(2, threads));
        CHECK(plan.performance_cores == 2);
        CheckSkipped(plan);
    }
}

TEST_CASE("Core pinning: no processors reported", "[common][core_pinning]") {
    const auto plan = Common::PlanGpuThreadPinning({});
    CHECK(plan.performance_cores == 0);
    CheckSkipped(plan);
}

TEST_CASE("Core pinning: the enumeration order does not change the cores",
          "[common][core_pinning]") {
    const auto ordered = Uniform(8, 2);
    auto reversed = ordered;
    std::ranges::reverse(reversed);
    const auto expected = Common::PlanGpuThreadPinning(ordered);
    const auto plan = Common::PlanGpuThreadPinning(reversed);
    CHECK(Sorted(plan.front) == expected.front);
    CHECK(Sorted(plan.back) == expected.back);
    CHECK(Sorted(plan.guest) == expected.guest);
}

TEST_CASE("Core pinning: sysfs CPU lists", "[common][core_pinning]") {
    using Common::ParseCpuList;
    CHECK(ParseCpuList("0-3,8,10-11") == std::vector<u32>{0, 1, 2, 3, 8, 10, 11});
    CHECK(ParseCpuList("16-23\n") == std::vector<u32>{16, 17, 18, 19, 20, 21, 22, 23});
    CHECK(ParseCpuList("5") == std::vector<u32>{5});
    CHECK(ParseCpuList("").empty());
    CHECK(ParseCpuList("3-1").empty());
    CHECK(ParseCpuList("1,x").empty());
    CHECK(ParseCpuList("0-").empty());
    CHECK(ParseCpuList("0-4294967295").empty());
    CHECK(ParseCpuList("4294967295") == std::vector<u32>{4294967295U});
    CHECK(ParseCpuList("4294967294-4294967295") == std::vector<u32>{4294967294U, 4294967295U});
    CHECK(ParseCpuList("4294967296").empty());
}

TEST_CASE("Core pinning: a 4P+4E CPU whose E-cores rank lower is skipped",
          "[common][core_pinning]") {
    // As LinuxProcessors reports an Intel hybrid CPU without cpu_capacity: the cpu_atom cores at
    // half capacity.
    std::vector<LogicalProcessor> processors;
    AddCores(processors, 4, 2, 128);
    AddCores(processors, 4, 1, 64);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.performance_cores == 4);
    CheckSkipped(plan);
}

#if defined(__linux__) && !defined(__ANDROID__)
namespace {

cpu_set_t ThreadMask(pid_t tid) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    REQUIRE(sched_getaffinity(tid, sizeof(mask), &mask) == 0);
    return mask;
}

// A thread of the test process may exit while the test runs (one a library started).
std::optional<cpu_set_t> MaskIfAlive(pid_t tid) {
    cpu_set_t mask;
    if (sched_getaffinity(tid, sizeof(mask), &mask) != 0) {
        return std::nullopt;
    }
    return mask;
}

std::vector<std::pair<pid_t, cpu_set_t>> ProcessMasks() {
    std::vector<std::pair<pid_t, cpu_set_t>> masks;
    for (const auto& entry : std::filesystem::directory_iterator{"/proc/self/task"}) {
        const pid_t tid = static_cast<pid_t>(std::stoi(entry.path().filename().string()));
        if (const auto mask = MaskIfAlive(tid)) {
            masks.emplace_back(tid, *mask);
        }
    }
    return masks;
}

cpu_set_t MaskOf(std::span<const u32> cpus) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (const u32 cpu : cpus) {
        CPU_SET(cpu, &mask);
    }
    return mask;
}

// A thread that reports its id and waits until told to finish; it sets its own mask first if
// given one, and otherwise keeps the mask it started with, its creator's.
class HelperThread {
public:
    explicit HelperThread(std::optional<cpu_set_t> mask = std::nullopt) {
        thread = std::thread([this, mask] {
            if (mask) {
                sched_setaffinity(0, sizeof(*mask), &*mask);
            }
            Run();
        });
        Started();
    }
    ~HelperThread() {
        Finish();
        thread.join();
        // join returns before the thread leaves /proc/self/task: a listing right after would
        // still see it (and narrow it, or try to), so the next case's counts would include it.
        // Bounded, in case a new thread of the process gets the id meanwhile.
        const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds{1};
        while (tid != 0 && syscall(SYS_tgkill, getpid(), tid, 0) == 0 &&
               std::chrono::steady_clock::now() < give_up) {
            std::this_thread::sleep_for(std::chrono::microseconds{50});
        }
    }
    pid_t Id() const {
        return tid;
    }
    /// Has the helper create a helper of its own, which starts with the helper's mask.
    pid_t Spawn() {
        std::unique_lock lock{mutex};
        spawn = true;
        changed.notify_all();
        changed.wait(lock, [this] { return child_tid != 0; });
        return child_tid;
    }

private:
    void Run() {
        std::unique_lock lock{mutex};
        tid = gettid();
        changed.notify_all();
        std::optional<HelperThread> child;
        while (!done) {
            changed.wait(lock, [this, &child] { return done || (spawn && !child); });
            if (spawn && !child && !done) {
                lock.unlock();
                child.emplace();
                lock.lock();
                child_tid = child->Id();
                changed.notify_all();
            }
        }
        lock.unlock();
        child.reset();
    }
    void Started() {
        std::unique_lock lock{mutex};
        changed.wait(lock, [this] { return tid != 0; });
    }
    void Finish() {
        std::scoped_lock lock{mutex};
        done = true;
        changed.notify_all();
    }

    std::mutex mutex;
    std::condition_variable changed;
    pid_t tid{};
    pid_t child_tid{};
    bool spawn{};
    bool done{};
    std::thread thread;
};

} // Anonymous namespace

// Reserves two of the CPUs this process may run on, whatever the host: the threads keep their
// own masks less those two, and get exactly their old masks back.
TEST_CASE("Core pinning: a reservation keeps each thread's own mask and restores it",
          "[common][core_pinning]") {
    const cpu_set_t allowed = ThreadMask(0);
    std::vector<u32> cpus;
    for (u32 cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &allowed)) {
            cpus.push_back(cpu);
        }
    }
    if (cpus.size() < 4) {
        SKIP("needs at least four CPUs, this process may use " << cpus.size());
    }
    const std::vector<u32> reserved{cpus[cpus.size() - 2], cpus[cpus.size() - 1]};
    const cpu_set_t reserved_mask = MaskOf(reserved);
    // Threads that start off the reserved CPUs, on one of them alone, and over both and one other.
    const HelperThread off{MaskOf(std::vector<u32>{cpus[0]})};
    const HelperThread inside{MaskOf(std::vector<u32>{reserved[0]})};
    const HelperThread across{MaskOf(std::vector<u32>{cpus[1], reserved[0], reserved[1]})};
    HelperThread creator{};
    const auto before = ProcessMasks();

    SCOPE_EXIT {
        Common::ReleaseReservedCpus();
        sched_setaffinity(0, sizeof(allowed), &allowed);
    };
    CHECK(Common::ReserveCpusForCurrentThread(reserved).value_or(0) >= 2);
    CHECK(!Common::ReserveCpusForCurrentThread(reserved)); // a second one changes nothing
    for (const auto& [tid, mask] : before) {
        const auto during = MaskIfAlive(tid);
        if (!during) {
            continue;
        }
        if (tid == gettid() || tid == inside.Id()) {
            // The caller is left for its own pinning; the other would be left with no CPU.
            CHECK(CPU_EQUAL(&*during, &mask));
            continue;
        }
        // Its own mask less the reserved CPUs.
        cpu_set_t overlap;
        CPU_AND(&overlap, &mask, &reserved_mask);
        cpu_set_t expected;
        CPU_XOR(&expected, &mask, &overlap);
        CHECK(CPU_EQUAL(&*during, &expected));
    }
    const cpu_set_t across_during = ThreadMask(across.Id());
    const cpu_set_t across_expected = MaskOf(std::vector<u32>{cpus[1]});
    CHECK(CPU_EQUAL(&across_during, &across_expected));

    // Created during the reservation: by a narrowed thread, it starts narrowed and gets the
    // reserved CPUs back; by the caller once pinned to the reserved CPUs, as the back does, it
    // starts on them and gets the caller's old mask.
    const pid_t from_narrowed = creator.Spawn();
    cpu_set_t narrowed_expected = allowed;
    for (const u32 cpu : reserved) {
        CPU_CLR(cpu, &narrowed_expected);
    }
    const cpu_set_t from_narrowed_during = ThreadMask(from_narrowed);
    CHECK(CPU_EQUAL(&from_narrowed_during, &narrowed_expected));
    const cpu_set_t caller_before = ThreadMask(0);
    REQUIRE(sched_setaffinity(0, sizeof(reserved_mask), &reserved_mask) == 0);
    std::optional<HelperThread> from_pinned_caller;
    from_pinned_caller.emplace();
    const cpu_set_t from_pinned_during = ThreadMask(from_pinned_caller->Id());
    CHECK(CPU_EQUAL(&from_pinned_during, &reserved_mask));

    Common::ReleaseReservedCpus();
    for (const auto& [tid, mask] : before) {
        if (const auto after = MaskIfAlive(tid)) {
            CHECK(CPU_EQUAL(&*after, &mask));
        }
    }
    const cpu_set_t from_narrowed_after = ThreadMask(from_narrowed);
    CHECK(CPU_EQUAL(&from_narrowed_after, &allowed));
    const cpu_set_t from_pinned_after = ThreadMask(from_pinned_caller->Id());
    CHECK(CPU_EQUAL(&from_pinned_after, &caller_before));
    Common::ReleaseReservedCpus(); // a second release changes nothing
    for (const auto& [tid, mask] : before) {
        if (const auto after = MaskIfAlive(tid)) {
            CHECK(CPU_EQUAL(&*after, &mask));
        }
    }
}

namespace {

// The CPUs this process may use, and two of them to reserve; nothing if there are fewer than four.
std::optional<std::pair<std::vector<u32>, std::vector<u32>>> ReservableCpus() {
    const cpu_set_t allowed = ThreadMask(0);
    std::vector<u32> cpus;
    for (u32 cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &allowed)) {
            cpus.push_back(cpu);
        }
    }
    if (cpus.size() < 4) {
        return std::nullopt;
    }
    std::vector<u32> reserved{cpus[cpus.size() - 2], cpus[cpus.size() - 1]};
    return std::pair{std::move(cpus), std::move(reserved)};
}

void CheckMasksAsBefore(const std::vector<std::pair<pid_t, cpu_set_t>>& before) {
    for (const auto& [tid, mask] : before) {
        if (const auto now = MaskIfAlive(tid)) {
            CHECK(CPU_EQUAL(&*now, &mask));
        }
    }
}

// After a failed reservation, a real one still works and is undone.
void CheckReserveAndReleaseWork(std::span<const u32> reserved,
                                const std::vector<std::pair<pid_t, cpu_set_t>>& before) {
    CHECK(Common::ReserveCpusForCurrentThread(reserved).has_value());
    Common::ReleaseReservedCpus();
    CheckMasksAsBefore(before);
}

// A ThreadAccess made of std::function members, for tests: each defaults to the process's own.
bool RealList(const std::function<void(int)>& visit) {
    const auto real = Common::detail::ProcessThreadAccess();
    return real.list(
        real.context,
        [](void* p, int tid) { (*static_cast<const std::function<void(int)>*>(p))(tid); },
        const_cast<void*>(static_cast<const void*>(&visit)));
}
Common::detail::AffinityRead RealRead(int tid, void* mask, std::size_t size) {
    const auto real = Common::detail::ProcessThreadAccess();
    return real.read(real.context, tid, mask, size);
}
struct TestAccess {
    std::function<bool(const std::function<void(int)>&)> list = RealList;
    std::function<Common::detail::AffinityRead(int, void*, std::size_t)> read = RealRead;

    Common::detail::ThreadAccess Get() {
        return {&List, &Read, this};
    }

private:
    static bool List(void* self, void (*visit)(void*, int), void* visit_context) {
        return static_cast<TestAccess*>(self)->list([&](int tid) { visit(visit_context, tid); });
    }
    static Common::detail::AffinityRead Read(void* self, int tid, void* mask, std::size_t size) {
        return static_cast<TestAccess*>(self)->read(tid, mask, size);
    }
};

} // Anonymous namespace

TEST_CASE("Core pinning: a failed second listing undoes the reservation, an unvisited child too",
          "[common][core_pinning]") {
    const auto cpus = ReservableCpus();
    if (!cpus) {
        SKIP("needs at least four CPUs");
    }
    const cpu_set_t allowed = ThreadMask(0);
    HelperThread creator{};
    const auto before = ProcessMasks();
    // Restores the main thread only: the helpers end with the test, and every other thread's mask
    // is checked below.
    SCOPE_EXIT {
        Common::ReleaseReservedCpus();
        sched_setaffinity(0, sizeof(allowed), &allowed);
    };
    // The first listing works; the second fails on every attempt, after the creator (narrowed by
    // then) has created a child the listing never reaches; the rollback's listing works.
    int calls = 0;
    pid_t child = 0;
    TestAccess failing;
    failing.list = [&](const std::function<void(int)>& visit) {
        ++calls;
        if (calls == 2) {
            child = creator.Spawn();
        }
        if (calls >= 2 && calls <= 4) {
            return false;
        }
        return RealList(visit);
    };
    CHECK(!Common::detail::ReserveCpusForCurrentThread(cpus->second, failing.Get()));
    CHECK(calls == 5); // one, three attempts, and the rollback's
    REQUIRE(child != 0);
    CheckMasksAsBefore(before);
    // The child started with its narrowed creator's mask; the rollback gave the reserved CPUs back.
    const cpu_set_t child_after = ThreadMask(child);
    CHECK(CPU_EQUAL(&child_after, &allowed));
    CheckReserveAndReleaseWork(cpus->second, before);
}

TEST_CASE("Core pinning: an unreadable mask in the second listing undoes the reservation",
          "[common][core_pinning]") {
    const auto cpus = ReservableCpus();
    if (!cpus) {
        SKIP("needs at least four CPUs");
    }
    const cpu_set_t allowed = ThreadMask(0);
    HelperThread creator{};
    const auto before = ProcessMasks();
    // Restores the main thread only: the helpers end with the test, and every other thread's mask
    // is checked below.
    SCOPE_EXIT {
        Common::ReleaseReservedCpus();
        sched_setaffinity(0, sizeof(allowed), &allowed);
    };
    // A child created between the two listings, whose mask the second walk can't read once (not
    // because it exited): the reservation must fail and be undone, the child included.
    int calls = 0;
    pid_t child = 0;
    bool failed_once = false;
    TestAccess failing;
    failing.list = [&](const std::function<void(int)>& visit) {
        if (++calls == 2) {
            child = creator.Spawn();
        }
        return RealList(visit);
    };
    failing.read = [&](int tid, void* mask, std::size_t size) {
        if (tid == child && child != 0 && !failed_once) {
            failed_once = true;
            return Common::detail::AffinityRead::Failed;
        }
        return RealRead(tid, mask, size);
    };
    CHECK(!Common::detail::ReserveCpusForCurrentThread(cpus->second, failing.Get()));
    CHECK(failed_once);
    CheckMasksAsBefore(before);
    const cpu_set_t child_after = ThreadMask(child);
    CHECK(CPU_EQUAL(&child_after, &allowed));
    CheckReserveAndReleaseWork(cpus->second, before);
}

TEST_CASE("Core pinning: an unreadable mask in the first listing changes nothing",
          "[common][core_pinning]") {
    const auto cpus = ReservableCpus();
    if (!cpus) {
        SKIP("needs at least four CPUs");
    }
    const cpu_set_t allowed = ThreadMask(0);
    const HelperThread other{};
    const auto before = ProcessMasks();
    // Restores the main thread only: the helpers end with the test, and every other thread's mask
    // is checked below.
    SCOPE_EXIT {
        Common::ReleaseReservedCpus();
        sched_setaffinity(0, sizeof(allowed), &allowed);
    };
    TestAccess failing;
    failing.read = [&](int tid, void* mask, std::size_t size) {
        if (tid == other.Id()) {
            return Common::detail::AffinityRead::Failed;
        }
        return RealRead(tid, mask, size);
    };
    CHECK(!Common::detail::ReserveCpusForCurrentThread(cpus->second, failing.Get()));
    CheckMasksAsBefore(before);
    CheckReserveAndReleaseWork(cpus->second, before);
}

TEST_CASE("Core pinning: a second listing that fails once is tried again without counting twice",
          "[common][core_pinning]") {
    const auto cpus = ReservableCpus();
    if (!cpus) {
        SKIP("needs at least four CPUs");
    }
    const cpu_set_t allowed = ThreadMask(0);
    const HelperThread off{MaskOf(std::vector<u32>{cpus->first[0]})};
    const auto before = ProcessMasks();
    // Restores the main thread only: the helpers end with the test, and every other thread's mask
    // is checked below.
    SCOPE_EXIT {
        Common::ReleaseReservedCpus();
        sched_setaffinity(0, sizeof(allowed), &allowed);
    };
    // Both reservations list only the caller and this case's own threads, so the counts depend
    // on nothing else the process runs (a library's worker starting or exiting meanwhile).
    std::optional<HelperThread> late;
    const pid_t self = gettid();
    const auto list_own = [&](const std::function<void(int)>& visit) {
        return RealList([&](int tid) {
            if (tid == self || tid == off.Id() || (late && tid == late->Id())) {
                visit(tid);
            }
        });
    };
    // A clean reservation's count, for comparison: `off` keeps its one unreserved CPU, so 0.
    TestAccess own;
    own.list = list_own;
    const auto clean = Common::detail::ReserveCpusForCurrentThread(cpus->second, own.Get());
    REQUIRE(clean.has_value());
    CHECK(*clean == 0);
    // Released through the same listing: the real one would take every unrelated thread for one
    // created during the reservation.
    Common::detail::ReleaseReservedCpus(own.Get());
    CheckMasksAsBefore(before);
    // Before the second listing, the caller (never narrowed) creates a thread, which starts with
    // the caller's whole mask: the second listing's first attempt narrows it and then fails, the
    // second attempt sees it again, narrowed already, and succeeds. Counted once, not twice.
    int calls = 0;
    TestAccess flaky;
    flaky.list = [&](const std::function<void(int)>& visit) {
        if (++calls == 2) {
            late.emplace();
            (void)list_own(visit);
            return false;
        }
        return list_own(visit);
    };
    const auto count = Common::detail::ReserveCpusForCurrentThread(cpus->second, flaky.Get());
    REQUIRE(count.has_value());
    CHECK(calls == 3);
    // The late thread, narrowed exactly once.
    CHECK(*count == 1);
    REQUIRE(late.has_value());
    cpu_set_t narrowed = allowed;
    for (const u32 cpu : cpus->second) {
        CPU_CLR(cpu, &narrowed);
    }
    const cpu_set_t late_during = ThreadMask(late->Id());
    CHECK(CPU_EQUAL(&late_during, &narrowed));
    Common::detail::ReleaseReservedCpus(own.Get());
    CheckMasksAsBefore(before);
    const cpu_set_t late_after = ThreadMask(late->Id());
    CHECK(CPU_EQUAL(&late_after, &allowed));
}

TEST_CASE("Core pinning: a thread that cannot be moved off the reserved CPUs fails the reservation",
          "[common][core_pinning]") {
    const auto cpus = ReservableCpus();
    if (!cpus) {
        SKIP("needs at least four CPUs");
    }
    const cpu_set_t allowed = ThreadMask(0);
    const auto before = ProcessMasks();
    SCOPE_EXIT {
        Common::ReleaseReservedCpus();
        sched_setaffinity(0, sizeof(allowed), &allowed);
    };
    // A thread created after the first listing reads, in the second walk and in the undo, as
    // allowed on the reserved CPUs and CPU 1023 (CPU_SETSIZE - 1), which the case assumes this
    // process cannot use: narrowing it leaves only that one, and the real write fails (EINVAL),
    // as it would for a thread whose allowed set shrank to the reserved CPUs. Its real mask is
    // never changed.
    std::optional<HelperThread> late;
    int calls = 0;
    TestAccess stuck;
    stuck.list = [&](const std::function<void(int)>& visit) {
        if (++calls == 2) {
            late.emplace();
        }
        return RealList(visit);
    };
    stuck.read = [&](int tid, void* mask, std::size_t size) {
        if (late && tid == late->Id()) {
            cpu_set_t fake = MaskOf(cpus->second);
            CPU_SET(CPU_SETSIZE - 1, &fake);
            std::memcpy(mask, &fake, std::min(size, sizeof(fake)));
            return Common::detail::AffinityRead::Read;
        }
        return RealRead(tid, mask, size);
    };
    const auto count = Common::detail::ReserveCpusForCurrentThread(cpus->second, stuck.Get());
    CHECK_FALSE(count.has_value());
    // The first listing, the second (which failed on the late thread), and the undo's.
    CHECK(calls == 3);
    REQUIRE(late.has_value());
    // Undone: every thread has its mask back, the stuck one never lost its own, and nothing is
    // reserved (a real reservation works).
    CheckMasksAsBefore(before);
    const cpu_set_t late_after = ThreadMask(late->Id());
    CHECK(CPU_EQUAL(&late_after, &allowed));
    CheckReserveAndReleaseWork(cpus->second, before);
}

TEST_CASE("Core pinning: a thread that cannot be moved in the first pass fails the reservation",
          "[common][core_pinning]") {
    const auto cpus = ReservableCpus();
    if (!cpus) {
        SKIP("needs at least four CPUs");
    }
    const cpu_set_t allowed = ThreadMask(0);
    // On the reserved CPUs alone, so a real reservation leaves it as it is.
    const cpu_set_t on_reserved = MaskOf(cpus->second);
    const HelperThread pinned{on_reserved};
    const auto before = ProcessMasks();
    SCOPE_EXIT {
        Common::ReleaseReservedCpus();
        sched_setaffinity(0, sizeof(allowed), &allowed);
    };
    // It exists at the first listing and reads as its own mask plus CPU 1023 (CPU_SETSIZE - 1),
    // which the case assumes this process cannot use: the first pass narrows it to that CPU
    // alone, and the real write fails (EINVAL). The undo writes the mask as read back, which the
    // kernel takes as the reserved CPUs alone, so its real mask ends as it began.
    int calls = 0;
    TestAccess stuck;
    stuck.list = [&](const std::function<void(int)>& visit) {
        ++calls;
        return RealList(visit);
    };
    stuck.read = [&](int tid, void* mask, std::size_t size) {
        if (tid == pinned.Id()) {
            cpu_set_t fake = on_reserved;
            CPU_SET(CPU_SETSIZE - 1, &fake);
            std::memcpy(mask, &fake, std::min(size, sizeof(fake)));
            return Common::detail::AffinityRead::Read;
        }
        return RealRead(tid, mask, size);
    };
    const auto count = Common::detail::ReserveCpusForCurrentThread(cpus->second, stuck.Get());
    CHECK_FALSE(count.has_value());
    // The first listing, then the undo's: the failure comes before the second listing.
    CHECK(calls == 2);
    CheckMasksAsBefore(before);
    const cpu_set_t pinned_after = ThreadMask(pinned.Id());
    CHECK(CPU_EQUAL(&pinned_after, &on_reserved));
    CheckReserveAndReleaseWork(cpus->second, before);
}

TEST_CASE("Core pinning: an id that is not a thread of this process is neither read nor written",
          "[common][core_pinning]") {
    const auto cpus = ReservableCpus();
    if (!cpus) {
        SKIP("needs at least four CPUs");
    }
    const cpu_set_t allowed = ThreadMask(0);
    const auto before = ProcessMasks();
    SCOPE_EXIT {
        Common::ReleaseReservedCpus();
        sched_setaffinity(0, sizeof(allowed), &allowed);
    };
    // Every listing also reports the parent process's id, as a listing would an id reissued to
    // another process's thread since it was taken. The reservation and its release must not read
    // it (counted through the injected reader) or change its mask.
    const pid_t stranger = getppid();
    cpu_set_t stranger_before;
    REQUIRE(sched_getaffinity(stranger, sizeof(stranger_before), &stranger_before) == 0);
    int stranger_reads = 0;
    TestAccess listing;
    listing.list = [&](const std::function<void(int)>& visit) {
        const bool listed = RealList(visit);
        visit(stranger);
        return listed;
    };
    listing.read = [&](int tid, void* mask, std::size_t size) {
        if (tid == stranger) {
            ++stranger_reads;
        }
        return RealRead(tid, mask, size);
    };
    const auto count = Common::detail::ReserveCpusForCurrentThread(cpus->second, listing.Get());
    CHECK(count.has_value());
    Common::detail::ReleaseReservedCpus(listing.Get());
    CHECK(stranger_reads == 0);
    cpu_set_t stranger_after;
    REQUIRE(sched_getaffinity(stranger, sizeof(stranger_after), &stranger_after) == 0);
    CHECK(CPU_EQUAL(&stranger_after, &stranger_before));
    CheckMasksAsBefore(before);
}

TEST_CASE("Core pinning: an exception in the second listing undoes the reservation",
          "[common][core_pinning]") {
    const auto cpus = ReservableCpus();
    if (!cpus) {
        SKIP("needs at least four CPUs");
    }
    const cpu_set_t allowed = ThreadMask(0);
    HelperThread creator{};
    const auto before = ProcessMasks();
    // Restores the main thread only: the helpers end with the test, and every other thread's mask
    // is checked below.
    SCOPE_EXIT {
        Common::ReleaseReservedCpus();
        sched_setaffinity(0, sizeof(allowed), &allowed);
    };
    // The second listing visits a child its narrowed creator just made (born narrowed), then
    // throws as an allocation would. The reservation must not throw, and its undo must reach the
    // child.
    int calls = 0;
    pid_t child = 0;
    TestAccess throwing;
    throwing.list = [&](const std::function<void(int)>& visit) -> bool {
        if (++calls == 2) {
            child = creator.Spawn();
            RealList(visit);
            throw std::bad_alloc{};
        }
        return RealList(visit);
    };
    std::optional<size_t> result{0};
    CHECK_NOTHROW(result =
                      Common::detail::ReserveCpusForCurrentThread(cpus->second, throwing.Get()));
    CHECK(!result);
    REQUIRE(child != 0);
    CheckMasksAsBefore(before);
    const cpu_set_t child_after = ThreadMask(child);
    CHECK(CPU_EQUAL(&child_after, &allowed));
    CheckReserveAndReleaseWork(cpus->second, before);
}

TEST_CASE("Core pinning: an exception in the undo's own listing ends the reservation without "
          "throwing",
          "[common][core_pinning]") {
    const auto cpus = ReservableCpus();
    if (!cpus) {
        SKIP("needs at least four CPUs");
    }
    const cpu_set_t allowed = ThreadMask(0);
    HelperThread creator{};
    const auto before = ProcessMasks();
    // Restores the main thread only: the helpers end with the test, and every other thread's mask
    // is checked below.
    SCOPE_EXIT {
        Common::ReleaseReservedCpus();
        sched_setaffinity(0, sizeof(allowed), &allowed);
    };
    // The second listing throws after visiting a new child born narrowed, and so does the undo's
    // listing: the stated residue. The saved masks are back, the child keeps its narrowed mask, and
    // nothing throws or stays reserved.
    int calls = 0;
    pid_t child = 0;
    TestAccess throwing;
    throwing.list = [&](const std::function<void(int)>& visit) -> bool {
        ++calls;
        if (calls == 2) {
            child = creator.Spawn();
            RealList(visit);
        }
        if (calls >= 2) {
            throw std::bad_alloc{};
        }
        return RealList(visit);
    };
    std::optional<size_t> result{0};
    CHECK_NOTHROW(result =
                      Common::detail::ReserveCpusForCurrentThread(cpus->second, throwing.Get()));
    CHECK(!result);
    CHECK(calls == 3);
    CheckMasksAsBefore(before);
    REQUIRE(child != 0);
    cpu_set_t narrowed = allowed;
    for (const u32 cpu : cpus->second) {
        CPU_CLR(cpu, &narrowed);
    }
    const cpu_set_t child_after = ThreadMask(child);
    CHECK(CPU_EQUAL(&child_after, &narrowed));
    // Nothing stayed reserved: a real reservation works.
    CheckReserveAndReleaseWork(cpus->second, before);
}
#endif
