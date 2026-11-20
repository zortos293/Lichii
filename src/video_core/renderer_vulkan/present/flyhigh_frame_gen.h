// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

struct FhEngine;
struct FhModel;
struct FhStream;
struct FhVkBuffer;
struct FhVkSemaphore;

namespace VideoCore::FrameGen {
struct FlyHighLibrary;
}

namespace Vulkan {

class Device;
class Scheduler;

/// FlyHigh frame generation: a neural network on its own Vulkan device. Each presented frame is
/// copied into memory both devices share and handed over with a timeline semaphore; the
/// in-betweens come back the same way and are copied into the frames Lichii presents.
class FlyHighFrameGen {
public:
    static constexpr size_t MAX_GENERATIONS = 3;

    /// Throws std::runtime_error with the reason when FlyHigh can't run on this device.
    explicit FlyHighFrameGen(const Device& device, MemoryAllocator& memory_allocator,
                             VkExtent2D extent, VkFormat format, bool protect_hud);
    ~FlyHighFrameGen();

    FlyHighFrameGen(const FlyHighFrameGen&) = delete;
    FlyHighFrameGen& operator=(const FlyHighFrameGen&) = delete;

    /// Hands the frame in source (GENERAL layout) to FlyHigh and asks for `generations`
    /// in-betweens of it and the frame before. Returns how many it made.
    [[nodiscard]] size_t Process(Scheduler& scheduler, VkImage source, size_t generations);

    /// Copies in-between `generation` into destination, left in GENERAL layout.
    void Generate(Scheduler& scheduler, VkImage destination, size_t generation);

    /// What the submission of the first generated frame waits for: FlyHigh has written every
    /// in-between. Empty after it has been taken once.
    [[nodiscard]] SemaphoreValue TakeWait();

private:
    struct SharedBuffer {
        VkBuffer buffer{};
        vk::DeviceMemory memory;
        FhVkBuffer* imported{};
    };

    struct SharedTimeline {
        vk::Semaphore semaphore;
        FhVkSemaphore* imported{};
        u64 value{};
    };

    void Build(MemoryAllocator& memory_allocator, VkFormat format, bool protect_hud);
    void Release() noexcept;
    void CreateSharedBuffer(SharedBuffer& shared);
    void CreateSharedTimeline(SharedTimeline& shared);
    void Check(int result, const char* call) const;

    const VideoCore::FrameGen::FlyHighLibrary& fh;
    const Device& device;
    VkExtent2D extent;
    u32 row_pitch;
    VkDeviceSize buffer_size;

    vk::Image staging;
    std::array<SharedBuffer, 2> inputs{};
    std::array<SharedBuffer, MAX_GENERATIONS> outputs{};
    /// One semaphore each way, each signalled by one device only.
    SharedTimeline frames_in;
    SharedTimeline frames_out;
    u64 pending_wait{};

    FhEngine* engine{};
    FhModel* model{};
    FhStream* stream{};
    u64 frame_count{};
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
};

} // namespace Vulkan
