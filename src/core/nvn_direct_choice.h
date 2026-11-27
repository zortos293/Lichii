// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string_view>

#include "common/common_types.h"

namespace Core::NvnHook {

/// Which renderer a boot's game runs on, and why (Core::NvnHook::Install decides once per boot).
enum class RendererChoice : u8 {
    SettingOff,  ///< GPU emulation: the nvn_direct_renderer setting is off
    OverrideOff, ///< GPU emulation: EDEN_NX2_NVN_DIRECT=0
    Setting,     ///< NVN direct renderer: the setting
    Override,    ///< NVN direct renderer: EDEN_NX2_NVN_DIRECT=1
    // Asked for, but the direct renderer cannot run the game: GPU emulation instead.
    NotBuilt,    ///< this build has no NVN direct renderer
    NotVulkan,   ///< the renderer backend is not Vulkan
    Not64Bit,    ///< a 32-bit game: the NVN hook covers A64 code only
    NoNvn,       ///< the game exports no nvnBootstrapLoader
    NoFastmem,   ///< there is no host mapping of the application's guest address space
    ReplayFailed, ///< the direct renderer could not start on the Vulkan device (in this process)
    NotReset,     ///< an earlier game's direct renderer could not be reset (its log says why)
};

struct RendererInputs {
    bool built;                    ///< LICHII_NVN_DIRECT
    std::optional<bool> override_; ///< EDEN_NX2_NVN_DIRECT=0/1
    bool setting;                  ///< nvn_direct_renderer, the game's value
    bool vulkan;
    bool is_64bit;
    bool has_nvn; ///< nvnBootstrapLoader found
    bool fastmem;
    bool replay_failed; ///< an earlier start of the direct renderer in this process failed
    bool not_reset;     ///< an earlier game's direct renderer state could not be reset
};

[[nodiscard]] constexpr RendererChoice ChooseRenderer(const RendererInputs& in) {
    if (in.override_ == false) {
        return RendererChoice::OverrideOff;
    }
    if (!in.override_ && !in.setting) {
        return RendererChoice::SettingOff;
    }
    if (!in.built) {
        return RendererChoice::NotBuilt;
    }
    if (!in.vulkan) {
        return RendererChoice::NotVulkan;
    }
    if (!in.is_64bit) {
        return RendererChoice::Not64Bit;
    }
    if (!in.has_nvn) {
        return RendererChoice::NoNvn;
    }
    if (!in.fastmem) {
        return RendererChoice::NoFastmem;
    }
    if (in.replay_failed) {
        return RendererChoice::ReplayFailed;
    }
    if (in.not_reset) {
        return RendererChoice::NotReset;
    }
    return in.override_ ? RendererChoice::Override : RendererChoice::Setting;
}

[[nodiscard]] constexpr bool IsDirect(RendererChoice choice) {
    return choice == RendererChoice::Setting || choice == RendererChoice::Override;
}

struct FreshProcessInputs {
    bool direct_asked; ///< the next boot asks for the direct renderer (setting or override)
    bool vulkan;
    bool replay_failed; ///< an earlier start of the direct renderer in this process failed
    bool not_reset;     ///< an earlier game's direct renderer state could not be reset
    bool relaunched;    ///< this process is itself such a fresh process (LICHII_RELAUNCH_WAIT_PID)
};

/// A frontend launch runs the game in a fresh process (MainWindow::RelaunchForGame) instead of
/// falling back with RendererChoice::NotReset. Once per chain: a relaunched process that cannot
/// reset the renderer falls back, as does a replay that failed on the device.
[[nodiscard]] constexpr bool NeedsFreshProcess(const FreshProcessInputs& in) {
    return in.direct_asked && in.vulkan && in.not_reset && !in.replay_failed && !in.relaunched;
}

/// The direct renderer was asked for and the game fell back to GPU emulation.
[[nodiscard]] constexpr bool IsFallback(RendererChoice choice) {
    return choice >= RendererChoice::NotBuilt;
}

/// Why, for the log.
[[nodiscard]] constexpr std::string_view Describe(RendererChoice choice) {
    switch (choice) {
    case RendererChoice::SettingOff:
        return "the NVN direct renderer setting is off";
    case RendererChoice::OverrideOff:
        return "EDEN_NX2_NVN_DIRECT=0";
    case RendererChoice::Setting:
        return "the NVN direct renderer setting";
    case RendererChoice::Override:
        return "EDEN_NX2_NVN_DIRECT=1";
    case RendererChoice::NotBuilt:
        return "fallback: this build has no NVN direct renderer";
    case RendererChoice::NotVulkan:
        return "fallback: the NVN direct renderer needs the Vulkan backend";
    case RendererChoice::Not64Bit:
        return "fallback: a 32-bit game (the NVN hook covers 64-bit code only)";
    case RendererChoice::NoNvn:
        return "fallback: the game does not use NVN (no nvnBootstrapLoader)";
    case RendererChoice::NoFastmem:
        return "fallback: the NVN direct renderer needs a usable fastmem arena";
    case RendererChoice::ReplayFailed:
        return "fallback: the NVN direct renderer could not start on this GPU (it needs Vulkan "
               "1.3; the replay's log lines say what is missing)";
    case RendererChoice::NotReset:
        return "fallback: an earlier game in this process used the NVN direct renderer and this "
               "could not be reset (the NVN direct log lines at its stop say why)";
    }
    return "?";
}

} // namespace Core::NvnHook
