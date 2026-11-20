// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/rebar_policy.h"

namespace {

using namespace Vulkan;

constexpr u64 MiB = u64{1} << 20;
constexpr u64 GiB = u64{1} << 30;
constexpr VkMemoryPropertyFlags DEVICE = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
constexpr VkMemoryPropertyFlags HOST =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
constexpr VkMemoryPropertyFlags CACHED = HOST | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

struct Type {
    VkMemoryPropertyFlags flags;
    u32 heap;
};

VkPhysicalDeviceMemoryProperties Props(std::initializer_list<u64> heaps,
                                       std::initializer_list<Type> types) {
    VkPhysicalDeviceMemoryProperties props{};
    for (const u64 size : heaps) {
        const bool device = props.memoryHeapCount == 0;
        props.memoryHeaps[props.memoryHeapCount++] = VkMemoryHeap{
            .size = size,
            .flags = device ? VkMemoryHeapFlags{VK_MEMORY_HEAP_DEVICE_LOCAL_BIT} : 0,
        };
    }
    for (const Type& type : types) {
        props.memoryTypes[props.memoryTypeCount++] = VkMemoryType{
            .propertyFlags = type.flags,
            .heapIndex = type.heap,
        };
    }
    return props;
}

// RTX 4080 on Windows: 16 GB VRAM, system RAM, and the BAR window as its own heap.
VkPhysicalDeviceMemoryProperties NvidiaWindow() {
    return Props({16 * GiB, 32 * GiB, 246 * MiB},
                 {{DEVICE, 0}, {HOST, 1}, {CACHED, 1}, {DEVICE | HOST, 2}});
}

// The same card with Resizable BAR on: the host-visible device-local type is on the VRAM heap.
VkPhysicalDeviceMemoryProperties NvidiaRebar() {
    return Props({16 * GiB, 32 * GiB}, {{DEVICE, 0}, {HOST, 1}, {CACHED, 1}, {DEVICE | HOST, 0}});
}

} // Anonymous namespace

TEST_CASE("ReBAR heap with its budget turns direct uploads on", "[rebar]") {
    const std::array<u64, 2> budgets{14 * GiB, 30 * GiB};
    const Rebar::Decision decision = Rebar::Decide(NvidiaRebar(), budgets, false, false);
    REQUIRE(decision.enabled);
    REQUIRE(decision.reason == Rebar::Reason::On);
    REQUIRE(decision.heap_index == 0);
    REQUIRE(decision.heap_size == 16 * GiB);
    REQUIRE(decision.budget == 14 * GiB);
    REQUIRE(decision.memory_type_bits == (1U << 3));
}

TEST_CASE("The classic 256 MiB BAR window keeps the staging path", "[rebar]") {
    const std::array<u64, 3> budgets{14 * GiB, 30 * GiB, 200 * MiB};
    const Rebar::Decision decision = Rebar::Decide(NvidiaWindow(), budgets, false, false);
    REQUIRE_FALSE(decision.enabled);
    REQUIRE(decision.reason == Rebar::Reason::SmallHeap);
    REQUIRE(decision.heap_index == 2);
    REQUIRE(decision.heap_size == 246 * MiB);
    REQUIRE(decision.budget == 200 * MiB);
}

TEST_CASE("Integrated and UMA GPUs keep their path", "[rebar]") {
    // Radeon 780M: everything is host-visible system memory.
    const auto apu = Props({16 * GiB, 512 * MiB}, {{DEVICE | HOST, 0}, {DEVICE | CACHED, 0},
                                                    {DEVICE | HOST, 1}});
    REQUIRE(Rebar::IsUnifiedMemory(apu));
    REQUIRE(Rebar::Decide(apu, {}, false, false).reason == Rebar::Reason::Integrated);
    REQUIRE_FALSE(Rebar::IsUnifiedMemory(NvidiaRebar()));
    const Rebar::Decision integrated = Rebar::Decide(NvidiaRebar(), {}, true, false);
    REQUIRE_FALSE(integrated.enabled);
    REQUIRE(integrated.reason == Rebar::Reason::Integrated);
}

TEST_CASE("EDEN_NX2_REBAR=0 turns direct uploads off", "[rebar]") {
    const Rebar::Decision decision = Rebar::Decide(NvidiaRebar(), {}, false, true);
    REQUIRE_FALSE(decision.enabled);
    REQUIRE(decision.reason == Rebar::Reason::KillSwitch);
    REQUIRE(decision.heap_size == 16 * GiB);
}

TEST_CASE("No coherent host-visible device-local type keeps the staging path", "[rebar]") {
    const auto props = Props({8 * GiB, 16 * GiB},
                             {{DEVICE, 0}, {HOST, 1},
                              {DEVICE | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0}});
    const Rebar::Decision decision = Rebar::Decide(props, {}, false, false);
    REQUIRE_FALSE(decision.enabled);
    REQUIRE(decision.reason == Rebar::Reason::NoHeap);
    REQUIRE(decision.heap_size == 0);
}

TEST_CASE("Unknown or oversized budgets fall back to the heap size", "[rebar]") {
    REQUIRE(Rebar::Decide(NvidiaRebar(), {}, false, false).budget == 16 * GiB);
    const std::array<u64, 2> zero{0, 0};
    REQUIRE(Rebar::Decide(NvidiaRebar(), zero, false, false).budget == 16 * GiB);
    const std::array<u64, 2> over{20 * GiB, 0};
    REQUIRE(Rebar::Decide(NvidiaRebar(), over, false, false).budget == 16 * GiB);
}

TEST_CASE("Direct buffers stay within seven eighths of the heap budget", "[rebar]") {
    const u64 budget = 8 * GiB;
    REQUIRE(Rebar::FitsBudget(0, budget, 7 * GiB));
    REQUIRE_FALSE(Rebar::FitsBudget(0, budget, 7 * GiB + 1));
    REQUIRE(Rebar::FitsBudget(6 * GiB, budget, GiB));
    REQUIRE_FALSE(Rebar::FitsBudget(6 * GiB, budget, GiB + 1));
    REQUIRE_FALSE(Rebar::FitsBudget(budget, budget, 1));
    REQUIRE_FALSE(Rebar::FitsBudget(0, 0, 1));
    REQUIRE_FALSE(Rebar::FitsBudget(GiB, budget, ~u64{0}));
}
