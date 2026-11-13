// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include <QGuiApplication>
#include <QStyleHints>

#include "common/settings_input.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "lichii/util/controller_navigation.h"

ControllerNavigation::ControllerNavigation(Core::HID::HIDCore& hid_core, QWidget* parent) {
    player1_controller = hid_core.GetEmulatedController(Core::HID::NpadIdType::Player1);
    handheld_controller = hid_core.GetEmulatedController(Core::HID::NpadIdType::Handheld);
    Core::HID::ControllerUpdateCallback engine_callback{
        .on_change = [this](Core::HID::ControllerTriggerType type) { ControllerUpdateEvent(type); },
        .is_npad_service = false,
    };
    repeat_timer.setTimerType(Qt::PreciseTimer);
    connect(&repeat_timer, &QTimer::timeout, this, [this] {
        if (!Settings::values.controller_navigation || repeat_key == Qt::Key_unknown) {
            repeat_timer.stop();
            return;
        }
        // After the first delay, the keyboard's repeat rate (kept between 10 and 30 a second).
        const qreal rate =
            std::clamp(QGuiApplication::styleHints()->keyboardAutoRepeatRateF(), 10.0, 30.0);
        repeat_timer.setInterval(static_cast<int>(1000.0 / rate));
        emit RepeatKeyboardEvent(repeat_key);
    });
    player1_callback_key = player1_controller->SetCallback(engine_callback);
    handheld_callback_key = handheld_controller->SetCallback(engine_callback);
    is_controller_set = true;
}

ControllerNavigation::~ControllerNavigation() {
    UnloadController();
}

void ControllerNavigation::UnloadController() {
    if (is_controller_set) {
        player1_controller->DeleteCallback(player1_callback_key);
        handheld_controller->DeleteCallback(handheld_callback_key);
        is_controller_set = false;
    }
}

void ControllerNavigation::TriggerButton(Settings::NativeButton::Values native_button,
                                         Qt::Key key) {
    if (button_values[native_button].value && !button_values[native_button].locked) {
        emit TriggerKeyboardEvent(key);
    }
}

void ControllerNavigation::ControllerUpdateEvent(Core::HID::ControllerTriggerType type) {
    std::scoped_lock lock{mutex};
    if (!Settings::values.controller_navigation) {
        return;
    }
    if (type == Core::HID::ControllerTriggerType::Button) {
        ControllerUpdateButton();
        UpdateHeldDirection();
        return;
    }

    if (type == Core::HID::ControllerTriggerType::Stick) {
        ControllerUpdateStick();
        UpdateHeldDirection();
        return;
    }
}

Qt::Key ControllerNavigation::HeldDirection() const {
    switch (player1_controller->GetNpadStyleIndex()) {
    case Core::HID::NpadStyleIndex::Fullkey:
    case Core::HID::NpadStyleIndex::JoyconDual:
    case Core::HID::NpadStyleIndex::Handheld:
    case Core::HID::NpadStyleIndex::GameCube:
        break;
    default:
        return Qt::Key_unknown;
    }
    using Button = Settings::NativeButton::Values;
    for (const auto& [button, key] : {std::pair{Button::DRight, Qt::Key_Right},
                                      std::pair{Button::DLeft, Qt::Key_Left},
                                      std::pair{Button::DDown, Qt::Key_Down},
                                      std::pair{Button::DUp, Qt::Key_Up}}) {
        if (button_values[button].value) {
            return key;
        }
    }
    const auto& stick = stick_values[Settings::NativeAnalog::LStick];
    if (stick.down) {
        return Qt::Key_Down;
    }
    if (stick.left) {
        return Qt::Key_Left;
    }
    if (stick.right) {
        return Qt::Key_Right;
    }
    if (stick.up) {
        return Qt::Key_Up;
    }
    return Qt::Key_unknown;
}

void ControllerNavigation::UpdateHeldDirection() {
    if (!direction_repeat) {
        return;
    }
    const Qt::Key key = HeldDirection();
    if (key == held_direction) {
        return;
    }
    held_direction = key;
    QMetaObject::invokeMethod(
        this, [this, key] { SetHeldDirection(key); }, Qt::QueuedConnection);
}

void ControllerNavigation::SetHeldDirection(Qt::Key key) {
    repeat_key = key;
    if (key == Qt::Key_unknown) {
        repeat_timer.stop();
        return;
    }
    // The first press was already sent: repeat only once the direction is held a moment.
    repeat_timer.start(400);
}

