// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "common/uuid.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_util.h"
#include "input_common/main.h"
#include "lichii/controller_hotswap.h"

namespace {

using namespace std::chrono_literals;

constexpr auto CheckInterval = 1s;
// Drivers that enumerate on their own thread (Joy-Con, GC adapter) need a moment after startup.
constexpr auto StartupGrace = 3s;
constexpr std::size_t HandheldIndex = 8;

struct Device {
    std::string engine;
    std::string guid;
    int port{};
    int pad{};

    bool operator==(const Device&) const = default;
};

struct Bindings {
    Core::HID::ButtonParams buttons;
    Core::HID::StickParams sticks;
    Core::HID::ControllerMotionParams motions;

    template <typename Func>
    void ForEach(Func&& func) {
        for (auto& param : buttons) {
            func(param);
        }
        for (auto& param : sticks) {
            func(param);
        }
        for (auto& param : motions) {
            func(param);
        }
    }

    bool operator==(const Bindings& other) const {
        const auto same = [](const auto& a, const auto& b) {
            return std::ranges::equal(
                a, b, [](const auto& x, const auto& y) { return x.Serialize() == y.Serialize(); });
        };
        return same(buttons, other.buttons) && same(sticks, other.sticks) &&
               same(motions, other.motions);
    }
};

/// A device in the list GetInputDevices returns. Dual Joy-Cons have two parts.
struct Connected {
    Common::ParamPackage params;
    std::vector<Device> parts;
};

bool IsSwappableEngine(const std::string& engine) {
    return engine == "sdl" || engine == "joycon" || engine == "gcpad";
}

Device ToDevice(const Common::ParamPackage& param, const std::string& guid_key = "guid") {
    return {param.Get("engine", ""), param.Get(guid_key, ""), param.Get("port", 0),
            param.Get("pad", 0)};
}

bool ReferencesDevice(const Common::ParamPackage& param, const Device& device) {
    return param.Has("engine") && ToDevice(param) == device;
}

std::vector<Device> MappedDevices(Bindings& bindings) {
    std::vector<Device> devices;
    bindings.ForEach([&](const Common::ParamPackage& param) {
        if (!IsSwappableEngine(param.Get("engine", ""))) {
            return;
        }
        const auto device = ToDevice(param);
        if (std::ranges::find(devices, device) == devices.end()) {
            devices.push_back(device);
        }
    });
    return devices;
}

Bindings FromSettings(const Settings::PlayerInput& player) {
    Bindings bindings;
    for (std::size_t i = 0; i < bindings.buttons.size(); ++i) {
        bindings.buttons[i] = Common::ParamPackage{player.buttons[i]};
    }
    for (std::size_t i = 0; i < bindings.sticks.size(); ++i) {
        bindings.sticks[i] = Common::ParamPackage{player.analogs[i]};
    }
    for (std::size_t i = 0; i < bindings.motions.size(); ++i) {
        bindings.motions[i] = Common::ParamPackage{player.motions[i]};
    }
    return bindings;
}

Bindings FromController(const Core::HID::EmulatedController& controller) {
    Bindings bindings;
    for (std::size_t i = 0; i < bindings.buttons.size(); ++i) {
        bindings.buttons[i] = controller.GetButtonParam(i);
    }
    for (std::size_t i = 0; i < bindings.sticks.size(); ++i) {
        bindings.sticks[i] = controller.GetStickParam(i);
    }
    for (std::size_t i = 0; i < bindings.motions.size(); ++i) {
        bindings.motions[i] = controller.GetMotionParam(i);
    }
    return bindings;
}

/// Same controller model, ignoring how it is attached. SDL GUIDs hold the bus (USB/Bluetooth),
/// vendor, product and firmware version, so the same pad gets a new GUID when plugged in by cable.
bool IsSameModel(const Device& a, const Device& b) {
    if (a.engine != b.engine) {
        return false;
    }
    if (a.engine != "sdl" || a.guid == b.guid) {
        return a.guid == b.guid;
    }
    const auto x = Common::UUID{a.guid}.uuid;
    const auto y = Common::UUID{b.guid}.uuid;
    const bool has_ids = (x[4] | x[5] | x[8] | x[9]) != 0;
    return has_ids && x[4] == y[4] && x[5] == y[5] && x[8] == y[8] && x[9] == y[9];
}

/// Keeps the user's custom mapping, pointing it at another pad of the same model.
Bindings Retarget(Bindings bindings, const Device& from, const Connected& to) {
    bindings.ForEach([&](Common::ParamPackage& param) {
        if (!ReferencesDevice(param, from)) {
            return;
        }
        param.Set("guid", to.params.Get("guid", ""));
        param.Set("port", to.params.Get("port", 0));
        if (to.params.Has("pad")) {
            param.Set("pad", to.params.Get("pad", 0));
        }
    });
    return bindings;
}

/// What picking the device in the "Input Device" list does, keeping keyboard and mouse bindings.
std::optional<Bindings> WithDefaults(Bindings bindings, const Connected& device,
                                     InputCommon::InputSubsystem& input_subsystem) {
    const auto buttons = input_subsystem.GetButtonMappingForDevice(device.params);
    if (buttons.empty()) {
        return std::nullopt;
    }
    const auto sticks = input_subsystem.GetAnalogMappingForDevice(device.params);
    const auto motions = input_subsystem.GetMotionMappingForDevice(device.params);

    bindings.ForEach([](Common::ParamPackage& param) {
        if (IsSwappableEngine(param.Get("engine", ""))) {
            param = {};
        }
    });
    for (const auto& [index, param] : buttons) {
        bindings.buttons[index] = param;
    }
    for (const auto& [index, param] : sticks) {
        bindings.sticks[index] = param;
    }
    for (const auto& [index, param] : motions) {
        bindings.motions[index] = param;
    }
    return bindings;
}

std::string DeviceNames(const std::vector<Device>& devices) {
    std::string names;
    for (const auto& device : devices) {
        if (!names.empty()) {
            names += ", ";
        }
        names += fmt::format("{} {} port {}", device.engine, device.guid, device.port);
    }
    return names;
}

struct Slot {
    std::size_t index{};
    Core::HID::EmulatedController* controller{};
    Bindings saved;
    std::vector<Device> saved_devices;
};

/// Player 1 and Handheld share one configuration page and usually the same pad.
bool ArePartners(std::size_t a, std::size_t b) {
    return (a == 0 && b == HandheldIndex) || (a == HandheldIndex && b == 0);
}

} // namespace

