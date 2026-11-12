// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "input_common/any_input.h"

namespace InputCommon {
namespace {
using Common::Input::CallbackStatus;
using Common::Input::InputDevice;
using Common::Input::InputType;

struct MappedDevice {
    Common::ParamPackage identity;
    ButtonMapping buttons;
    AnalogMapping sticks;
    MotionMapping motions;
};

std::string Identity(const Common::ParamPackage& p) {
    return p.Get("engine", "") + "/" + p.Get("guid", "") + "/" + p.Get("guid2", "") + "/" +
           std::to_string(p.Get("port", 0)) + "/" + std::to_string(p.Get("pad", 0));
}

bool Matches(const Common::ParamPackage& binding, const Common::ParamPackage& device) {
    return binding.Get("engine", "") == device.Get("engine", "") &&
           binding.Get("guid", "") == device.Get("guid", "") &&
           binding.Get("port", 0) == device.Get("port", 0) &&
           binding.Get("pad", 0) == device.Get("pad", 0);
}

float AxisMagnitude(const Common::Input::AnalogStatus& axis) {
    const float value = std::abs(axis.raw_value - axis.properties.offset);
    return value > axis.properties.deadzone ? value : 0.0f;
}

float Activity(const CallbackStatus& status) {
    switch (status.type) {
    case InputType::Button:
        return status.button_status.value != status.button_status.inverted ? 1.0f : 0.0f;
    case InputType::Analog: {
        const auto& axis = status.analog_status;
        const auto& p = axis.properties;
        const float raw = axis.raw_value - p.offset;
        const float magnitude = std::abs(raw);
        float value = 0.0f;
        if (magnitude > p.deadzone && p.deadzone < 1.0f && p.range > 0.0f) {
            value = std::copysign((magnitude - p.deadzone) / (1.0f - p.deadzone) / p.range, raw);
            if (p.inverted)
                value = -value;
        }
        const bool pressed = (value > p.threshold) != p.inverted_button;
        return pressed ? 1.0f : 0.0f;
    }
    case InputType::Trigger:
        return status.trigger_status.pressed.value ? 1.0f : 0.0f;
    case InputType::Stick: {
        const float x = AxisMagnitude(status.stick_status.x);
        const float y = AxisMagnitude(status.stick_status.y);
        return x * x + y * y;
    }
    default:
        return 0.0f;
    }
}

struct State : std::enable_shared_from_this<State> {
    struct Source {
        std::shared_ptr<InputDevice> input;
        CallbackStatus status{};
    };

    explicit State(const Common::ParamPackage& params)
        : base(params.Get("base", "")), keyboard(params.Get("keyboard", "")),
          kind(params.Get("kind", "button")), index(params.Get("index", 0)) {}

    CallbackStatus Current() const {
        CallbackStatus result{};
        result.type = kind == "stick" ? InputType::Stick : InputType::Button;
        if (const auto it = sources.find(recent);
            it != sources.end() && it->second.status.type != InputType::None) {
            result = it->second.status;
        }
        float best = 0.0f;
        for (const auto& [key, source] : sources) {
            const float activity = Activity(source.status);
            if (activity > best) {
                best = activity;
                result = source.status;
            }
        }
        return result;
    }

    void OnChange(const std::string& key, const CallbackStatus& status) {
        std::scoped_lock lock{mutex};
        const auto it = sources.find(key);
        if (!alive || it == sources.end()) {
            return;
        }
        it->second.status = status;
        recent = key;
        if (notify) {
            notify(kind == "motion" ? status : Current());
        }
    }

    void Sync(const std::map<std::string, MappedDevice>& mapped) {
        std::map<std::string, Common::ParamPackage> wanted;
        const auto engine = base.Get("engine", "");
        if (engine == "keyboard" || engine == "analog_from_button" ||
            (engine != "sdl" && engine != "joycon" && engine != "gcpad" &&
             engine != "cemuhookudp" && engine != "mouse" && !engine.empty())) {
            wanted.emplace("base", base);
        }
        if (engine != "keyboard" && engine != "analog_from_button" && keyboard.Has("engine")) {
            wanted.emplace("keyboard", keyboard);
        }
        for (const auto& [key, device] : mapped) {
            Common::ParamPackage binding;
            bool found = false;
            if (Matches(base, device.identity)) {
                binding = base;
                found = true;
            } else if (kind == "button") {
                const auto it =
                    device.buttons.find(static_cast<Settings::NativeButton::Values>(index));
                if (it != device.buttons.end()) {
                    binding = it->second;
                    found = true;
                }
            } else if (kind == "stick") {
                const auto it =
                    device.sticks.find(static_cast<Settings::NativeAnalog::Values>(index));
                if (it != device.sticks.end()) {
                    binding = it->second;
                    found = true;
                }
            } else if (kind == "motion") {
                const auto it =
                    device.motions.find(static_cast<Settings::NativeMotion::Values>(index));
                if (it != device.motions.end()) {
                    binding = it->second;
                    found = true;
                }
            }
            if (found) {
                wanted.emplace(key, std::move(binding));
            }
        }

        std::vector<std::shared_ptr<InputDevice>> removed;
        {
            std::scoped_lock lock{mutex};
            if (!alive)
                return;
            for (auto it = sources.begin(); it != sources.end();) {
                if (!wanted.contains(it->first)) {
                    removed.push_back(std::move(it->second.input));
                    it = sources.erase(it);
                } else {
                    ++it;
                }
            }
            if (!removed.empty() && notify && kind != "motion")
                notify(Current());
        }
        removed.clear();
        for (const auto& [key, binding] : wanted) {
            {
                std::scoped_lock lock{mutex};
                if (!alive)
                    return;
                if (sources.contains(key))
                    continue;
            }
            std::shared_ptr<InputDevice> input = Common::Input::CreateInputDevice(binding);
            std::weak_ptr<State> weak = shared_from_this();
            input->SetCallback({.on_change = [weak, key](const CallbackStatus& status) {
                if (const auto state = weak.lock())
                    state->OnChange(key, status);
            }});
            {
                std::scoped_lock lock{mutex};
                if (!alive)
                    return;
                sources.emplace(key, Source{input, {}});
            }
            input->ForceUpdate();
        }
    }

