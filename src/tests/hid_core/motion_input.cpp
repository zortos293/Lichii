// SPDX-License-Identifier: GPL-3.0-or-later

#include <cmath>
#include <catch2/catch_test_macros.hpp>

#include "hid_core/frontend/motion_input.h"

using Core::HID::MotionInput;

namespace {

constexpr u64 TenMs = 10000; // UpdateRotation takes microseconds

bool Near(f32 a, f32 b) {
    return std::abs(a - b) < 1e-6f;
}

} // namespace

TEST_CASE("Gyro samples add to rotations scaled by the rotation scale", "[hid][motion_input]") {
    MotionInput motion;
    motion.SetGyroscope({1.0f, 0.0f, 0.0f});
    motion.UpdateRotation(TenMs);
    REQUIRE(Near(motion.GetRotations()[0], 0.01f));

    // A new scale applies to later samples only: the total doesn't move without motion.
    motion.SetRotationScale(2.0f);
    REQUIRE(Near(motion.GetRotations()[0], 0.01f));

    motion.UpdateRotation(TenMs);
    REQUIRE(Near(motion.GetRotations()[0], 0.03f));
}

TEST_CASE("A rotation reset after a scale change starts the total from zero",
          "[hid][motion_input]") {
    MotionInput motion;
    motion.SetGyroscope({1.0f, 0.0f, 0.0f});
    for (int i = 0; i < 10; ++i) {
        motion.UpdateRotation(TenMs);
    }
    REQUIRE(Near(motion.GetRotations()[0], 0.1f));

    motion.SetRotationScale(2.0f);
    motion.ResetRotations();
    motion.UpdateRotation(TenMs);
    REQUIRE(Near(motion.GetRotations()[0], 0.02f));
}
