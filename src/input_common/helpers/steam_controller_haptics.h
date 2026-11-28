// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "common/input.h"

namespace InputCommon {

namespace SteamHaptics {
constexpr std::size_t SamplesPerGrip = 31;
using Report = std::array<u8, 64>;

constexpr bool Supported(u16 product) {
    return product >= 0x1302 && product <= 0x1305;
}

constexpr u32 SampleRate(u16 product) {
    return product == 0x1302 ? 8000 : 4000;
}

// Worker-owned synthesis state. No allocation is needed to render a HID packet.
class Pcm {
public:
    explicit Pcm(u32 sample_rate);
    Report Render(const std::array<Common::Input::VibrationStatus, 2>& targets);

private:
    struct Band {
        f64 phase{};
        f32 amplitude{};
    };
    std::array<std::array<Band, 2>, 2> bands{};
    u32 rate;
    f32 smoothing;
};
} // namespace SteamHaptics

// One instance per physical SDL device. Callers serialize SetVibration and destruction;
// only the worker touches its HID handle. Setup never runs on the vibration caller.
class SteamControllerHaptics {
public:
    SteamControllerHaptics(std::string path, u16 product);
    ~SteamControllerHaptics();
    bool SetVibration(const Common::Input::VibrationStatus& vibration, bool enabled);
    bool HasFailed() const {
        return failed.load();
    }

private:
    void Close();
    void Run(std::stop_token stop);

    const std::string path;
    const u16 product;
    std::mutex mutex;
    std::condition_variable_any wake;
    std::array<Common::Input::VibrationStatus, 2> targets{};
    std::array<u64, 2> updated_at{};
    std::atomic<bool> failed{};
    std::jthread worker;
};

} // namespace InputCommon
