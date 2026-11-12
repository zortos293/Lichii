// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(__linux__) && !defined(__ANDROID__)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include "common/input.h"
#include "common/settings_input.h"
#include "input_common/drivers/keyboard.h"
#include "input_common/main.h"

TEST_CASE("Any input receives SDL virtual controller hotplug", "[input][any][sdl]") {
    InputCommon::InputSubsystem subsystem;
    subsystem.Initialize();
    struct Cleanup {
        InputCommon::InputSubsystem& subsystem;
        SDL_JoystickID id{};
        SDL_Joystick* joystick{};
        ~Cleanup() {
            if (joystick)
                SDL_CloseJoystick(joystick);
            if (id)
                SDL_DetachVirtualJoystick(id);
            subsystem.Shutdown();
        }
    } cleanup{subsystem};

    Common::ParamPackage params;
    params.Set("engine", "any_input");
    params.Set("kind", "button");
    params.Set("index", static_cast<int>(Settings::NativeButton::A));
    params.Set("base", InputCommon::GenerateKeyboardParam(123));
    auto input = Common::Input::CreateInputDevice(params);
    bool pressed{};
    input->SetCallback({.on_change = [&](const Common::Input::CallbackStatus& status) {
        pressed = status.button_status.value;
    }});
    subsystem.PumpEvents();
    subsystem.GetKeyboard()->PressKey(123);
    REQUIRE(pressed);
    subsystem.GetKeyboard()->ReleaseKey(123);
    REQUIRE_FALSE(pressed);

    SDL_VirtualJoystickDesc description{};
    SDL_INIT_INTERFACE(&description);
    description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    description.naxes = 6;
    description.nbuttons = 20;
    description.button_mask = 0xfffff;
    description.axis_mask = 0x3f;
    description.name = "Lichii test gamepad";
    cleanup.id = SDL_AttachVirtualJoystick(&description);
    REQUIRE(cleanup.id != 0);
    cleanup.joystick = SDL_OpenJoystick(cleanup.id);
    REQUIRE(cleanup.joystick != nullptr);
    subsystem.PumpEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds{260});
    subsystem.PumpEvents();
    const auto devices = subsystem.GetInputDevices();
    const auto device = std::find_if(devices.begin(), devices.end(), [](const auto& p) {
        return p.Get("engine", "") == "sdl" &&
               p.Get("display", "").find("Lichii test gamepad") != std::string::npos;
    });
    REQUIRE(device != devices.end());
    const auto mapping = subsystem.GetButtonMappingForDevice(*device);
    REQUIRE(mapping.contains(Settings::NativeButton::A));
    const int button = mapping.at(Settings::NativeButton::A).Get("button", -1);
    REQUIRE(button >= 0);
    const auto sticks = subsystem.GetAnalogMappingForDevice(*device);
    REQUIRE(sticks.contains(Settings::NativeAnalog::LStick));
    const int axis = sticks.at(Settings::NativeAnalog::LStick).Get("axis_x", -1);
    REQUIRE(axis >= 0);
    Common::ParamPackage stick_params;
    stick_params.Set("engine", "any_input");
    stick_params.Set("kind", "stick");
    stick_params.Set("index", static_cast<int>(Settings::NativeAnalog::LStick));
    stick_params.Set("base", InputCommon::GenerateAnalogParamFromKeys(101, 102, 103, 104, 0, 0.5f));
    auto stick_input = Common::Input::CreateInputDevice(stick_params);
    float stick_x{};
    stick_input->SetCallback({.on_change = [&](const Common::Input::CallbackStatus& status) {
        stick_x = status.stick_status.x.raw_value;
    }});
    std::this_thread::sleep_for(std::chrono::milliseconds{260});
    subsystem.PumpEvents();
    REQUIRE(SDL_SetJoystickVirtualAxis(cleanup.joystick, axis, 20000));
    subsystem.PumpEvents();
    REQUIRE(std::abs(stick_x) > 0.4f);
    subsystem.GetKeyboard()->PressKey(104);
    REQUIRE(SDL_SetJoystickVirtualAxis(cleanup.joystick, axis, 0));
    subsystem.PumpEvents();
    REQUIRE(stick_x > 0.4f);
    subsystem.GetKeyboard()->ReleaseKey(104);
    subsystem.PumpEvents();
    REQUIRE(std::abs(stick_x) < 0.01f);
    const int custom_button = (button + 1) % 20;
    auto custom_mapping = mapping.at(Settings::NativeButton::A);
    custom_mapping.Set("button", custom_button);
    Common::ParamPackage custom_params = params;
    custom_params.Set("base", custom_mapping.Serialize());
    custom_params.Set("keyboard", InputCommon::GenerateKeyboardParam(123));
    auto custom_input = Common::Input::CreateInputDevice(custom_params);
    bool custom_pressed{};
    custom_input->SetCallback({.on_change = [&](const Common::Input::CallbackStatus& status) {
        custom_pressed = status.button_status.value;
    }});
    std::this_thread::sleep_for(std::chrono::milliseconds{260});
    subsystem.PumpEvents();
    REQUIRE(SDL_SetJoystickVirtualButton(cleanup.joystick, custom_button, true));
    subsystem.PumpEvents();
    REQUIRE(custom_pressed);
    REQUIRE_FALSE(pressed);
    REQUIRE(SDL_SetJoystickVirtualButton(cleanup.joystick, custom_button, false));
    subsystem.PumpEvents();
    REQUIRE_FALSE(custom_pressed);
    REQUIRE(SDL_SetJoystickVirtualButton(cleanup.joystick, button, true));
    subsystem.PumpEvents();
    REQUIRE(pressed);
    REQUIRE_FALSE(custom_pressed);
    subsystem.GetKeyboard()->PressKey(123);
    REQUIRE(custom_pressed);
    REQUIRE(SDL_SetJoystickVirtualButton(cleanup.joystick, button, false));
    subsystem.PumpEvents();
    REQUIRE(pressed);
    subsystem.GetKeyboard()->ReleaseKey(123);
    REQUIRE_FALSE(pressed);
    REQUIRE_FALSE(custom_pressed);
    REQUIRE(SDL_SetJoystickVirtualButton(cleanup.joystick, button, true));
    subsystem.PumpEvents();
    REQUIRE(pressed);

    SDL_CloseJoystick(cleanup.joystick);
    cleanup.joystick = nullptr;
    REQUIRE(SDL_DetachVirtualJoystick(cleanup.id));
    cleanup.id = 0;
    subsystem.PumpEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds{260});
    subsystem.PumpEvents();
    REQUIRE_FALSE(pressed);
    REQUIRE_FALSE(custom_pressed);

    description.product_id = 42;
    description.name = "Lichii replacement gamepad";
    cleanup.id = SDL_AttachVirtualJoystick(&description);
    REQUIRE(cleanup.id != 0);
    cleanup.joystick = SDL_OpenJoystick(cleanup.id);
    REQUIRE(cleanup.joystick != nullptr);
    subsystem.PumpEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds{260});
    subsystem.PumpEvents();
    const auto replacement_devices = subsystem.GetInputDevices();
    const auto replacement =
        std::find_if(replacement_devices.begin(), replacement_devices.end(), [](const auto& p) {
            return p.Get("engine", "") == "sdl" &&
                   p.Get("display", "").find("Lichii replacement gamepad") != std::string::npos;
        });
    REQUIRE(replacement != replacement_devices.end());
    const auto replacement_mapping = subsystem.GetButtonMappingForDevice(*replacement);
    REQUIRE(replacement_mapping.contains(Settings::NativeButton::A));
    const int replacement_button =
        replacement_mapping.at(Settings::NativeButton::A).Get("button", -1);
    REQUIRE(replacement_button >= 0);
    REQUIRE(SDL_SetJoystickVirtualButton(cleanup.joystick, replacement_button, true));
    subsystem.PumpEvents();
    REQUIRE(pressed);
    REQUIRE(custom_pressed);
    stick_input.reset();
    custom_input.reset();
    input.reset();
}
#endif
