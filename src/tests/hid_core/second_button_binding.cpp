// SPDX-License-Identifier: GPL-3.0-or-later

#include <map>
#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "common/input.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "hid_core/frontend/emulated_controller.h"

namespace {

using Core::HID::EmulatedController;
using Core::HID::NpadIdType;

constexpr auto ButtonA = Settings::NativeButton::A;

class TestButton final : public Common::Input::InputDevice {
public:
    TestButton(bool inverted_, bool toggle_) : inverted{inverted_}, toggle{toggle_} {}

    void Press(bool pressed) {
        TriggerOnChange({
            .type = Common::Input::InputType::Button,
            .button_status = {.value = pressed, .inverted = inverted, .toggle = toggle},
        });
    }

    void Tap() {
        Press(true);
        Press(false);
    }

private:
    bool inverted;
    bool toggle;
};

class TestButtonFactory final : public Common::Input::Factory<Common::Input::InputDevice> {
public:
    std::unique_ptr<Common::Input::InputDevice> Create(
        const Common::ParamPackage& params) override {
        auto device = std::make_unique<TestButton>(params.Get("inverted", false) != 0,
                                                   params.Get("toggle", false) != 0);
        // Battery, colour and other devices are also made from button A's params; the button
        // device is made first.
        devices.emplace(params.Get("id", 0), device.get());
        return device;
    }

    std::map<int, TestButton*> devices;
};

struct TestSetup {
    TestSetup() : saved{Settings::values.players.GetValue()[0]} {
        Common::Input::RegisterInputFactory("test_button", factory);
        auto& player = Settings::values.players.GetValue()[0];
        player.connected = true;
        player.controller_type = Settings::ControllerType::ProController;
    }
    ~TestSetup() {
        Common::Input::UnregisterInputFactory("test_button");
        Settings::values.players.GetValue()[0] = saved;
    }

    void BindA(const std::string& alt, bool toggle = false) {
        Common::ParamPackage param{{"engine", "test_button"}, {"id", "1"}};
        if (toggle) {
            param.Set("toggle", true);
        }
        if (!alt.empty()) {
            param.Set("alt", alt);
        }
        Settings::values.players.GetValue()[0].buttons[ButtonA] = param.Serialize();
    }

    std::shared_ptr<TestButtonFactory> factory = std::make_shared<TestButtonFactory>();
    Settings::PlayerInput saved;
};

std::string SecondBinding(bool inverted = false) {
    Common::ParamPackage param{{"engine", "test_button"}, {"id", "2"}};
    if (inverted) {
        param.Set("inverted", true);
    }
    return param.Serialize();
}

bool PressedA(const EmulatedController& controller) {
    return controller.GetButtonsValues()[ButtonA].value;
}

} // namespace

TEST_CASE("Either binding presses a button with a second binding", "[hid][emulated_controller]") {
    TestSetup setup;
    setup.BindA(SecondBinding());
    EmulatedController controller(NpadIdType::Player1);
    controller.ReloadFromSettings();

    auto& first = *setup.factory->devices.at(1);
    auto& second = *setup.factory->devices.at(2);
    REQUIRE_FALSE(PressedA(controller));

    second.Press(true);
    REQUIRE(PressedA(controller));
    second.Press(false);
    REQUIRE_FALSE(PressedA(controller));

    first.Press(true);
    REQUIRE(PressedA(controller));

    // Releasing one binding keeps the button pressed while the other is held.
    second.Press(true);
    first.Press(false);
    REQUIRE(PressedA(controller));
    second.Press(false);
    REQUIRE_FALSE(PressedA(controller));
}

TEST_CASE("A second binding keeps its own inversion", "[hid][emulated_controller]") {
    TestSetup setup;
    setup.BindA(SecondBinding(true));
    EmulatedController controller(NpadIdType::Player1);
    controller.ReloadFromSettings();

    auto& second = *setup.factory->devices.at(2);
    second.Press(false);
    REQUIRE(PressedA(controller));
    second.Press(true);
    REQUIRE_FALSE(PressedA(controller));
}

TEST_CASE("A button without a second binding has one device", "[hid][emulated_controller]") {
    TestSetup setup;
    setup.BindA({});
    EmulatedController controller(NpadIdType::Player1);
    controller.ReloadFromSettings();

    REQUIRE(setup.factory->devices.size() == 1);
    setup.factory->devices.at(1)->Press(true);
    REQUIRE(PressedA(controller));
}

TEST_CASE("A toggle binding stays toggled while the other binding is tapped",
          "[hid][emulated_controller]") {
    TestSetup setup;
    setup.BindA(SecondBinding(), true);
    EmulatedController controller(NpadIdType::Player1);
    controller.ReloadFromSettings();

    auto& first = *setup.factory->devices.at(1);
    auto& second = *setup.factory->devices.at(2);
    // Real devices report their state when the controller loads.
    first.Press(false);

    second.Press(true);
    REQUIRE(PressedA(controller));
    second.Press(false);
    REQUIRE_FALSE(PressedA(controller));

    first.Tap();
    REQUIRE(PressedA(controller));
    second.Tap();
    REQUIRE(PressedA(controller));
    first.Tap();
    REQUIRE_FALSE(PressedA(controller));
}

TEST_CASE("Mapped devices include a second binding's device", "[hid][emulated_controller]") {
    TestSetup setup;
    setup.BindA(Common::ParamPackage{{"engine", "other_button"}, {"id", "2"}}.Serialize());
    EmulatedController controller(NpadIdType::Player1);
    controller.ReloadFromSettings();

    bool found = false;
    for (const auto& device : controller.GetMappedDevices()) {
        found = found || device.Get("engine", "") == "other_button";
    }
    REQUIRE(found);
}
