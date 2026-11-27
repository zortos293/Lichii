// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace Dynarmic::Backend::Arm64::JitTrace {

/// DYNARMIC_JIT_TRACE=<file>: records what a program asks of the arm64 code cache, so that the cache can be
/// benchmarked afterwards with tests/A64 jit_cache_replay and no program at all. Recorded: the block lookups that
/// miss a Jit's own table (hits there cost nothing and are not), every block compiled with the guest instructions
/// it was translated from, fastmem faults, invalidated ranges and cache clears; each with the time and the Jit.
///
/// File: records of 32 bytes, little endian: u8 type, u8 jit, u16 flags, u32 count, u64 time_ns, u64 a, u64 b;
/// a Compile record is followed by `count` pairs of (u64 guest address, u64 instruction word).
enum class Type : u8 {
    Header = 0,      ///< a = code cache size, flags = 1 if shared between Jits
    Lookup = 1,      ///< a = location descriptor; flags: 0 found, 1 compiled here, 2 translated here but found emitted
    Compile = 2,     ///< a = location descriptor, count = instructions that follow
    Fault = 3,       ///< a = location descriptor of the block, b = index of the access; flags: 1 the code was stale, 2 a block was dropped
    Invalidate = 4,  ///< a = start address, b = length
    Clear = 5,       ///< flags: 1 the cache was full
};

struct Record {
    u8 type;
    u8 jit;
    u16 flags;
    u32 count;
    u64 time_ns;
    u64 a;
    u64 b;
};
static_assert(sizeof(Record) == 32);

inline std::FILE* File() {
    static std::FILE* const file = [] {
        const char* const path = std::getenv("DYNARMIC_JIT_TRACE");
        std::FILE* const opened = path && *path ? std::fopen(path, "wb") : nullptr;
        if (opened) {
            std::setvbuf(opened, nullptr, _IOFBF, 8 << 20);
        }
        return opened;
    }();
    return file;
}

inline bool Enabled() {
    return File() != nullptr;
}

inline void Write(Type type, u32 jit, u16 flags, u64 a, u64 b, const std::vector<std::pair<u64, u64>>* code = nullptr) {
    std::FILE* const file = File();
    if (!file) {
        return;
    }
    static const auto start = std::chrono::steady_clock::now();
    static std::mutex mutex;
    const Record record{
        static_cast<u8>(type), static_cast<u8>(jit), flags, code ? static_cast<u32>(code->size()) : 0,
        static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
        a, b};
    std::scoped_lock lock{mutex};
    std::fwrite(&record, sizeof(record), 1, file);
    if (code && !code->empty()) {
        std::fwrite(code->data(), sizeof(code->front()), code->size(), file);
    }
    if (type == Type::Clear || type == Type::Header) {
        std::fflush(file);
    }
}

}  // namespace Dynarmic::Backend::Arm64::JitTrace
