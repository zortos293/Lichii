// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/dkb_frame_trace.h"
#include "common/game_presets.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/hle/kernel/k_hardware_timer.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/nx2_thread_stats.h"
#include "core/hle/service/nx2_present_pacer.h"
#include "core/memory.h"
#include "core/perf_stats.h"
#include "video_core/gpu.h"
#include "video_core/host_vsync.h"
#include "video_core/nx2.h"
#include "video_core/renderer_base.h"

namespace Service {

/// Rate of the nx2:vi2 vsync event the game starts frames on: Settings frame_rate_limit, or
/// EDEN_NX2_VSYNC_HZ=<rate> for experiments.
inline u32 Nx2VsyncRateHz() {
    static const long long forced = [] {
        const char* text = std::getenv("EDEN_NX2_VSYNC_HZ");
        return text ? (std::max)(std::strtoll(text, nullptr, 10), 1LL) : 0LL;
    }();
    return forced != 0 ? static_cast<u32>(forced)
                       : (std::max<u32>)(Settings::values.frame_rate_limit.GetValue(), 1);
}

/**
 * The display HDR-change checks of the live nx2:vi2 sessions (INx2Vi2Session::NotifyHdrChange).
 * nx2:vi2's Composite runs its session's own; NVN direct mode's presents skip Composite and run
 * them all through Nx2NotifyHdrChange. A session is listed while it lives, and the mutex keeps it
 * alive through a check.
 */
struct Nx2HdrChangeChecks {
    std::mutex mutex;
    std::vector<std::pair<const void*, std::function<void()>>> checks; ///< by session
};
inline Nx2HdrChangeChecks g_nx2_hdr_change_checks;

/// Runs the HDR-change check of every live nx2:vi2 session, for a present made outside nx2:vi2.
inline void Nx2NotifyHdrChange() {
    std::scoped_lock lock{g_nx2_hdr_change_checks.mutex};
    for (const auto& check : g_nx2_hdr_change_checks.checks) {
        check.second();
    }
}

/**
 * What a Switch 2 game's present does besides showing its image, for both presenters: nx2:vi2's
 * PresentClientBuffer (the presents of the game's NVN driver) and NVN2 direct mode's
 * nvnQueuePresentTexture (core/nvn_hook.cpp). The present count and times the
 * bench and the debug tools read, thread statistics, pacing to the vblank, the frame capture, the
 * speed limiter and perf stats.
 */
class Nx2PresentBookkeeping {
public:
    explicit Nx2PresentBookkeeping(Core::System& system_) : system{system_} {}

    /// Counts a present made on `thread`. Returns its number among every present of the process
    /// (Core::g_nx2_present_count, from 1).
    u64 Count(Kernel::KThread& thread) {
        if (Kernel::Nx2ThreadStats::g_enabled) {
            Kernel::Nx2ThreadStats::OnPresent(&thread);
        }
        Core::RecordGuestPresent();
        return Core::g_nx2_present_count.load(std::memory_order_relaxed);
    }

