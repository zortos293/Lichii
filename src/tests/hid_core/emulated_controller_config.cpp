// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "common/param_package.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "hid_core/frontend/emulated_controller.h"

namespace {

using Core::HID::EmulatedController;
using Core::HID::NpadIdType;

constexpr auto LStick = Settings::NativeAnalog::LStick;
constexpr char SavedStick[] =
    "engine:sdl,port:0,guid:030000005e0400008e02000010010000,axis_x:0,axis_y:1,deadzone:0.150000";

float Deadzone(const EmulatedController& controller) {
    return controller.GetStickParam(LStick).Get("deadzone", -1.0f);
}

void SetDeadzone(EmulatedController& controller, float deadzone) {
    auto param = controller.GetStickParam(LStick);
    param.Set("deadzone", deadzone);
    controller.SetStickParam(LStick, param);
}

struct SavedPlayer {
    SavedPlayer() : saved{Settings::values.players.GetValue()[0]} {
        auto& player = Settings::values.players.GetValue()[0];
        player.connected = true;
        player.controller_type = Settings::ControllerType::ProController;
        player.analogs[LStick] = SavedStick;
    }
    ~SavedPlayer() {
        Settings::values.players.GetValue()[0] = saved;
    }
    Settings::PlayerInput saved;
};

struct VibrationOutput {
    std::array<Common::Input::VibrationStatus, 2> targets{};
    int writes{};
    bool fail{}, streamed{};
};

class TestVibrationDevice final : public Common::Input::OutputDevice {
public:
    explicit TestVibrationDevice(std::shared_ptr<VibrationOutput> output_) : output{output_} {}
    Common::Input::DriverResult SetVibration(const Common::Input::VibrationStatus& value) override {
        ++output->writes;
        if (output->fail) {
            return Common::Input::DriverResult::ErrorWritingData;
        }
        if (value.side != Common::Input::VibrationSide::Right) {
            output->targets[0] = value;
        }
        if (value.side != Common::Input::VibrationSide::Left) {
            output->targets[1] = value;
        }
        return Common::Input::DriverResult::Success;
    }
    bool IsVibrationStreamed() override {
        return output->streamed;
    }

private:
    std::shared_ptr<VibrationOutput> output;
};

class TestVibrationFactory final : public Common::Input::Factory<Common::Input::OutputDevice> {
public:
    std::shared_ptr<VibrationOutput> output = std::make_shared<VibrationOutput>();
    std::unique_ptr<Common::Input::OutputDevice> Create(const Common::ParamPackage&) override {
        return std::make_unique<TestVibrationDevice>(output);
    }
};

} // namespace

TEST_CASE("Cancelled controller edits revert to the saved deadzone", "[hid][emulated_controller]") {
    const SavedPlayer player;
    EmulatedController controller(NpadIdType::Player1);
    controller.ReloadFromSettings();

    controller.SaveCurrentConfig();
    controller.EnableConfiguration();
    SetDeadzone(controller, 0.0f);
    REQUIRE(Deadzone(controller) == 0.0f);

    controller.RestoreConfig();
    controller.DisableConfiguration();

    REQUIRE(Deadzone(controller) == 0.15f);
    const auto& saved = Settings::values.players.GetValue()[0].analogs[LStick];
    REQUIRE(Common::ParamPackage{saved}.Get("deadzone", -1.0f) == 0.15f);
}

TEST_CASE("An applied 0% deadzone is saved and reloaded", "[hid][emulated_controller]") {
    const SavedPlayer player;
    EmulatedController controller(NpadIdType::Player1);
    controller.ReloadFromSettings();

    controller.SaveCurrentConfig();
    controller.EnableConfiguration();
    SetDeadzone(controller, 0.0f);
    controller.DisableConfiguration();
    controller.SaveCurrentConfig();

    const auto& saved = Settings::values.players.GetValue()[0].analogs[LStick];
    REQUIRE(Common::ParamPackage{saved}.Get("deadzone", 0.15f) == 0.0f);

    EmulatedController restarted(NpadIdType::Player1);
    restarted.ReloadFromSettings();
    REQUIRE(Deadzone(restarted) == 0.0f);
}

TEST_CASE("Controller vibration caches successful writes and stops used outputs",
          "[hid][emulated_controller][rumble]") {
    const SavedPlayer player;
    auto& settings = Settings::values;
    const bool enabled = settings.vibration_enabled.GetValue();
    const bool accurate = settings.enable_accurate_vibrations.GetValue();
    SCOPE_EXIT {
        settings.vibration_enabled.SetValue(enabled);
        settings.enable_accurate_vibrations.SetValue(accurate);
    };
    settings.vibration_enabled.SetValue(true);
    settings.enable_accurate_vibrations.SetValue(true);
    settings.players.GetValue()[0].vibration_enabled = true;
    settings.players.GetValue()[0].vibration_strength = 100;
    auto factory = std::make_shared<TestVibrationFactory>();
    Common::Input::RegisterOutputFactory("vibration-test", factory);
    SCOPE_EXIT {
        Common::Input::UnregisterOutputFactory("vibration-test");
    };
    EmulatedController controller{NpadIdType::Player1};
    controller.ReloadFromSettings();
    controller.SetButtonParam(Settings::NativeButton::DRight,
                              Common::ParamPackage{"engine:vibration-test"});
    auto& output = *factory->output;
    constexpr auto left = Core::HID::DeviceIndex::Left;
    const auto stop = Core::HID::DEFAULT_VIBRATION_VALUE;
    const Core::HID::VibrationValue tone{0.5f, 220, 0.5f, 660};

    SECTION("failed writes can be retried without changing the request") {
        output.fail = true;
        REQUIRE_FALSE(controller.SetVibration(left, tone));
        REQUIRE((controller.GetActualVibrationValue(left) == stop));
        output.fail = false;
        REQUIRE(controller.SetVibration(left, tone));
        REQUIRE(output.writes == 2);
        REQUIRE(controller.SetVibration(left, tone));
        REQUIRE(output.writes == 2);
    }
    SECTION("reloading stops ordinary output and permits a subsequent stop") {
        REQUIRE(controller.SetVibration(left, tone));
        controller.ReloadInput();
        REQUIRE(output.targets[0].low_amplitude == 0);
        REQUIRE(controller.SetVibration(left, stop));
        REQUIRE(output.writes == 3);
    }
    SECTION("a failed lifecycle stop does not suppress a subsequent stop") {
        REQUIRE(controller.SetVibration(left, tone));
        output.fail = true;
        controller.ReloadInput();
        output.fail = false;
        REQUIRE(controller.SetVibration(left, stop));
        REQUIRE(output.targets[0].low_amplitude == 0);
    }
    SECTION("disconnect stops ordinary output") {
        REQUIRE(controller.SetVibration(left, tone));
        controller.Disconnect();
        REQUIRE(output.targets[0].low_amplitude == 0);
    }
    SECTION("unloading stops the remaining grip after a Both request") {
        output.streamed = GENERATE(false, true);
        REQUIRE(controller.SetVibration(true));
        REQUIRE(controller.SetVibration(left, stop));
        REQUIRE(output.targets[1].low_amplitude > 0);
        controller.UnloadInput();
        REQUIRE(output.targets[1].low_amplitude == 0);
    }
}
