// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/settings_input.h"
#include "input_common/any_input.h"

namespace {
using namespace Common::Input;

struct TestFactory;
struct TestInput final : InputDevice {
    TestInput(TestFactory& owner_, std::string name_);
    ~TestInput() override;
    void ForceUpdate() override;
    TestFactory& owner;
    std::string name;
};

struct TestFactory final : Factory<InputDevice> {
    std::unique_ptr<InputDevice> Create(const Common::ParamPackage& params) override {
        return std::make_unique<TestInput>(*this, params.Get("key", ""));
    }
    void Set(const std::string& name, bool pressed) {
        values[name] = pressed;
        const auto snapshot = inputs;
        for (auto* input : snapshot)
            if (input->name == name)
                input->ForceUpdate();
    }
    void Send(const std::string& name, const CallbackStatus& status) {
        const auto snapshot = inputs;
        for (auto* input : snapshot)
            if (input->name == name)
                input->TriggerOnChange(status);
    }
    std::map<std::string, bool> values;
    std::vector<TestInput*> inputs;
};

TestInput::TestInput(TestFactory& owner_, std::string name_)
    : owner(owner_), name(std::move(name_)) {
    owner.inputs.push_back(this);
}
TestInput::~TestInput() {
    std::erase(owner.inputs, this);
}
void TestInput::ForceUpdate() {
    CallbackStatus status{};
    status.type = InputType::Button;
    status.button_status.value = owner.values[name];
    TriggerOnChange(status);
}

Common::ParamPackage Binding(const std::string& engine, const std::string& key) {
    Common::ParamPackage p;
    p.Set("engine", engine);
    p.Set("key", key);
    return p;
}
} // namespace

TEST_CASE("Any input combines keyboard and hotplugged controllers", "[input][any]") {
    auto test_factory = std::make_shared<TestFactory>();
    RegisterInputFactory("keyboard", test_factory);
    RegisterInputFactory("test_any", test_factory);
    std::vector<Common::ParamPackage> devices;
    InputCommon::AnyInputFactory factory(
        [&] { return devices; },
        [](const Common::ParamPackage& device) {
            InputCommon::ButtonMapping result;
            result.emplace(Settings::NativeButton::A, Binding("test_any", device.Get("guid", "")));
            return result;
        },
        [](const auto&) { return InputCommon::AnalogMapping{}; },
        [](const auto&) { return InputCommon::MotionMapping{}; });
    Common::ParamPackage params;
    params.Set("base", Binding("keyboard", "key_a").Serialize());
    params.Set("kind", "button");
    params.Set("index", static_cast<int>(Settings::NativeButton::A));
    bool pressed = false;
    CallbackStatus current{};
    {
        auto input = factory.Create(params);
        input->SetCallback({.on_change = [&](const CallbackStatus& value) {
            current = value;
            pressed = value.button_status.value;
        }});
        factory.Pump();
        test_factory->Set("key_a", true);
        REQUIRE(pressed);
        test_factory->Set("key_a", false);
        REQUIRE_FALSE(pressed);

        Common::ParamPackage first{{"engine", "test_any"}, {"guid", "first"}};
        devices.push_back(first);
        std::this_thread::sleep_for(std::chrono::milliseconds{260});
        factory.Pump();
        test_factory->Set("first", true);
        REQUIRE(pressed);
        test_factory->Set("key_a", true);
        test_factory->Set("first", false);
        REQUIRE(pressed);
        test_factory->Set("key_a", false);
        REQUIRE_FALSE(pressed);

        test_factory->Set("key_a", true);
        CallbackStatus axis{};
        axis.type = InputType::Analog;
        axis.analog_status.raw_value = -1.0f;
        test_factory->Send("first", axis);
        REQUIRE(current.type == InputType::Button);
        REQUIRE(current.button_status.value);
        axis.analog_status.raw_value = 0.8f;
        test_factory->Send("first", axis);
        test_factory->Set("key_a", false);
        REQUIRE(current.type == InputType::Analog);
        REQUIRE(current.analog_status.raw_value == 0.8f);
        test_factory->Set("first", false);
        REQUIRE_FALSE(pressed);

        test_factory->Set("first", true);
        devices.clear();
        devices.emplace_back(Common::ParamPackage{{"engine", "test_any"}, {"guid", "second"}});
        std::this_thread::sleep_for(std::chrono::milliseconds{260});
        factory.Pump();
        REQUIRE_FALSE(pressed);
        test_factory->Set("second", true);
        REQUIRE(pressed);
    }
    UnregisterInputFactory("test_any");
    UnregisterInputFactory("keyboard");
}
