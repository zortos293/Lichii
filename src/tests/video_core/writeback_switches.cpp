// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/writeback_switches.h"

using VideoCommon::WritebackSwitches::AnyWritebackOn;
using VideoCommon::WritebackSwitches::SwitchOn;

namespace {
constexpr int Unset = -1;
constexpr int Off = 0;
constexpr int On = 1;
} // Anonymous namespace

TEST_CASE("A write-back switch is on for The Duskbloods when unset",
          "[video_core][texture_cache][writeback]") {
    REQUIRE(SwitchOn(Unset, true));
    REQUIRE_FALSE(SwitchOn(Unset, false));
    REQUIRE(SwitchOn(On, false));
    REQUIRE_FALSE(SwitchOn(Off, true));
}

TEST_CASE("Join write-backs alone still count as write-backs being on",
          "[video_core][texture_cache][writeback]") {
    // EDEN_NX2_JOIN_WRITEBACK=1 with EDEN_NX2_GC_DOWNLOAD_OVERLAPS=0: joins queue write-backs,
    // so the collector's synchronous downloads and the flushes must still be kept from them.
    REQUIRE(AnyWritebackOn(Off, On, false));
    REQUIRE(AnyWritebackOn(Off, On, true));
    REQUIRE(AnyWritebackOn(Off, Unset, true));
    // And the collector's alone.
    REQUIRE(AnyWritebackOn(On, Off, false));
    REQUIRE(AnyWritebackOn(Unset, Off, true));
}

TEST_CASE("Write-backs are off when both switches are",
          "[video_core][texture_cache][writeback]") {
    REQUIRE_FALSE(AnyWritebackOn(Off, Off, true));
    REQUIRE_FALSE(AnyWritebackOn(Unset, Unset, false));
    REQUIRE_FALSE(AnyWritebackOn(Off, Unset, false));
}

static_assert(AnyWritebackOn(Off, On, false));
static_assert(!AnyWritebackOn(Off, Off, true));