    /**
     * Holds this present until the next vblank its word asks for, or for longer when an FPS limit
     * (the setting, or EDEN_NX2_FPS_LIMIT) is below that rate.
     *
     * Fire Emblem: Fortune's Weave sends 0x1003c during its boot logos and 0x2003c (two vblanks
     * per frame, 30 fps) after; paced at every vblank it ran at double speed.
     * EDEN_NX2_SWAP_INTERVAL=<n> forces the interval.
     *
     * @param timing  Present word 12: (swap interval << 16) | refresh rate.
     * @param process Owning process, for the Mario Kart World dynamic FPS case; may be null.
     * @param presents The presenter's own count of presents (for its log line).
     * @param vsync_owner The session's vsync state for HostVsync::SetPacing (the host display
     *                    starts the frames of a game with an unlocked frame rate); null when the
     *                    presenter has none (direct presents keep the vsync timer's rate).
     */
    void PaceToVblank(u32 timing, const Kernel::KProcess* process, u64 presents,
                      const void* vsync_owner) {
        auto& kernel = system.Kernel();
        const bool trace = Common::DkbFrameTrace::Enabled();
        if (trace) {
            if (frame_trace.previous_word && *frame_trace.previous_word != timing) {
                ++frame_trace.switches;
                frame_trace.switch_from = *frame_trace.previous_word;
                frame_trace.switch_to = timing;
            }
            frame_trace.previous_word = timing;
            frame_trace.target_ms = 0;
            frame_trace.sleep_requested_ms = 0;
            frame_trace.sleep_actual_ms = 0;
            frame_trace.deadline_reset = false;
        }
        static const u32 forced_interval = [] {
            const char* value = std::getenv("EDEN_NX2_SWAP_INTERVAL");
            return value ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 0U;
        }();
        const bool mkw_dynamic =
            process != nullptr && process->GetProgramId() == Settings::MarioKartWorldProgramId &&
            process->IsMkwDynamicFpsPatchApplied();
        if (process != nullptr && process->GetProgramId() == Settings::MarioKartWorldProgramId) {
            if (const u64 address = process->GetMkwFrameModeAddress(); address != 0) {
                UpdateMkwFrameMode(address, presents);
            }
        }
        u32 swap_interval = std::clamp<u32>(timing >> 16, 1, 4);
        if (mkw_dynamic) {
            swap_interval = 1;
        }
        if (forced_interval != 0) {
            swap_interval = forced_interval;
        }
        const u32 refresh_rate = (timing & 0xffff) >= 30 && (timing & 0xffff) <= 240 ? timing & 0xffff : 60;
        if (trace) {
            frame_trace.interval = swap_interval;
            frame_trace.refresh = refresh_rate;
            frame_trace.forced = forced_interval;
        }
        static const u32 forced_fps_limit = [] {
            const char* value = std::getenv("EDEN_NX2_FPS_LIMIT");
            return value ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 0U;
        }();
        const u32 fps_limit =
            forced_fps_limit != 0 ? forced_fps_limit : Settings::values.fps_limit.GetValue();
        // The display starts this session's frames (HostVsync) only for a game whose frame rate
        // is unlocked, no faster than the FPS limit. A locked game counts vsync events to time
        // itself, so it keeps the timer's fixed rate.
        if (vsync_owner != nullptr) {
            VideoCore::HostVsync::SetPacing(
                vsync_owner, process != nullptr && process->IsFrameRateUnlocked(), fps_limit);
        }
        // With a frame rate limit other than 60 Hz the vsync event already paces the game; holding
        // its swap interval here as well kept it at 60 fps (interval 2 of a 120 Hz event). Only an
        // explicit FPS limit still applies then.
        const bool timer_paced = Nx2VsyncRateHz() != 60;
        const bool speed_limited = !timer_paced && Settings::values.use_speed_limit.GetValue() &&
                                   Settings::SpeedLimit() != 0;
        if (!speed_limited && fps_limit == 0) {
            Tegra::g_nx2_guest_frame_interval_ns.store(0, std::memory_order_relaxed);
            return;
        }
        if (timing != std::exchange(m_swap_word, timing)) {
            LOG_INFO(Service,
                     "nx2:vi2 present #{} swap interval {} at {} Hz, FPS limit {} (word 12 = {:#x})",
                     presents, swap_interval, refresh_rate, fps_limit, timing);
        }
        const auto vblank_interval =
            timer_paced ? std::chrono::nanoseconds{0}
                        : std::chrono::nanoseconds{100'000'000'000LL * swap_interval /
                                                   refresh_rate /
                                                   (speed_limited ? Settings::SpeedLimit() : 100)};
        const auto interval = PacedInterval(vblank_interval, fps_limit);
        Tegra::g_nx2_guest_frame_interval_ns.store(
            static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(interval).count()),
            std::memory_order_relaxed);
        if (trace) {
            frame_trace.target_ms = std::chrono::duration<double, std::milli>(interval).count();
        }
        const auto now = std::chrono::steady_clock::now();
        const auto pacing = m_present_pacer.Advance(now, interval);
        if (trace) {
            frame_trace.deadline_reset = pacing.reset;
        }
        if (now < pacing.deadline) {
            // Sleep this service thread in the guest kernel. It runs on guest core 3, and a host
            // sleep held that core for the whole wait: game threads scheduled there ran only
            // between presents (Fortune's Weave's loader read one RomFS block per frame).
            const auto wait =
                std::chrono::duration_cast<std::chrono::nanoseconds>(pacing.deadline - now);
            const auto sleep_start = trace ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
            static_cast<void>(Kernel::GetCurrentThread(kernel).Sleep(
                kernel, kernel.HardwareTimer().GetTick() + wait.count() + 2));
            if (trace) {
                frame_trace.sleep_requested_ms =
                    std::chrono::duration<double, std::milli>(wait).count();
                frame_trace.sleep_actual_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - sleep_start).count();
            }
        }
    }

    /// After the frame was handed to the renderer (GPU::RequestComposite): the frame capture, the
    /// speed limiter and perf stats.
    void Finish(u64 presents) {
        CaptureFrame(presents);
        system.SpeedLimiter().DoSpeedLimiting(system.CoreTiming().GetGlobalTimeUs());
        system.GetPerfStats().EndSystemFrame();
        system.GetPerfStats().BeginSystemFrame();
    }

    /// EDEN DKB frame trace (Common::DkbFrameTrace): the pacing of the last present.
    struct FrameTrace {
        Common::DkbFrameTrace::RateLimit rate;
        std::chrono::steady_clock::time_point last_present{};
        std::optional<u32> previous_word;
        u64 switches{};
        u32 switch_from{}, switch_to{}, interval{}, refresh{}, forced{};
        double target_ms{}, sleep_requested_ms{}, sleep_actual_ms{};
        bool deadline_reset{};
    };
    FrameTrace frame_trace;

