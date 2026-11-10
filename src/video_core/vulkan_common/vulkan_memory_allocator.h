// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/vulkan_common/vma.h"

namespace Vulkan {

    class Device;

/// Hints and requirements for the backing memory type of a commit
    enum class MemoryUsage {
        DeviceLocal, ///< Requests device local host visible buffer, falling back to device local memory.
        Upload,      ///< Requires a host visible memory type optimized for CPU to GPU uploads
        Download,    ///< Requires a host visible memory type optimized for GPU to CPU readbacks
        Stream,      ///< Requests device local host visible buffer, falling back host memory.
    };

    template<typename F>
    void ForEachDeviceLocalHostVisibleHeap(const Device &device, F &&f) {
        auto memory_props = device.GetPhysical().GetMemoryProperties().memoryProperties;
        for (size_t i = 0; i < memory_props.memoryTypeCount; i++) {
            auto &memory_type = memory_props.memoryTypes[i];
            if ((memory_type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
                (memory_type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                f(memory_type.heapIndex, memory_props.memoryHeaps[memory_type.heapIndex]);
            }
        }
    }

/// Ownership handle of a memory commitment (real VMA allocation).
    class MemoryCommit {
    public:
        MemoryCommit() noexcept = default;

        MemoryCommit(VmaAllocator allocator, VmaAllocation allocation,
                     const VmaAllocationInfo &info) noexcept;

        ~MemoryCommit();

        MemoryCommit(const MemoryCommit &) = delete;

        MemoryCommit &operator=(const MemoryCommit &) = delete;

        MemoryCommit(MemoryCommit &&) noexcept;

        MemoryCommit &operator=(MemoryCommit &&) noexcept;

        [[nodiscard]] std::span<u8> Map();

        [[nodiscard]] std::span<const u8> Map() const;

        void Unmap();

        explicit operator bool() const noexcept { return allocation != nullptr; }

        VkDeviceMemory Memory() const noexcept { return memory; }

        VkDeviceSize Offset() const noexcept { return offset; }

        VkDeviceSize Size() const noexcept { return size; }

        VmaAllocation Allocation() const noexcept { return allocation; }

    private:
        void Release();

        VmaAllocator allocator{};   ///< VMA allocator
        VmaAllocation allocation{};  ///< VMA allocation handle
        VkDeviceMemory memory{};      ///< Underlying VkDeviceMemory chosen by VMA
        VkDeviceSize offset{};      ///< Offset of this allocation inside VkDeviceMemory
        VkDeviceSize size{};        ///< Size of the allocation
        void *mapped_ptr{};  ///< Optional persistent mapped pointer
    };

/// Memory allocator container.
/// Allocates and releases memory allocations on demand.
    class MemoryAllocator {
    public:
        /**
         * Construct memory allocator
         *
         * @param device_  Device to allocate from
         *
         * @throw vk::Exception on failure
         */
        explicit MemoryAllocator(const Device &device_);

        ~MemoryAllocator();

        MemoryAllocator &operator=(const MemoryAllocator &) = delete;

        MemoryAllocator(const MemoryAllocator &) = delete;

        /// `dedicated`: own device memory, returned to the driver on destruction (short-lived
        /// images would otherwise leave holes in the shared blocks).
        /// texture: not a render target; near the video memory budget it goes to host memory, so
        /// render targets keep RenderTargetReserveBytes of it.
        vk::Image CreateImage(const VkImageCreateInfo &ci, bool dedicated = false,
                              bool texture = false) const;

        /// CreateImage, but a null image instead of an exception when there is no memory for it.
        /// scratch: a texture that only lives until the GPU is done with it (an ASTC decode for
        /// the BC encoder); it counts twice its size against the render target reserve.
        vk::Image TryCreateImage(const VkImageCreateInfo &ci, bool dedicated = false,
                                 bool texture = false, bool scratch = false) const;

        /// A sparse resident image with nothing bound (reads return zero, writes are dropped), or
        /// a null image when the device can't make one.
        /// aliased: its sparse blocks may share memory (VK_IMAGE_CREATE_SPARSE_ALIASED_BIT).
        vk::Image CreateUnbackedImage(const VkImageCreateInfo &ci, bool aliased = false) const;

        /// Device memory to bind to part of a sparse resource. Null when there is none left.
        /// Device-local buffers in their own VMA pool; false when it doesn't apply or failed.
        bool CreateBufferInSeparatePool(const VkBufferCreateInfo& ci, MemoryUsage usage,
                                        const VmaAllocationCreateInfo& alloc_ci, VkBuffer& handle,
                                        VmaAllocation& allocation,
                                        VmaAllocationInfo& alloc_info) const;

        /// Counts a device-local buffer VMA placed outside video memory.
        void NoteDeviceLocalBuffer(MemoryUsage usage, VkMemoryPropertyFlags flags,
                                   VkDeviceSize size) const;

        VmaAllocation AllocateSparseMemory(const VkMemoryRequirements &requirements,
                                           VkDeviceMemory &memory, VkDeviceSize &offset) const;

        void FreeSparseMemory(VmaAllocation allocation) const;

        /// Counts the images that did not fit the device memory budget (made anyway, in host
        /// memory or with nothing bound): the texture cache collects harder when it grows.
        u32 MemoryPressureEvents() const noexcept {
            return pressure_events.load(std::memory_order_relaxed);
        }

        /// Counts the images that could not be made at all (no memory left anywhere): the
        /// texture cache collects aggressively when it grows.
        u32 FailedImages() const noexcept {
            return failed_images.load(std::memory_order_relaxed);
        }

        /// Logs, at Info, how many images and device-local buffers ended outside video memory
        /// since the start and how much lives now where only they go. Silent when nothing changed
        /// since its last line; the texture cache calls it every 600 frames.
        void LogOutsideVideoMemory() const;

        vk::Buffer CreateBuffer(const VkBufferCreateInfo &ci, MemoryUsage usage) const;

        vk::Buffer CreateBuffer(const VkBufferCreateInfo &ci, MemoryUsage usage,
                                VkDeviceSize min_alignment) const;

        /**
         * Commits a memory with the specified requirements.
         *
         * @param requirements Requirements returned from a Vulkan call.
         * @param usage        Indicates how the memory will be used.
         *
         * @returns A memory commit.
         */
        MemoryCommit Commit(const VkMemoryRequirements &requirements, MemoryUsage usage);

        /// Commits memory required by the buffer and binds it (for buffers created outside VMA).
        MemoryCommit Commit(const vk::Buffer &buffer, MemoryUsage usage);

    private:
        static bool IsAutoUsage(VmaMemoryUsage u) noexcept {
            switch (u) {
                case VMA_MEMORY_USAGE_AUTO:
                case VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE:
                case VMA_MEMORY_USAGE_AUTO_PREFER_HOST:
                    return true;
                default:
                    return false;
            }
        }

        /// CreateImage's fallbacks; the image is left null when the result is an error.
        /// may_fail: the caller handles VK_ERROR_OUT_OF_DEVICE_MEMORY (see UsesUnbackedFallback).
        VkResult AllocateImage(const VkImageCreateInfo &ci, bool dedicated, bool may_fail,
                               bool texture, bool scratch, vk::Image &image) const;

        /// Whether some memory type is not device-local (false on an integrated GPU).
        bool HasHostMemoryApartFromVideo() const noexcept;

        /// The largest device-local heap (where images go), or ~0 without one.
        u32 MainDeviceLocalHeap() const noexcept;

        /// The memory an image made from `ci` needs: from vkGetDeviceImageMemoryRequirements, or
        /// before Vulkan 1.3 from a probe image with nothing bound; 0 when the probe can't be made.
        u64 ImageBytes(const VkImageCreateInfo &ci) const;

        const Device &device;                              ///< Device handle.
        VmaAllocator allocator;                           ///< VMA allocator.
        const VkPhysicalDeviceMemoryProperties properties; ///< Physical device memory properties.
        VkDeviceSize buffer_image_granularity;            ///< Adjacent buffer/image granularity
        /// vkGetDeviceImageMemoryRequirements (Vulkan 1.3), or null: ImageBytes makes a probe image.
        PFN_vkGetDeviceImageMemoryRequirements device_image_requirements{};
        u32 valid_memory_types{~0u};
        mutable std::atomic<u32> pressure_events{0};
        mutable std::atomic<u32> failed_images{0};
        mutable std::atomic<u64> images_outside{0};
        mutable std::atomic<u64> image_bytes_outside{0};
        mutable std::atomic<u64> buffers_outside{0};
        mutable std::atomic<u64> buffer_bytes_outside{0};
        mutable std::atomic<u64> textures_yielded{0};
        mutable std::atomic<u64> texture_bytes_yielded{0};
        mutable bool outside_logged{};            ///< LogOutsideVideoMemory: a line was written
        mutable std::array<u64, 7> last_outside{}; ///< LogOutsideVideoMemory: its last figures
        mutable std::mutex buffer_type_mutex;
        /// Memory type of device-local buffers by usage and flags (CreateBufferInSeparatePool).
        mutable std::unordered_map<u64, u32> buffer_memory_types;
    };

    /// Logs VMA's blocks and allocations per memory type, and each heap's budget.
    void LogMemoryStatistics(const Device& device);

    /// One line each for the process's memory, what DXGI counts for the GPU (Windows), and VMA's
    /// heaps and memory types against what the driver reports (EDEN_NX2_MEM_TRACE).
    void LogMemoryTrace(const Device& device);

} // namespace Vulkan
