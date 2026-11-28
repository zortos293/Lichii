// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <numbers>
#include <system_error>
#include <utility>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_hidapi.h>
#include <SDL3/SDL_timer.h>

#include "common/logging.h"
#include "input_common/helpers/steam_controller_haptics.h"

namespace InputCommon {
namespace {
using Common::Input::VibrationStatus;
using SteamHaptics::Report;

VibrationStatus Sanitize(VibrationStatus value) {
    const auto amplitude = [](f32 a) {
        return std::isfinite(a) ? std::clamp(a, 0.0f, 1.0f) : 0.0f;
    };
    const auto frequency = [](f32 f) {
        return std::isfinite(f) ? std::clamp(f, 0.0f, 1253.0f) : 0.0f;
    };
    value.low_frequency = frequency(value.low_frequency);
    value.high_frequency = frequency(value.high_frequency);
    value.low_amplitude = value.low_frequency > 0 ? amplitude(value.low_amplitude) : 0;
    value.high_amplitude = value.high_frequency > 0 ? amplitude(value.high_amplitude) : 0;
    return value;
}

bool Active(const std::array<VibrationStatus, 2>& targets) {
    return std::ranges::any_of(
        targets, [](const auto& v) { return v.low_amplitude > 0 || v.high_amplitude > 0; });
}

u8 MuLaw(f32 sample) {
    const int pcm = static_cast<int>(std::lround(std::clamp(sample, -1.0f, 1.0f) * 32767));
    const int magnitude = std::min(std::abs(pcm), 32635) + 132;
    int exponent = 7;
    for (int mask = 0x4000; exponent > 0 && !(magnitude & mask); mask >>= 1) {
        --exponent;
    }
    return static_cast<u8>(
        ~((pcm < 0 ? 0x80 : 0) | (exponent << 4) | ((magnitude >> (exponent + 3)) & 15)));
}

bool Write(SDL_hid_device* device, const Report& report) {
    // Windows can return the padded output report length.
    return SDL_hid_write(device, report.data(), report.size()) >= static_cast<int>(report.size());
}

bool Mode(SDL_hid_device* device, u8 operation, u8 format) {
    bool success = true;
    for (u8 channel : {u8{2}, u8{5}}) {
        Report report{0x86, operation, channel, format};
        success = Write(device, report) && success;
    }
    return success;
}

void Silence(SDL_hid_device* device, u8 format) {
    Mode(device, 1, format);
    Mode(device, 1, 0);
}
} // namespace

SteamHaptics::Pcm::Pcm(u32 sample_rate)
    : rate{sample_rate}, smoothing{1.0f - std::exp(-1000.0f / static_cast<f32>(rate))} {}

Report SteamHaptics::Pcm::Render(const std::array<VibrationStatus, 2>& targets) {
    Report report{0x88, static_cast<u8>(SamplesPerGrip)};
    constexpr f64 turn = 2.0 * std::numbers::pi;
    for (std::size_t side = 0; side < 2; ++side) {
        const auto target = Sanitize(targets[side]);
        const f32 low = std::sqrt(target.low_amplitude);
        const f32 high = std::sqrt(target.high_amplitude);
        // Keep headroom without making equal-band strength changes disappear.
        const f32 gain = 0.8f / (1.0f + low * high);
        const std::array amplitudes{low * gain, high * gain};
        const std::array steps{turn * target.low_frequency / rate,
                               turn * target.high_frequency / rate};
        for (std::size_t sample = 0; sample < SamplesPerGrip; ++sample) {
            f64 value = 0;
            for (std::size_t band = 0; band < 2; ++band) {
                auto& wave = bands[side][band];
                wave.amplitude += (amplitudes[band] - wave.amplitude) * smoothing;
                value += wave.amplitude * std::sin(wave.phase);
                wave.phase += steps[band];
                if (wave.phase >= turn) {
                    wave.phase -= turn;
                }
            }
            report[2 + side * SamplesPerGrip + sample] = MuLaw(static_cast<f32>(value));
        }
        for (auto& wave : bands[side]) {
            if (wave.amplitude < 0.000001f) {
                wave.amplitude = 0;
            }
        }
    }
    return report;
}

SteamControllerHaptics::SteamControllerHaptics(std::string path_, u16 product_)
    : path{std::move(path_)}, product{product_} {}

SteamControllerHaptics::~SteamControllerHaptics() {
    Close();
}

void SteamControllerHaptics::Close() {
    worker.request_stop();
    wake.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
    std::scoped_lock lock{mutex};
    targets = {};
    failed = false;
}

bool SteamControllerHaptics::SetVibration(const VibrationStatus& vibration, bool enabled) {
    if (!enabled) {
        Close();
        return false;
    }
    if (failed) {
        return false;
    }
    {
        std::scoped_lock lock{mutex};
        const auto value = Sanitize(vibration);
        const u64 now = SDL_GetTicksNS();
        if (vibration.side != Common::Input::VibrationSide::Right) {
            targets[0] = value;
            updated_at[0] = now;
        }
        if (vibration.side != Common::Input::VibrationSide::Left) {
            targets[1] = value;
            updated_at[1] = now;
        }
        if (!worker.joinable() && Active(targets)) {
            try {
                worker = std::jthread([this](std::stop_token stop) { Run(stop); });
            } catch (const std::system_error& error) {
                LOG_WARNING(Input, "Steam haptics thread: {}", error.what());
                failed = true;
                return false;
            }
        }
        if (Active(targets)) {
            wake.notify_one();
        }
    }
    return true;
}

void SteamControllerHaptics::Run(std::stop_token stop) {
    SDL_hid_device* device = SDL_hid_open_path(path.c_str());
    if (!device) {
        LOG_WARNING(Input, "Steam haptics HID open failed: {}", SDL_GetError());
        failed = true;
        return;
    }
    const u8 format = product == 0x1302 ? 8 : 9;
    const auto configure = [&] {
        if (!Write(device, Report{0x80}) || !Mode(device, 1, format) || !Mode(device, 1, 0) ||
            !Mode(device, 2, format)) {
            return false;
        }
        std::unique_lock lock{mutex};
        wake.wait_for(lock, stop, std::chrono::milliseconds{200}, [] { return false; });
        return !stop.stop_requested();
    };
    bool ready = configure();
    const u32 rate = SteamHaptics::SampleRate(product);
    SteamHaptics::Pcm pcm{rate};
    const u64 period = SteamHaptics::SamplesPerGrip * 1'000'000'000ULL / rate;
    u64 deadline = SDL_GetTicksNS();
    u64 last_active = deadline;
    unsigned failures = 0;
    bool recovered = false;
    while (ready && !stop.stop_requested()) {
        std::array<VibrationStatus, 2> snapshot;
        {
            std::unique_lock lock{mutex};
            const u64 now = SDL_GetTicksNS();
            for (std::size_t side = 0; side < targets.size(); ++side) {
                // Match SDL rumble's two-second lifetime independently for each grip.
                if (now - updated_at[side] >= 2'000'000'000) {
                    targets[side] = {};
                }
            }
            if (Active(targets)) {
                last_active = now;
            } else if (now - last_active >= 100'000'000) {
                wake.wait(lock, stop, [this] { return Active(targets); });
                deadline = SDL_GetTicksNS();
                last_active = deadline;
            }
            snapshot = targets;
        }
        if (stop.stop_requested()) {
            break;
        }
        failures = Write(device, pcm.Render(snapshot)) ? 0 : failures + 1;
        if (failures == 100) {
            ready = !recovered && configure();
            recovered = true;
            failures = 0;
            deadline = SDL_GetTicksNS();
        }
        deadline += period;
        const u64 now = SDL_GetTicksNS();
        if (deadline > now) {
            // SDL's high resolution sleep blocks; DelayPrecise finishes with a busy wait.
            SDL_DelayNS(deadline - now);
        } else if (now - deadline > period) {
            deadline = now;
        }
    }
    Silence(device, format);
    SDL_hid_close(device);
    if (!stop.stop_requested()) {
        LOG_WARNING(Input, "Steam haptics failed; using standard rumble on subsequent updates");
        failed = true;
    }
}

} // namespace InputCommon
