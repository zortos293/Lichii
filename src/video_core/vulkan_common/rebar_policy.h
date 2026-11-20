// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <span>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan.h"

// Experiment (test/rebar-uploads): with Resizable BAR or Smart Access Memory on, the device-local
// heap is host-visible as a whole, and the buffer cache's buffers can live in host-visible VRAM so
// guest buffer uploads are written into them directly instead of staged and copied.
// EDEN_NX2_REBAR=0 keeps the staging path.
namespace Vulkan::Rebar {

/// Smallest host-visible device-local heap taken for ReBAR. Without it, discrete GPUs expose a
/// 256 MiB window (NVIDIA ~246 MiB, AMD 256 MiB).
inline constexpr u64 MIN_HEAP_SIZE = u64{1} << 30;

/// Largest upload written directly, a stream buffer region: smaller ones are already written to
/// host-visible VRAM there, so going direct only drops the GPU copy. Larger ones stage in system
/// RAM, where the CPU writes faster than across PCIe.
inline constexpr u64 MAX_DIRECT_UPLOAD_SIZE = u64{8} << 20;

enum class Reason {
    On,
    KillSwitch, ///< EDEN_NX2_REBAR=0
    Integrated, ///< Integrated or UMA GPU: all its memory is host-visible already
    NoHeap,     ///< No device-local, host-visible, host-coherent memory type
    SmallHeap,  ///< Only the classic BAR window
};

struct Decision {
    bool enabled{};
    Reason reason{};
    u32 heap_index{};
    u32 memory_type_bits{}; ///< Device-local, host-visible, host-coherent types on that heap
    u64 heap_size{};
    u64 budget{};
};

[[nodiscard]] inline const char* ReasonText(Reason reason) {
    switch (reason) {
    case Reason::On:
        return "resizable BAR heap";
    case Reason::KillSwitch:
        return "EDEN_NX2_REBAR=0";
    case Reason::Integrated:
        return "integrated or UMA GPU";
    case Reason::NoHeap:
        return "no host-visible device-local memory";
    case Reason::SmallHeap:
        return "BAR window only, ReBAR/SAM off";
    }
    return "unknown";
}

[[nodiscard]] inline bool KillSwitchSet() {
    const char* const value = std::getenv("EDEN_NX2_REBAR");
    return value != nullptr && std::strcmp(value, "0") == 0;
}

/// Whether every device-local memory type is host-visible too (unified memory).
[[nodiscard]] inline bool IsUnifiedMemory(const VkPhysicalDeviceMemoryProperties& props) {
    bool any_device_local = false;
    for (u32 i = 0; i < props.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = props.memoryTypes[i].propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0) {
            continue;
        }
        any_device_local = true;
        if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
            return false;
        }
    }
    return any_device_local;
}

/// `budgets`: each heap's budget (VK_EXT_memory_budget through VMA), 0 where unknown.
[[nodiscard]] inline Decision Decide(const VkPhysicalDeviceMemoryProperties& props,
                                     std::span<const u64> budgets, bool integrated,
                                     bool kill_switch) {
    constexpr VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    Decision decision{};
    for (u32 i = 0; i < props.memoryTypeCount; ++i) {
        const VkMemoryType& type = props.memoryTypes[i];
        if ((type.propertyFlags & wanted) != wanted ||
            (type.propertyFlags & VK_MEMORY_PROPERTY_PROTECTED_BIT) != 0) {
            continue;
        }
        const u64 size = props.memoryHeaps[type.heapIndex].size;
        if (size > decision.heap_size) {
            decision.heap_index = type.heapIndex;
            decision.heap_size = size;
            decision.memory_type_bits = 0;
        }
        if (type.heapIndex == decision.heap_index) {
            decision.memory_type_bits |= 1U << i;
        }
    }
    if (decision.heap_size != 0) {
        const u64 budget =
            decision.heap_index < budgets.size() ? budgets[decision.heap_index] : u64{0};
        decision.budget = budget != 0 ? (std::min)(budget, decision.heap_size)
                                      : decision.heap_size;
    }
    if (kill_switch) {
        decision.reason = Reason::KillSwitch;
    } else if (integrated || IsUnifiedMemory(props)) {
        decision.reason = Reason::Integrated;
    } else if (decision.heap_size == 0) {
        decision.reason = Reason::NoHeap;
    } else if (decision.heap_size < MIN_HEAP_SIZE) {
        decision.reason = Reason::SmallHeap;
    } else {
        decision.reason = Reason::On;
        decision.enabled = true;
    }
    return decision;
}

/// Whether a direct buffer of `size` bytes keeps the heap within its budget, an eighth of it
/// held back for images and the driver: past it, buffers take the staging path's memory.
[[nodiscard]] inline bool FitsBudget(u64 heap_usage, u64 heap_budget, u64 size) {
    const u64 limit = heap_budget - heap_budget / 8;
    return heap_usage <= limit && size <= limit - heap_usage;
}

} // namespace Vulkan::Rebar
