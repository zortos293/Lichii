// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/writeback_switches.h"

TEST_CASE("Overlap write-backs stay on only for the tested titles",
          "[video_core][texture_cache][writeback]") {
    using namespace VideoCommon::WritebackSwitches;
    using Settings::RendererBackend;
    for (const RendererBackend backend :
         {RendererBackend::Vulkan, RendererBackend::OpenGL_GLSL, RendererBackend::OpenGL_GLASM,
          RendererBackend::OpenGL_SPIRV}) {
        REQUIRE(WritebackDefault(DuskbloodsNetworkTestProgramId, backend));
        REQUIRE(WritebackDefault(KirbyForgottenLandNs2eProgramId, backend));
        REQUIRE_FALSE(WritebackDefault(0, backend));
        REQUIRE_FALSE(WritebackDefault(0x01004D300C5AE000ULL, backend));
        // Ocarina of Time: write-backs regress Castle Town (#2038), and #818's stale-upload skip
        // is its frame rate.
        REQUIRE_FALSE(WritebackDefault(0x0400486024DEC000ULL, backend));
    }
    // Fortune's Weave: Vulkan only, stale-page uploads included.
    REQUIRE(WritebackDefault(FireEmblemFortunesWeaveProgramId, RendererBackend::Vulkan));
    REQUIRE_FALSE(WritebackDefault(FireEmblemFortunesWeaveProgramId, RendererBackend::OpenGL_GLSL));
    REQUIRE_FALSE(
        WritebackDefault(FireEmblemFortunesWeaveProgramId, RendererBackend::OpenGL_GLASM));
    REQUIRE_FALSE(
        WritebackDefault(FireEmblemFortunesWeaveProgramId, RendererBackend::OpenGL_SPIRV));
}