void ControllerNavigation::ControllerUpdateButton() {
    const auto controller_type = player1_controller->GetNpadStyleIndex();
    const auto& player1_buttons = player1_controller->GetButtonsValues();
    const auto& handheld_buttons = handheld_controller->GetButtonsValues();

    for (std::size_t i = 0; i < player1_buttons.size(); ++i) {
        const bool button = player1_buttons[i].value || handheld_buttons[i].value;
        // Trigger only once
        button_values[i].locked = button == button_values[i].value;
        button_values[i].value = button;
    }

    switch (controller_type) {
    case Core::HID::NpadStyleIndex::Fullkey:
    case Core::HID::NpadStyleIndex::JoyconDual:
    case Core::HID::NpadStyleIndex::Handheld:
    case Core::HID::NpadStyleIndex::GameCube:
        TriggerButton(Settings::NativeButton::A, Qt::Key_Enter);
        TriggerButton(Settings::NativeButton::B, Qt::Key_Escape);
        TriggerButton(Settings::NativeButton::DDown, Qt::Key_Down);
        TriggerButton(Settings::NativeButton::DLeft, Qt::Key_Left);
        TriggerButton(Settings::NativeButton::DRight, Qt::Key_Right);
        TriggerButton(Settings::NativeButton::DUp, Qt::Key_Up);
        if (library_buttons) {
            TriggerButton(Settings::NativeButton::X, Qt::Key_X);
            TriggerButton(Settings::NativeButton::Y, Qt::Key_Y);
            TriggerButton(Settings::NativeButton::L, Qt::Key_BracketLeft);
            TriggerButton(Settings::NativeButton::R, Qt::Key_BracketRight);
        }
        break;
    case Core::HID::NpadStyleIndex::JoyconLeft:
        TriggerButton(Settings::NativeButton::DDown, Qt::Key_Enter);
        TriggerButton(Settings::NativeButton::DLeft, Qt::Key_Escape);
        break;
    case Core::HID::NpadStyleIndex::JoyconRight:
        TriggerButton(Settings::NativeButton::X, Qt::Key_Enter);
        TriggerButton(Settings::NativeButton::A, Qt::Key_Escape);
        break;
    default:
        break;
    }
}

void ControllerNavigation::ControllerUpdateStick() {
    const auto controller_type = player1_controller->GetNpadStyleIndex();
    const auto& player1_sticks = player1_controller->GetSticksValues();
    const auto& handheld_sticks = player1_controller->GetSticksValues();
    bool update = false;

    for (std::size_t i = 0; i < player1_sticks.size(); ++i) {
        const Common::Input::StickStatus stick{
            .left = player1_sticks[i].left || handheld_sticks[i].left,
            .right = player1_sticks[i].right || handheld_sticks[i].right,
            .up = player1_sticks[i].up || handheld_sticks[i].up,
            .down = player1_sticks[i].down || handheld_sticks[i].down,
        };
        // Trigger only once
        if (stick.down != stick_values[i].down || stick.left != stick_values[i].left ||
            stick.right != stick_values[i].right || stick.up != stick_values[i].up) {
            update = true;
        }
        stick_values[i] = stick;
    }

    if (!update) {
        return;
    }

    switch (controller_type) {
    case Core::HID::NpadStyleIndex::Fullkey:
    case Core::HID::NpadStyleIndex::JoyconDual:
    case Core::HID::NpadStyleIndex::Handheld:
    case Core::HID::NpadStyleIndex::GameCube:
        if (stick_values[Settings::NativeAnalog::LStick].down) {
            emit TriggerKeyboardEvent(Qt::Key_Down);
            return;
        }
        if (stick_values[Settings::NativeAnalog::LStick].left) {
            emit TriggerKeyboardEvent(Qt::Key_Left);
            return;
        }
        if (stick_values[Settings::NativeAnalog::LStick].right) {
            emit TriggerKeyboardEvent(Qt::Key_Right);
            return;
        }
        if (stick_values[Settings::NativeAnalog::LStick].up) {
            emit TriggerKeyboardEvent(Qt::Key_Up);
            return;
        }
        break;
    case Core::HID::NpadStyleIndex::JoyconLeft:
        if (stick_values[Settings::NativeAnalog::LStick].left) {
            emit TriggerKeyboardEvent(Qt::Key_Down);
            return;
        }
        if (stick_values[Settings::NativeAnalog::LStick].up) {
            emit TriggerKeyboardEvent(Qt::Key_Left);
            return;
        }
        if (stick_values[Settings::NativeAnalog::LStick].down) {
            emit TriggerKeyboardEvent(Qt::Key_Right);
            return;
        }
        if (stick_values[Settings::NativeAnalog::LStick].right) {
            emit TriggerKeyboardEvent(Qt::Key_Up);
            return;
        }
        break;
    case Core::HID::NpadStyleIndex::JoyconRight:
        if (stick_values[Settings::NativeAnalog::RStick].right) {
            emit TriggerKeyboardEvent(Qt::Key_Down);
            return;
        }
        if (stick_values[Settings::NativeAnalog::RStick].down) {
            emit TriggerKeyboardEvent(Qt::Key_Left);
            return;
        }
        if (stick_values[Settings::NativeAnalog::RStick].up) {
            emit TriggerKeyboardEvent(Qt::Key_Right);
            return;
        }
        if (stick_values[Settings::NativeAnalog::RStick].left) {
            emit TriggerKeyboardEvent(Qt::Key_Up);
            return;
        }
        break;
    default:
        break;
    }
}