private:
    // Mario Kart World's 60 FPS mode with the 30 FPS fallback (loader/nso.cpp): the game reads its
    // frame mode from `address` each frame. Three frames in a row over 19 ms at 60 switch it to 30
    // (one 1/30 s step a frame, so it keeps normal speed); 60 is tried again after 2 s, and after
    // up to 16 s while tries keep failing within 5 s. A single hitch doesn't switch.
    struct MkwFrameMode {
        u32 mode{1};
        u32 late{};
        std::chrono::steady_clock::time_point last_present{};
        std::chrono::steady_clock::time_point since{};
        std::chrono::steady_clock::time_point retry{};
        std::chrono::seconds backoff{2};
    } m_mkw{};

    void UpdateMkwFrameMode(u64 address, u64 presents) {
        using namespace std::chrono_literals;
        const auto now = std::chrono::steady_clock::now();
        const auto gap = now - std::exchange(m_mkw.last_present, now);
        if (m_mkw.mode == 1) {
            m_mkw.late = gap > 19ms ? m_mkw.late + 1 : 0;
            if (m_mkw.late < 3) {
                return;
            }
            m_mkw.backoff = now - m_mkw.since < 5s
                                ? (std::min)(m_mkw.backoff * 2, std::chrono::seconds{16})
                                : std::chrono::seconds{2};
            m_mkw.mode = 2;
            m_mkw.late = 0;
            m_mkw.retry = now + m_mkw.backoff;
        } else if (now >= m_mkw.retry) {
            m_mkw.mode = 1;
        } else {
            return;
        }
        m_mkw.since = now;
        system.ApplicationMemory().Write32(address, m_mkw.mode);
        LOG_INFO(Service, "nx2:vi2 Mario Kart World frame mode {} FPS (present #{})",
                 m_mkw.mode == 1 ? 60 : 30, presents);
    }

    // EDEN_NX2_FRAME_CAPTURE=<dir>: every 120th present, save what the renderer shows as
    // nx2-frame-<n>.tga in <dir> (lets hidden test runs check the output; idea from changes.patch).
    void CaptureFrame(u64 presents) {
        static const char* const directory = std::getenv("EDEN_NX2_FRAME_CAPTURE");
        static const u64 every = [] {
            const char* value = std::getenv("EDEN_NX2_FRAME_CAPTURE_EVERY");
            return value != nullptr ? (std::max)(u64{1}, static_cast<u64>(std::strtoull(value, nullptr, 10))) : u64{120};
        }();
        if (directory == nullptr || (presents % every) != 1 % every ||
            m_capture_pending->exchange(true)) {
            return;
        }
        const auto layout = Layout::DefaultFrameLayout(1280, 720);
        auto pixels = std::make_shared<std::vector<u8>>(layout.width * layout.height * 4);
        const std::string path =
            fmt::format("{}/nx2-frame-{:05}.tga", directory, presents);
        auto pending = m_capture_pending; // the screenshot callback can outlive the session
        const bool requested = system.GPU().Renderer().RequestScreenshot(
            pixels->data(),
            [pixels, layout, path, pending](bool invert_y) {
                std::array<u8, 18> header{};
                header[2] = 2; // uncompressed true colour
                header[12] = static_cast<u8>(layout.width);
                header[13] = static_cast<u8>(layout.width >> 8);
                header[14] = static_cast<u8>(layout.height);
                header[15] = static_cast<u8>(layout.height >> 8);
                header[16] = 32;
                // Descriptor bit 5: rows top to bottom. invert_y means the data is bottom-up.
                header[17] = invert_y ? 0x08 : 0x28;
                if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(header.data(), 1, header.size(), file);
                    std::fwrite(pixels->data(), 1, pixels->size(), file);
                    std::fclose(file);
                }
                pending->store(false);
            },
            layout);
        if (!requested) {
            // Another screenshot (the frontend's, the debug server's) holds the renderer: this
            // capture is skipped, and its callback never runs to clear the flag.
            m_capture_pending->store(false);
        }
    }

    Core::System& system;
    // The console flips one frame per 60 Hz vblank and keeps the rest queued, so a game can't
    // present faster than that. Every slot is reported free here: hold each present until the
    // next vblank instead (title 4 ran at up to 160 fps, its timing and audio with it).
    Nx2PresentPacer m_present_pacer;
    u32 m_swap_word{};
    std::shared_ptr<std::atomic<bool>> m_capture_pending = std::make_shared<std::atomic<bool>>(false);
};

} // namespace Service
