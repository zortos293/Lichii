// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <utility>

#include "common/common_types.h"

namespace Vulkan {

class PipelineBuildTraceWriter {
public:
    /// The caller owns the file and keeps it open until every traced call has ended.
    explicit PipelineBuildTraceWriter(std::FILE* file_)
        : file{file_}, session_ns{file ? Now() : 0} {}

    bool Enabled() const {
        return file != nullptr;
    }

    static u64 Now() {
        // The same epoch and units as nx2_gsv.h's guest-present timestamp ring.
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    u64 NextCall() {
        return next_call.fetch_add(1, std::memory_order_relaxed);
    }

    void Write(const char* phase, const char* kind, u64 key_hash, std::span<const u64> stage_hashes,
               bool unoptimized, const char* role, u64 call, u64 timestamp_ns,
               u64 call_begin_ns = 0, const char* outcome = nullptr) {
        std::scoped_lock lock{mutex};
        std::fprintf(file, "%s %s pipeline=%016llx thread=%zu%s", phase, kind,
                     static_cast<unsigned long long>(key_hash),
                     std::hash<std::thread::id>{}(std::this_thread::get_id()),
                     unoptimized ? " unoptimized" : "");
        for (size_t stage = 0; stage < stage_hashes.size(); ++stage) {
            if (stage_hashes[stage] != 0) {
                std::fprintf(file, " s%zu:%016llx", stage,
                             static_cast<unsigned long long>(stage_hashes[stage]));
            }
        }
        if (outcome) {
            std::fprintf(file, " ms=%.1f", static_cast<double>(timestamp_ns - call_begin_ns) / 1e6);
        }
        std::fprintf(file, " ts_ns=%llu call=%llu session_ns=%llu",
                     static_cast<unsigned long long>(timestamp_ns),
                     static_cast<unsigned long long>(call),
                     static_cast<unsigned long long>(session_ns));
        if (role) {
            std::fprintf(file, " role=%s", role);
        }
        if (outcome) {
            std::fprintf(file, " call_begin_ns=%llu outcome=%s",
                         static_cast<unsigned long long>(call_begin_ns), outcome);
        }
        std::fputc('\n', file);
        std::fflush(file);
    }

private:
    std::FILE* const file;
    const u64 session_ns;
    std::atomic<u64> next_call{1};
    std::mutex mutex;
};

class PipelineBuildTraceScope {
public:
    PipelineBuildTraceScope(PipelineBuildTraceWriter& writer_, const char* kind_, u64 key_hash_,
                            std::span<const u64> stage_hashes_, bool unoptimized_,
                            const char* role_)
        : writer{writer_}, kind{kind_}, key_hash{key_hash_}, stage_hashes{stage_hashes_},
          unoptimized{unoptimized_}, role{role_}, call{writer.NextCall()},
          exceptions{std::uncaught_exceptions()} {
        writer.Write("BEGIN", kind, key_hash, stage_hashes, unoptimized, role, call, writer.Now());
        begin_ns = writer.Now();
    }

    ~PipelineBuildTraceScope() {
        const u64 end_ns = writer.Now();
        writer.Write("END", kind, key_hash, stage_hashes, unoptimized, role, call, end_ns, begin_ns,
                     std::uncaught_exceptions() > exceptions ? "exception" : "success");
    }

    PipelineBuildTraceScope(const PipelineBuildTraceScope&) = delete;
    PipelineBuildTraceScope& operator=(const PipelineBuildTraceScope&) = delete;

private:
    PipelineBuildTraceWriter& writer;
    const char* const kind;
    const u64 key_hash;
    const std::span<const u64> stage_hashes;
    const bool unoptimized;
    const char* const role;
    const u64 call;
    const int exceptions;
    u64 begin_ns{};
};

template <typename Function>
decltype(auto) TracePipelineBuild(PipelineBuildTraceWriter& writer, const char* kind, u64 key_hash,
                                  std::span<const u64> stage_hashes, bool unoptimized,
                                  Function&& build, const char* role = nullptr) {
    if (!writer.Enabled()) {
        return std::forward<Function>(build)();
    }
    const PipelineBuildTraceScope trace{writer, kind, key_hash, stage_hashes, unoptimized, role};
    return std::forward<Function>(build)();
}

inline PipelineBuildTraceWriter& GetPipelineBuildTraceWriter() {
    static PipelineBuildTraceWriter writer{[] {
        const char* const path = std::getenv("EDEN_NX2_PIPELINE_BUILD_TRACE");
        return path != nullptr && *path != '\0' ? std::fopen(path, "a") : nullptr;
    }()};
    return writer;
}

/// EDEN_NX2_PIPELINE_BUILD_TRACE=<file>: preserve BEGIN/END fields and append timestamps and call
/// IDs. An unmatched BEGIN identifies a crash or unfinished call; exceptions get an END outcome.
/// END's call_begin_ns and ts_ns exclude the trace's own output from ms, but still include driver
/// contention. BEGIN is flushed before the call starts. A disabled trace reads no clock or ID.
/// A call through PipelineBinaries also includes its lookup, retries and binary capture/store.
template <typename Function>
decltype(auto) TracePipelineBuild(const char* kind, u64 key_hash, std::span<const u64> stage_hashes,
                                  bool unoptimized, Function&& build, const char* role = nullptr) {
    return TracePipelineBuild(GetPipelineBuildTraceWriter(), kind, key_hash, stage_hashes,
                              unoptimized, std::forward<Function>(build), role);
}

} // namespace Vulkan
