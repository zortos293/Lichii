// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/writeback_switches.h"

TEST_CASE("Overlap write-backs stay on only for the tested titles",
          "[video_core][texture_cache][writeback]") {
    using namespace VideoCommon::WritebackSwitches;
    REQUIRE(WritebackDefault(DuskbloodsNetworkTestProgramId));
    REQUIRE(WritebackDefault(KirbyForgottenLandNs2eProgramId));
    REQUIRE_FALSE(WritebackDefault(0));
    REQUIRE_FALSE(WritebackDefault(0x01004D300C5AE000ULL));
    REQUIRE_FALSE(WritebackDefault(0x04007EF01F8AE000ULL));
    REQUIRE_FALSE(WritebackDefault(0x0400486024DEC000ULL));
}

TEST_CASE("EDEN_NX2_OOT_STALE_WRITEBACK takes 1 or 2 and nothing else",
          "[video_core][texture_cache][writeback]") {
    using namespace VideoCommon::WritebackSwitches;
    REQUIRE(ParseOcarinaStaleWriteback(nullptr) == OcarinaStaleWriteback::Off);
    REQUIRE(ParseOcarinaStaleWriteback("") == OcarinaStaleWriteback::Off);
    REQUIRE(ParseOcarinaStaleWriteback("0") == OcarinaStaleWriteback::Off);
    REQUIRE(ParseOcarinaStaleWriteback("1") == OcarinaStaleWriteback::Sampled);
    REQUIRE(ParseOcarinaStaleWriteback("2") == OcarinaStaleWriteback::Default);
    REQUIRE(ParseOcarinaStaleWriteback("3") == OcarinaStaleWriteback::Off);
    REQUIRE(ParseOcarinaStaleWriteback("12") == OcarinaStaleWriteback::Off);
}
