// SPDX-License-Identifier: GPL-2.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/resources/npad/npad_types.h"
#include "hid_core/resources/npad/npad_vibration.h"
#include "hid_core/resources/vibration/n64_vibration_device.h"
#include "tests/core/system_test_context.h"

TEST_CASE("N64 vibration device forwards the requested state", "[hid][vibration]") {
    using namespace Service::HID;
    using Core::HID::DeviceIndex;

    ScopedSystemTestContext environment;
    Core::System system;
    REQUIRE(system.Initialize());
    auto& kernel = system.Kernel();
    Kernel::KThread bootstrap{kernel};
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, &bootstrap, nullptr).IsSuccess());
    kernel.SetCurrentEmuThread(&bootstrap);
    kernel.Initialize();
    SCOPE_EXIT {
        system.CoreTiming().ClearPendingEvents();
        kernel.Shutdown();
        kernel.SetCurrentEmuThread(nullptr);
        kernel.UnregisterKernelObject(&bootstrap);
    };

    auto* controller = system.HIDCore().GetEmulatedControllerByIndex(0);
    REQUIRE(controller != nullptr);
    controller->ReloadInput();

    IAbstractedPad abstracted_pad{};
    abstracted_pad.internal_flags.is_connected.Assign(1);
    abstracted_pad.xcd_handle = controller;

    NpadVibration vibration_handler;
    NpadN64VibrationDevice device;
    REQUIRE(device.Mount(abstracted_pad, &vibration_handler).IsSuccess());
    REQUIRE(device.Activate().IsSuccess());

    CHECK(controller->GetActualVibrationValue(DeviceIndex::Left).high_amplitude == 0.0f);

    // A guest vibration request must reach the controller.
    REQUIRE(device.SendValueInBool(true).IsSuccess());
    CHECK(controller->GetActualVibrationValue(DeviceIndex::Left).high_amplitude == 1.0f);
    CHECK(controller->GetActualVibrationValue(DeviceIndex::Left).low_amplitude == 1.0f);

    // Stopping must stop it again.
    REQUIRE(device.SendValueInBool(false).IsSuccess());
    CHECK(controller->GetActualVibrationValue(DeviceIndex::Left).high_amplitude == 0.0f);
    CHECK(controller->GetActualVibrationValue(DeviceIndex::Left).low_amplitude == 0.0f);
}
