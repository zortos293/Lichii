// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace {

enum class EnumerationCase { GrowOnce, Shrink, Empty, AlwaysIncomplete, FatalCount };

struct EnumerationState {
    EnumerationCase scenario{};
    u32 count_calls{};
    u32 fill_calls{};
};

EnumerationState* active_state{};

VkPhysicalDevice FakePhysicalDevice(std::uintptr_t n) {
    return reinterpret_cast<VkPhysicalDevice>(n);
}

VkResult VKAPI_CALL FakeEnumerate(VkInstance, u32* count, VkPhysicalDevice* devices) {
    auto& state = *active_state;
    if (!devices) {
        ++state.count_calls;
        if (state.scenario == EnumerationCase::FatalCount) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (state.scenario == EnumerationCase::Empty) {
            *count = 0;
        } else if (state.scenario == EnumerationCase::GrowOnce &&
                   state.count_calls == 1) {
            *count = 1;
        } else if (state.scenario == EnumerationCase::Shrink) {
            *count = 3;
        } else {
            *count = 2;
        }
        return VK_SUCCESS;
    }

    ++state.fill_calls;
    devices[0] = FakePhysicalDevice(1);
    if (state.scenario == EnumerationCase::GrowOnce && state.fill_calls == 1) {
        *count = 1; // Vulkan reports the one handle written, not the total now available.
        return VK_INCOMPLETE;
    }
    if (state.scenario == EnumerationCase::AlwaysIncomplete) {
        *count = 2;
        return VK_INCOMPLETE;
    }
    if (state.scenario == EnumerationCase::Shrink) {
        *count = 1;
        return VK_SUCCESS;
    }
    devices[1] = FakePhysicalDevice(2);
    *count = 2;
    return VK_SUCCESS;
}

} // Anonymous namespace

TEST_CASE("Vulkan physical devices: retry VK_INCOMPLETE after adapter list growth",
          "[vulkan_enumeration]") {
    EnumerationState state{.scenario = EnumerationCase::GrowOnce};
    active_state = &state;
    const auto devices = Vulkan::vk::detail::EnumeratePhysicalDevicesWithRetry(
        VK_NULL_HANDLE, &FakeEnumerate);
    REQUIRE(devices.size() == 2);
    REQUIRE(devices[0] == FakePhysicalDevice(1));
    REQUIRE(devices[1] == FakePhysicalDevice(2));
    REQUIRE(state.count_calls == 2);
    REQUIRE(state.fill_calls == 2);
}

TEST_CASE("Vulkan physical devices: discard trailing slots when the list shrinks",
          "[vulkan_enumeration]") {
    EnumerationState state{.scenario = EnumerationCase::Shrink};
    active_state = &state;
    const auto devices = Vulkan::vk::detail::EnumeratePhysicalDevicesWithRetry(
        VK_NULL_HANDLE, &FakeEnumerate);
    REQUIRE(devices.size() == 1);
    REQUIRE(devices[0] == FakePhysicalDevice(1));
    REQUIRE(state.count_calls == 1);
    REQUIRE(state.fill_calls == 1);
}

TEST_CASE("Vulkan physical devices: zero adapters returns an empty list",
          "[vulkan_enumeration]") {
    EnumerationState state{.scenario = EnumerationCase::Empty};
    active_state = &state;
    REQUIRE(Vulkan::vk::detail::EnumeratePhysicalDevicesWithRetry(
                VK_NULL_HANDLE, &FakeEnumerate).empty());
    REQUIRE(state.fill_calls == 0);
}

TEST_CASE("Vulkan physical devices: incomplete and fatal errors remain bounded",
          "[vulkan_enumeration]") {
    EnumerationState incomplete{.scenario = EnumerationCase::AlwaysIncomplete};
    active_state = &incomplete;
    REQUIRE_THROWS_AS(Vulkan::vk::detail::EnumeratePhysicalDevicesWithRetry(
                          VK_NULL_HANDLE, &FakeEnumerate), Vulkan::vk::Exception);
    REQUIRE(incomplete.count_calls == 4);
    REQUIRE(incomplete.fill_calls == 4);

    EnumerationState fatal{.scenario = EnumerationCase::FatalCount};
    active_state = &fatal;
    REQUIRE_THROWS_AS(Vulkan::vk::detail::EnumeratePhysicalDevicesWithRetry(
                          VK_NULL_HANDLE, &FakeEnumerate), Vulkan::vk::Exception);
    REQUIRE(fatal.count_calls == 1);
    REQUIRE(fatal.fill_calls == 0);
}
