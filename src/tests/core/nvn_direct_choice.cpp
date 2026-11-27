// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/nvn_direct_choice.h"

namespace {
using Core::NvnHook::ChooseRenderer;
using Core::NvnHook::RendererChoice;
using Core::NvnHook::RendererInputs;

/// A 64-bit NVN game on Vulkan, in a build with the direct renderer.
constexpr RendererInputs Runnable{
    .built = true,
    .override_ = std::nullopt,
    .setting = true,
    .vulkan = true,
    .is_64bit = true,
    .has_nvn = true,
    .fastmem = true,
    .replay_failed = false,
    .not_reset = false,
};
} // namespace

TEST_CASE("The setting and the override choose the renderer", "[nvn_direct_choice]") {
    REQUIRE(ChooseRenderer(Runnable) == RendererChoice::Setting);

    RendererInputs off = Runnable;
    off.setting = false;
    REQUIRE(ChooseRenderer(off) == RendererChoice::SettingOff);

    // EDEN_NX2_NVN_DIRECT wins over the setting both ways.
    off.override_ = true;
    REQUIRE(ChooseRenderer(off) == RendererChoice::Override);
    RendererInputs on = Runnable;
    on.override_ = false;
    REQUIRE(ChooseRenderer(on) == RendererChoice::OverrideOff);
}

TEST_CASE("A game the direct renderer cannot run falls back", "[nvn_direct_choice]") {
    const auto with = [](auto change) {
        RendererInputs inputs = Runnable;
        change(inputs);
        return ChooseRenderer(inputs);
    };
    REQUIRE(with([](RendererInputs& in) { in.built = false; }) == RendererChoice::NotBuilt);
    REQUIRE(with([](RendererInputs& in) { in.vulkan = false; }) == RendererChoice::NotVulkan);
    REQUIRE(with([](RendererInputs& in) { in.is_64bit = false; }) == RendererChoice::Not64Bit);
    REQUIRE(with([](RendererInputs& in) { in.has_nvn = false; }) == RendererChoice::NoNvn);
    REQUIRE(with([](RendererInputs& in) {
                in.override_ = true;
                in.fastmem = false;
            }) == RendererChoice::NoFastmem);
    // A start that failed on the device stays the reason for the later boots of the process.
    REQUIRE(with([](RendererInputs& in) { in.replay_failed = true; }) ==
            RendererChoice::ReplayFailed);
    // A build that cannot reset the direct renderer's state: the first game had it, later ones do not.
    REQUIRE(with([](RendererInputs& in) { in.not_reset = true; }) == RendererChoice::NotReset);
    // The override cannot force a game the direct renderer cannot run.
    REQUIRE(with([](RendererInputs& in) {
                in.override_ = true;
                in.is_64bit = false;
            }) == RendererChoice::Not64Bit);
    // Nothing asked for: no fallback to report, whatever the game.
    REQUIRE(with([](RendererInputs& in) {
                in.setting = false;
                in.has_nvn = false;
            }) == RendererChoice::SettingOff);
}

TEST_CASE("Only refusals of an asked-for direct renderer are fallbacks", "[nvn_direct_choice]") {
    using Core::NvnHook::IsDirect;
    using Core::NvnHook::IsFallback;
    REQUIRE(IsDirect(RendererChoice::Setting));
    REQUIRE(IsDirect(RendererChoice::Override));
    REQUIRE_FALSE(IsFallback(RendererChoice::Setting));
    REQUIRE_FALSE(IsFallback(RendererChoice::SettingOff));
    REQUIRE_FALSE(IsFallback(RendererChoice::OverrideOff));
    REQUIRE(IsFallback(RendererChoice::ReplayFailed));
    REQUIRE_FALSE(IsDirect(RendererChoice::ReplayFailed));
    REQUIRE(IsFallback(RendererChoice::NotReset));
    REQUIRE_FALSE(IsDirect(RendererChoice::NotReset));
}

TEST_CASE("A later game reuses the reset renderer or relaunches once", "[nvn_direct_choice]") {
    using Core::NvnHook::FreshProcessInputs;
    using Core::NvnHook::NeedsFreshProcess;
    constexpr FreshProcessInputs Reset{
        .direct_asked = true,
        .vulkan = true,
        .replay_failed = false,
        .not_reset = false,
        .relaunched = false,
    };
    // The earlier game's renderer was reset: the next game runs on it in this process.
    REQUIRE_FALSE(NeedsFreshProcess(Reset));
    REQUIRE(ChooseRenderer(Runnable) == RendererChoice::Setting);

    // It could not be reset: the frontend relaunches for the game instead of falling back.
    FreshProcessInputs not_reset = Reset;
    not_reset.not_reset = true;
    REQUIRE(NeedsFreshProcess(not_reset));

    // In the relaunched process it could not be reset either: no second relaunch, a fallback.
    FreshProcessInputs relaunched = not_reset;
    relaunched.relaunched = true;
    REQUIRE_FALSE(NeedsFreshProcess(relaunched));
    RendererInputs child = Runnable;
    child.not_reset = true;
    REQUIRE(ChooseRenderer(child) == RendererChoice::NotReset);

    // A replay that failed on the device is not retried in a fresh process.
    FreshProcessInputs failed = not_reset;
    failed.replay_failed = true;
    REQUIRE_FALSE(NeedsFreshProcess(failed));
    // Nor is a boot that does not ask for the direct renderer, or one not on Vulkan.
    FreshProcessInputs not_asked = not_reset;
    not_asked.direct_asked = false;
    REQUIRE_FALSE(NeedsFreshProcess(not_asked));
    FreshProcessInputs not_vulkan = not_reset;
    not_vulkan.vulkan = false;
    REQUIRE_FALSE(NeedsFreshProcess(not_vulkan));
}
