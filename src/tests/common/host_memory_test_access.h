// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/host_memory.h"

namespace Common {

class HostMemoryTestAccess {
public:
    explicit HostMemoryTestAccess(bool fail_fallback = true)
        : previous{HostMemory::allocation_test_control},
          control{0x10000, 0x400000, true, fail_fallback} {
        HostMemory::allocation_test_control = &control;
    }

    ~HostMemoryTestAccess() {
        HostMemory::allocation_test_control = previous;
    }

    HostMemoryTestAccess(const HostMemoryTestAccess&) = delete;
    HostMemoryTestAccess& operator=(const HostMemoryTestAccess&) = delete;

    void AllowFallback() {
        control.fail_fallback = false;
    }

    void FailFallback() {
        control.fail_fallback = true;
    }

    size_t FastmemAttempts() const {
        return control.fastmem_attempts;
    }

    size_t FallbackAttempts() const {
        return control.fallback_attempts;
    }

    /// Makes every CommitBacking of the memory fail, as when Windows is out of commit.
    static void FailCommits(HostMemory& memory, bool fail) {
        memory.commits_left_for_test = fail ? 0 : HostMemory::NoCommitLimit;
    }

    /// Lets the next `count` CommitBacking calls of the memory succeed and fails the ones after.
    static void FailCommitsAfter(HostMemory& memory, size_t count) {
        memory.commits_left_for_test = count;
    }

private:
    HostMemory::AllocationTestControl* previous;
    HostMemory::AllocationTestControl control;
};

} // namespace Common
