// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <sysinfoapi.h>
#include <psapi.h>
// clang-format on
#else
#include <sys/types.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#else
#include <unistd.h>
#endif
#endif

#include <charconv>
#include <string_view>

#ifdef __linux__
#include <fstream>
#include <iterator>
#include <string>
#endif

#if defined(__linux__) && defined(__GLIBC__)
#include <chrono>
#include <malloc.h>
#include <unistd.h>
#endif

#include "common/logging.h"
#include "common/memory_detect.h"

namespace Common {

// Detects the RAM and Swapfile sizes
static MemoryInfo Detect() {
    MemoryInfo mem_info{};

#ifdef _WIN32
    MEMORYSTATUSEX memorystatus;
    memorystatus.dwLength = sizeof(memorystatus);
    GlobalMemoryStatusEx(&memorystatus);
    mem_info.TotalPhysicalMemory = memorystatus.ullTotalPhys;
    mem_info.TotalSwapMemory = memorystatus.ullTotalPageFile - mem_info.TotalPhysicalMemory;
#elif defined(__APPLE__)
    u64 ramsize;
    struct xsw_usage vmusage;
    std::size_t sizeof_ramsize = sizeof(ramsize);
    std::size_t sizeof_vmusage = sizeof(vmusage);
    // hw and vm are defined in sysctl.h
    // https://github.com/apple/darwin-xnu/blob/master/bsd/sys/sysctl.h#L471
    // sysctlbyname(const char *, void *, size_t *, void *, size_t);
    sysctlbyname("hw.memsize", &ramsize, &sizeof_ramsize, nullptr, 0);
    sysctlbyname("vm.swapusage", &vmusage, &sizeof_vmusage, nullptr, 0);
    mem_info.TotalPhysicalMemory = ramsize;
    mem_info.TotalSwapMemory = vmusage.xsu_total;
#elif defined(__FreeBSD__)
    u_long physmem, swap_total;
    std::size_t sizeof_u_long = sizeof(u_long);
    // sysctlbyname(const char *, void *, size_t *, const void *, size_t);
    sysctlbyname("hw.physmem", &physmem, &sizeof_u_long, nullptr, 0);
    sysctlbyname("vm.swap_total", &swap_total, &sizeof_u_long, nullptr, 0);
    mem_info.TotalPhysicalMemory = physmem;
    mem_info.TotalSwapMemory = swap_total;
#elif defined(__linux__)
    struct sysinfo meminfo;
    sysinfo(&meminfo);
    mem_info.TotalPhysicalMemory = meminfo.totalram;
    mem_info.TotalSwapMemory = meminfo.totalswap;
#else
    mem_info.TotalPhysicalMemory = sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGE_SIZE);
    mem_info.TotalSwapMemory = 0;
#endif

    return mem_info;
}

const MemoryInfo& GetMemInfo() {
    static MemoryInfo mem_info = Detect();
    return mem_info;
}

ProcessMemoryUsage GetProcessMemoryUsage() {
    ProcessMemoryUsage usage{};
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX2 counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        usage.commit = counters.PrivateUsage;
        usage.working_set = counters.WorkingSetSize;
        usage.private_working_set = counters.PrivateWorkingSetSize;
    }
    PERFORMANCE_INFORMATION performance{};
    performance.cb = sizeof(performance);
    if (GetPerformanceInfo(&performance, sizeof(performance))) {
        usage.system_commit = u64{performance.CommitTotal} * performance.PageSize;
        usage.system_commit_limit = u64{performance.CommitLimit} * performance.PageSize;
    }
#elif defined(__linux__)
    const auto read = [](const char* path) {
        std::ifstream file{path};
        return std::string{std::istreambuf_iterator<char>{file}, {}};
    };
    const std::string status = read("/proc/self/status");
    usage.commit = ProcKilobytesField(status, "VmData:");
    usage.working_set = ProcKilobytesField(status, "VmRSS:");
    usage.private_working_set = ProcKilobytesField(status, "RssAnon:");
    // The memory trace printed "system commit 0 of 0 MiB" on Linux.
    const std::string meminfo = read("/proc/meminfo");
    usage.system_commit = ProcKilobytesField(meminfo, "Committed_AS:");
    usage.system_commit_limit = ProcKilobytesField(meminfo, "CommitLimit:");
#endif
    return usage;
}

std::optional<u64> GetSystemCommitRoom() {
#ifdef _WIN32
    PERFORMANCE_INFORMATION performance{};
    performance.cb = sizeof(performance);
    if (GetPerformanceInfo(&performance, sizeof(performance))) {
        const u64 limit = performance.CommitLimit;
        const u64 committed = performance.CommitTotal;
        return (committed < limit ? limit - committed : 0) * performance.PageSize;
    }
#endif
    return std::nullopt;
}

u64 ProcKilobytesField(std::string_view text, std::string_view key) {
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (!line.starts_with(key)) {
            continue;
        }
        line.remove_prefix(key.size());
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        u64 kilobytes = 0;
        const auto [ptr, error] =
            std::from_chars(line.data(), line.data() + line.size(), kilobytes);
        if (error != std::errc{} || ptr == line.data()) {
            return 0;
        }
        return kilobytes << 10;
    }
    return 0;
}

#if defined(__linux__) && defined(__GLIBC__)
namespace {
/// Resident set in bytes from /proc/self/statm (its second field, in pages), 0 when unreadable.
u64 ResidentBytes() {
    std::ifstream file{"/proc/self/statm"};
    u64 size = 0;
    u64 resident = 0;
    if (!(file >> size >> resident)) {
        return 0;
    }
    return resident * static_cast<u64>(sysconf(_SC_PAGESIZE));
}
} // Anonymous namespace
#endif

HeapTrimResult TrimHeap([[maybe_unused]] std::string_view reason) {
    HeapTrimResult result{};
#if defined(__linux__) && defined(__GLIBC__)
    result.resident_before = ResidentBytes();
    const auto start = std::chrono::steady_clock::now();
    const int released = malloc_trim(0);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    result.resident_after = ResidentBytes();
    const s64 handed_back =
        (static_cast<s64>(result.resident_before) - static_cast<s64>(result.resident_after)) >> 20;
    LOG_INFO(Common_Memory,
             "malloc_trim after {}: resident {} -> {} MiB ({} MiB handed back) in {:.1f} ms{}",
             reason, result.resident_before >> 20, result.resident_after >> 20, handed_back, ms,
             released != 0 ? "" : " (nothing to release)");
#endif
    return result;
}

} // namespace Common