void ControllerHotSwap::Update(Core::HID::HIDCore& hid_core,
                               InputCommon::InputSubsystem& input_subsystem) {
    const auto now = std::chrono::steady_clock::now();
    if (now - start_time < StartupGrace || now - last_check < CheckInterval) {
        return;
    }
    last_check = now;

    const bool enabled = Settings::values.controller_hot_swap.GetValue();
    if (!enabled && std::ranges::none_of(swapped, [](bool value) { return value; })) {
        return;
    }

    std::vector<Connected> connected;
    for (const auto& params : input_subsystem.GetInputDevices()) {
        if (!IsSwappableEngine(params.Get("engine", ""))) {
            continue;
        }
        Connected entry{params, {ToDevice(params)}};
        if (params.Has("guid2")) {
            entry.parts.push_back(ToDevice(params, "guid2"));
        }
        connected.push_back(std::move(entry));
    }
    const auto is_present = [&](const Device& device) {
        return std::ranges::any_of(connected, [&](const Connected& entry) {
            return std::ranges::find(entry.parts, device) != entry.parts.end();
        });
    };
    const auto any_present = [&](const std::vector<Device>& devices) {
        return std::ranges::any_of(devices, is_present);
    };

    // Devices that already drive a player, with the player using them.
    std::vector<std::pair<std::size_t, Device>> in_use;
    std::vector<Slot> pending;
    const auto& players = Settings::values.players.GetValue();

    for (std::size_t index = 0; index < swapped.size(); ++index) {
        auto* const controller =
            hid_core.GetEmulatedController(Service::HID::IndexToNpadIdType(index));
        if (controller == nullptr || controller->IsConfiguring()) {
            continue;
        }

        Slot slot{index, controller, FromSettings(players[index]), {}};
        slot.saved_devices = MappedDevices(slot.saved);
        auto live = FromController(*controller);

        const auto restore = [&](const char* reason) {
            if (!(live == slot.saved)) {
                LOG_INFO(Input, "Controller hot-swap: player {} back on its saved bindings ({})",
                         index + 1, reason);
                controller->SetInputParams(slot.saved.buttons, slot.saved.sticks,
                                           slot.saved.motions);
                live = slot.saved;
            }
            swapped[index] = false;
        };

        if (!enabled || slot.saved_devices.empty()) {
            if (swapped[index]) {
                restore(enabled ? "no controller in the saved bindings" : "hot-swap turned off");
            }
            continue;
        }
        if (!controller->IsConnected()) {
            continue;
        }

        const auto live_devices = MappedDevices(live);
        if (any_present(live_devices)) {
            for (const auto& device : live_devices) {
                in_use.emplace_back(index, device);
            }
            continue;
        }
        if (any_present(slot.saved_devices)) {
            restore("saved controller is connected");
            for (const auto& device : slot.saved_devices) {
                in_use.emplace_back(index, device);
            }
            continue;
        }
        pending.push_back(std::move(slot));
    }

    for (auto& slot : pending) {
        const auto used_by_other = [&](const Device& device) {
            return std::ranges::any_of(in_use, [&](const auto& use) {
                return use.second == device && use.first != slot.index &&
                       !ArePartners(use.first, slot.index);
            });
        };
        const auto used_by_partner = [&](const Connected& entry) {
            return std::ranges::any_of(in_use, [&](const auto& use) {
                return ArePartners(use.first, slot.index) &&
                       std::ranges::find(entry.parts, use.second) != entry.parts.end();
            });
        };
        const auto is_single_of_pair = [&](const Connected& entry) {
            return entry.parts.size() == 1 &&
                   std::ranges::any_of(connected, [&](const Connected& other) {
                       return other.parts.size() == 2 &&
                              std::ranges::find(other.parts, entry.parts[0]) != other.parts.end();
                   });
        };

        std::vector<std::pair<int, const Connected*>> candidates;
        for (const auto& entry : connected) {
            if (std::ranges::any_of(entry.parts, used_by_other) || is_single_of_pair(entry)) {
                continue;
            }
            const bool same_model = slot.saved_devices.size() == 1 && entry.parts.size() == 1 &&
                                    IsSameModel(slot.saved_devices[0], entry.parts[0]);
            candidates.emplace_back((used_by_partner(entry) ? 2 : 0) + (same_model ? 1 : 0),
                                    &entry);
        }
        std::ranges::stable_sort(candidates, std::greater{},
                                 [](const auto& candidate) { return candidate.first; });

        const Connected* best = nullptr;
        bool same_model = false;
        std::optional<Bindings> bindings;
        for (const auto& [rank, entry] : candidates) {
            same_model = (rank & 1) != 0;
            bindings = same_model
                           ? std::optional{Retarget(slot.saved, slot.saved_devices[0], *entry)}
                           : WithDefaults(slot.saved, *entry, input_subsystem);
            if (bindings) {
                best = entry;
                break;
            }
        }
        if (best == nullptr) {
            continue;
        }

        LOG_INFO(Input, "Controller hot-swap: player {} ({} missing) now uses {} ({})",
                 slot.index + 1, DeviceNames(slot.saved_devices),
                 best->params.Get("display", "controller"),
                 same_model ? "same model, custom mapping kept" : "default mapping");
        slot.controller->SetInputParams(bindings->buttons, bindings->sticks, bindings->motions);
        swapped[slot.index] = true;
        for (const auto& device : best->parts) {
            in_use.emplace_back(slot.index, device);
        }
    }
}