    std::mutex mutex;
    bool alive{true};
    Common::ParamPackage base;
    Common::ParamPackage keyboard;
    std::string kind;
    int index;
    std::string recent;
    std::map<std::string, Source> sources;
    std::function<void(const CallbackStatus&)> notify;
};

class AnyDevice final : public InputDevice {
public:
    explicit AnyDevice(std::shared_ptr<State> state_) : state(std::move(state_)) {
        std::scoped_lock lock{state->mutex};
        state->notify = [this](const CallbackStatus& status) { TriggerOnChange(status); };
    }
    ~AnyDevice() override {
        std::map<std::string, State::Source> sources;
        {
            std::scoped_lock lock{state->mutex};
            state->alive = false;
            state->notify = {};
            sources.swap(state->sources);
        }
    }
    void ForceUpdate() override {
        std::scoped_lock lock{state->mutex};
        TriggerOnChange(state->Current());
    }

private:
    std::shared_ptr<State> state;
};
} // namespace

struct AnyInputFactory::Impl {
    DeviceList devices;
    Buttons buttons;
    Analogs analogs;
    Motions motions;
    std::mutex mutex;
    std::vector<std::weak_ptr<State>> states;
    bool dirty{};
    std::map<std::string, MappedDevice> mapped;
    std::chrono::steady_clock::time_point next_update{};
};

AnyInputFactory::AnyInputFactory(DeviceList devices, Buttons buttons, Analogs analogs,
                                 Motions motions)
    : impl(std::make_unique<Impl>()) {
    impl->devices = std::move(devices);
    impl->buttons = std::move(buttons);
    impl->analogs = std::move(analogs);
    impl->motions = std::move(motions);
}
AnyInputFactory::~AnyInputFactory() = default;

std::unique_ptr<InputDevice> AnyInputFactory::Create(const Common::ParamPackage& params) {
    auto state = std::make_shared<State>(params);
    {
        std::scoped_lock lock{impl->mutex};
        impl->states.emplace_back(state);
        impl->dirty = true;
    }
    return std::make_unique<AnyDevice>(std::move(state));
}

void AnyInputFactory::Pump() {
    const auto now = std::chrono::steady_clock::now();
    if (now < impl->next_update)
        return;
    impl->next_update = now + std::chrono::milliseconds{250};
    std::vector<std::shared_ptr<State>> states;
    bool changed{};
    {
        std::scoped_lock lock{impl->mutex};
        changed = std::exchange(impl->dirty, false);
        for (auto it = impl->states.begin(); it != impl->states.end();) {
            if (auto state = it->lock()) {
                states.push_back(std::move(state));
                ++it;
            } else {
                it = impl->states.erase(it);
            }
        }
    }
    if (states.empty()) {
        impl->mapped.clear();
        return;
    }
    std::map<std::string, Common::ParamPackage> available;
    for (const auto& device : impl->devices()) {
        const auto engine = device.Get("engine", "");
        if (engine != "any" && engine != "keyboard")
            available.emplace(Identity(device), device);
    }
    for (auto it = impl->mapped.begin(); it != impl->mapped.end();) {
        if (!available.contains(it->first)) {
            it = impl->mapped.erase(it);
            changed = true;
        } else
            ++it;
    }
    for (const auto& [key, device] : available) {
        if (!impl->mapped.contains(key)) {
            impl->mapped.emplace(key, MappedDevice{device, impl->buttons(device),
                                                   impl->analogs(device), impl->motions(device)});
            changed = true;
        }
    }
    if (!changed)
        return;
    for (auto& state : states)
        state->Sync(impl->mapped);
}
} // namespace InputCommon
