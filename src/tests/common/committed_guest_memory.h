// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include "core/device_memory.h"

namespace Tests {

/// Guest memory at the start of the backing that fixtures write without a kernel allocation.
inline constexpr std::size_t FixtureGuestMemory = 0x100000;

/// On Windows the guest memory section only reserves its pages: commit what a fixture writes
/// directly, so a test doesn't lean on the fault handler and a real miss still shows.
inline void CommitFixtureMemory(Core::DeviceMemory& memory,
                                std::size_t size = FixtureGuestMemory) {
    REQUIRE(memory.buffer.CommitBacking(0, size));
}

} // namespace Tests
