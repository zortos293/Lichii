// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>

namespace Core::HID {
class HIDCore;
}

namespace InputCommon {
class InputSubsystem;
}

/**
 * Moves a player whose controller went away (battery died, switched from Bluetooth to USB, another
 * pad picked up) onto a free connected controller, the way PCSX2 does. Only the live bindings
 * change: the saved configuration is untouched and comes back once its controller reconnects.
 */
class ControllerHotSwap {
public:
    void Update(Core::HID::HIDCore& hid_core, InputCommon::InputSubsystem& input_subsystem);

private:
    std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_check{};
    // Indexed like Settings::values.players: Player1-8, then Handheld.
    std::array<bool, 9> swapped{};
};
