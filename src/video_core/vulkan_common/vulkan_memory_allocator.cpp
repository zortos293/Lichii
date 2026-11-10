// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <dxgi1_4.h>
#endif

#include <cstring>
#include <iterator>
#include <string>

#include <fmt/format.h>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/common_types.h"
#include "common/literals.h"
#include "common/logging.h"
#include "common/memory_detect.h"
#include <ranges>
#include "video_core/vulkan_common/image_placement.h"
#include "video_core/vulkan_common/unbacked_fallback.h"
#include "video_core/vulkan_common/vma.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "common/settings.h"

namespace Vulkan {
namespace {

// Helpers translating MemoryUsage to flags/usage

    [[maybe_unused]] VkMemoryPropertyFlags MemoryUsagePropertyFlags(MemoryUsage usage) {
        switch (usage) {
            case MemoryUsage::DeviceLocal:
                return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            case MemoryUsage::Upload:
                return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            case MemoryUsage::Download:
                return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            case MemoryUsage::Stream:
                return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        }
        ASSERT_MSG(false, "Invalid memory usage={}", usage);
        return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }

    [[nodiscard]] VkMemoryPropertyFlags MemoryUsagePreferredVmaFlags(MemoryUsage usage) {
        if (usage == MemoryUsage::Download) {
            return VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        }
        return usage != MemoryUsage::DeviceLocal ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                    : VkMemoryPropertyFlagBits{};
    }

    [[nodiscard]] VmaAllocationCreateFlags MemoryUsageVmaFlags(MemoryUsage usage) {
        switch (usage) {
            case MemoryUsage::Upload:
            case MemoryUsage::Stream:
                return VMA_ALLOCATION_CREATE_MAPPED_BIT |
                        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            case MemoryUsage::Download:
                return VMA_ALLOCATION_CREATE_MAPPED_BIT |
                        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
            case MemoryUsage::DeviceLocal:
                return {};
        }
        return {};
    }

    [[nodiscard]] VmaMemoryUsage MemoryUsageVma(MemoryUsage usage) {
        switch (usage) {
            case MemoryUsage::DeviceLocal:
            case MemoryUsage::Stream:
                return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            case MemoryUsage::Upload:
            case MemoryUsage::Download:
                return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        }
        return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }
} // namespace

//MemoryCommit is now VMA-backed
MemoryCommit::MemoryCommit(VmaAllocator alloc, VmaAllocation a,
                            const VmaAllocationInfo &info) noexcept
        : allocator{alloc}, allocation{a}, memory{info.deviceMemory},
            offset{info.offset}, size{info.size}, mapped_ptr{info.pMappedData} {
    // Log GPU memory allocation
    if (GPU::Logging::IsActive() &&
        Settings::values.gpu_log_memory_tracking.GetValue()) {
        GPU::Logging::GPULogger::GetInstance().LogMemoryAllocation(
            reinterpret_cast<uintptr_t>(memory),
            static_cast<u64>(size),
            0  // Memory property flags (not easily available from VMA)
        );
    }
}

MemoryCommit::~MemoryCommit() { Release(); }

MemoryCommit::MemoryCommit(MemoryCommit &&rhs) noexcept
        : allocator{std::exchange(rhs.allocator, nullptr)},
            allocation{std::exchange(rhs.allocation, nullptr)},
            memory{std::exchange(rhs.memory, VK_NULL_HANDLE)},
            offset{std::exchange(rhs.offset, 0)},
            size{std::exchange(rhs.size, 0)},
            mapped_ptr{std::exchange(rhs.mapped_ptr, nullptr)} {}

MemoryCommit &MemoryCommit::operator=(MemoryCommit &&rhs) noexcept {
    if (this != &rhs) {
        Release();
        allocator = std::exchange(rhs.allocator, nullptr);
        allocation = std::exchange(rhs.allocation, nullptr);
        memory = std::exchange(rhs.memory, VK_NULL_HANDLE);
        offset = std::exchange(rhs.offset, 0);
        size = std::exchange(rhs.size, 0);
        mapped_ptr = std::exchange(rhs.mapped_ptr, nullptr);
    }
    return *this;
}

std::span<u8> MemoryCommit::Map()
{
    if (!allocation) return {};
    if (!mapped_ptr) {
        if (vmaMapMemory(allocator, allocation, &mapped_ptr) != VK_SUCCESS) return {};
    }
    const size_t n = static_cast<size_t>(std::min<VkDeviceSize>(size,
                                                                (std::numeric_limits<size_t>::max)()));
    return std::span<u8>{static_cast<u8 *>(mapped_ptr), n};
}

std::span<const u8> MemoryCommit::Map() const
{
    if (!allocation) return {};
    if (!mapped_ptr) {
        void *p = nullptr;
        if (vmaMapMemory(allocator, allocation, &p) != VK_SUCCESS) return {};
        const_cast<MemoryCommit *>(this)->mapped_ptr = p;
    }
    const size_t n = static_cast<size_t>(std::min<VkDeviceSize>(size,
                                                                (std::numeric_limits<size_t>::max)()));
    return std::span<const u8>{static_cast<const u8 *>(mapped_ptr), n};
}

void MemoryCommit::Unmap()
{
    if (allocation && mapped_ptr) {
        vmaUnmapMemory(allocator, allocation);
        mapped_ptr = nullptr;
    }
}

void MemoryCommit::Release() {
    if (allocation && allocator) {
        // Log GPU memory deallocation
        if (GPU::Logging::IsActive() &&
            Settings::values.gpu_log_memory_tracking.GetValue() &&
            memory != VK_NULL_HANDLE) {
            GPU::Logging::GPULogger::GetInstance().LogMemoryDeallocation(
                reinterpret_cast<uintptr_t>(memory)
            );
        }

        if (mapped_ptr) {
            vmaUnmapMemory(allocator, allocation);
            mapped_ptr = nullptr;
        }
        vmaFreeMemory(allocator, allocation);
    }
    allocation = nullptr;
    allocator = nullptr;
    memory = VK_NULL_HANDLE;
    offset = 0;
    size = 0;
}

MemoryAllocator::MemoryAllocator(const Device &device_)
        : device{device_}, allocator{device.GetAllocator()},
            properties{device_.GetPhysical().GetMemoryProperties().memoryProperties},
            buffer_image_granularity{
                    device_.GetPhysical().GetProperties().limits.bufferImageGranularity} {
    // Image sizes for the render target reserve without making a probe image per texture.
    if (HasDeviceImageRequirements(device.ApiVersion())) {
        device_image_requirements = reinterpret_cast<PFN_vkGetDeviceImageMemoryRequirements>(
            device.GetDispatchLoader().vkGetDeviceProcAddr(*device.GetLogical(),
                                                           "vkGetDeviceImageMemoryRequirements"));
    }

    // Preserve the previous "RenderDoc small heap" trimming behavior that we had in original vma minus the heap bug
    if (device.HasDebuggingToolAttached())
    {
        using namespace Common::Literals;
        ForEachDeviceLocalHostVisibleHeap(device, [this](size_t heap_idx, VkMemoryHeap &heap) {
            if (heap.size <= 256_MiB) {
                for (u32 t = 0; t < properties.memoryTypeCount; ++t) {
                    if (properties.memoryTypes[t].heapIndex == heap_idx) {
                        valid_memory_types &= ~(1u << t);
                    }
                }
            }
        });
    }
}

MemoryAllocator::~MemoryAllocator() = default;

vk::Image MemoryAllocator::CreateImage(const VkImageCreateInfo &ci, bool dedicated,
                                       bool texture) const
{
    vk::Image image;
    vk::Check(AllocateImage(ci, dedicated, false, texture, false, image));
    return image;
}

vk::Image MemoryAllocator::TryCreateImage(const VkImageCreateInfo &ci, bool dedicated,
                                          bool texture, bool scratch) const {
    vk::Image image;
    const VkResult result = AllocateImage(ci, dedicated, true, texture, scratch, image);
    if (result != VK_ERROR_OUT_OF_DEVICE_MEMORY && result != VK_ERROR_OUT_OF_HOST_MEMORY) {
        vk::Check(result);
    }
    return image;
}

VkResult MemoryAllocator::AllocateImage(const VkImageCreateInfo &ci, bool dedicated, bool may_fail,
                                        bool texture, bool scratch, vk::Image &image) const {
    // Images require device-local memory. When they only preferred it, with the device-local heap
    // at its budget VMA moved them to the system memory type without a word: on a GTX 1650 (4 GB,
    // no ReBAR) a title's load left 805 MiB of images there (VMA type 0: 297 allocations), 2 GB/s
    // over PCIe each way at 6 fps, and lower texture cache thresholds did not help. Host memory
    // is only the last, logged fallback.
    // Only where host memory is a separate type: on an integrated GPU every type is device-local.
    const bool separate = HasHostMemoryApartFromVideo();
    const auto steps = ImagePlacementSteps(separate);
    const auto step_ci = [dedicated](const ImagePlacementStep& step) {
        return VmaAllocationCreateInfo{
            // Best fit: images freed by the thousand leave holes in the blocks, which new ones
            // should fill before VMA takes another block.
            .flags = static_cast<VmaAllocationCreateFlags>(
                         VMA_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT) |
                     (step.within_budget
                          ? VmaAllocationCreateFlags{VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT}
                          : VmaAllocationCreateFlags{0}) |
                     (dedicated ? VmaAllocationCreateFlags{VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT}
                                : VmaAllocationCreateFlags{0}),
            .usage = step.prefer_host ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST
                                      : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .requiredFlags = step.require_device_local
                                 ? VkMemoryPropertyFlags{VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT}
                                 : VkMemoryPropertyFlags{0},
            .preferredFlags = step.prefer_host
                                  ? VkMemoryPropertyFlags{0}
                                  : VkMemoryPropertyFlags{VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT},
            .memoryTypeBits = 0,
            .pool = VK_NULL_HANDLE,
            .pUserData = nullptr,
            .priority = 0.f,
        };
    };
    const VmaAllocationCreateInfo alloc_ci = step_ci(steps[0]);

    VkImage handle{};
    VmaAllocation allocation{};
    VmaAllocationInfo alloc_info{};
    const u64 texels = u64{ci.extent.width} * ci.extent.height * ci.extent.depth * ci.arrayLayers;
    const bool huge = texels > MaxBackedImageTexels;
    // A texture with the device-local heap within RenderTargetReserveBytes of its budget goes to
    // host memory now, so the render targets made after it still fit: on a 4 GB GTX 1650 the
    // textures of a title's load filled the heap and its depth buffer and targets went to system
    // memory. Not a pressure event: those made the texture cache collect ever harder (1923 images
    // downloaded in 600 frames).
    // The image's own size counts: with the reserve at 1024 MiB, something over 512 MiB still got
    // in right before a 1365x1365x90 array's BC1 copy, and the depth buffers and targets made next
    // did not fit even over the budget. A decode image for the BC encoder that is too big to be
    // kept counts twice its size: on the 4 GB card it comes with the heap already past the reserve
    // (2186 and 2442 of 3702 MiB); on a 12 GB card, counted once, two 914 MiB decodes got in and
    // the 110 textures made before the next frame freed them yielded (515 MiB).
    bool yielded = false;
    u64 heap_usage = 0;
    u64 heap_budget = 0;
    u64 image_bytes = 0;
    if (texture && !huge && separate) {
        if (const u32 heap = MainDeviceLocalHeap(); heap != ~0U) {
            std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
            vmaGetHeapBudgets(allocator, budgets.data());
            heap_usage = budgets[heap].usage;
            heap_budget = budgets[heap].budget;
            if (const u64 reserve = RenderTargetReserveBytes(heap_budget); reserve != 0) {
                image_bytes = ImageBytes(ci);
                yielded = YieldsToRenderTargets(false, heap_usage,
                                                PlacementBytes(scratch, image_bytes), heap_budget,
                                                reserve);
            }
        }
    }
    VkResult result{};
    if (huge) {
        result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    } else if (yielded) {
        const VmaAllocationCreateInfo host = step_ci(steps[2]);
        result = vmaCreateImage(allocator, &ci, &host, &handle, &allocation, &alloc_info);
        if (result == VK_SUCCESS) {
            textures_yielded.fetch_add(1, std::memory_order_relaxed);
            texture_bytes_yielded.fetch_add(alloc_info.size, std::memory_order_relaxed);
            static u32 yield_logged{};
            if (yield_logged++ < 16) {
                LOG_INFO(Render_Vulkan,
                         "Image {}x{}x{} ({} levels, {} layers, format {}, {} MiB) placed in host "
                         "memory: it would end within the render target reserve{}; video memory "
                         "{} of {} MiB",
                         ci.extent.width, ci.extent.height, ci.extent.depth, ci.mipLevels,
                         ci.arrayLayers, static_cast<u32>(ci.format), image_bytes >> 20,
                         scratch ? " (decode image, counted twice)" : "", heap_usage >> 20,
                         heap_budget >> 20);
            }
        }
    } else {
        result = vmaCreateImage(allocator, &ci, &alloc_ci, &handle, &allocation, &alloc_info);
    }
    // No device-local memory type takes this image at all: straight to the host fallback, as
    // before images required one.
    const bool no_device_type = result == VK_ERROR_FEATURE_NOT_PRESENT && steps[0].require_device_local;
    if (no_device_type) {
        result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    // A texture that yielded never gets a second try in video memory: that would take the room the
    // reserve keeps for render targets. If host memory refused it, the host retry below decides.
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && !huge && !no_device_type && !yielded) {
        // Over the device memory budget (Switch 2 titles fill 8 GB cards): let the driver page
        // it out rather than failing the draw and aborting.
        static u32 logged{};
        if (logged++ < 32) {
            LOG_WARNING(Render_Vulkan,
                        "Image {}x{}x{} ({} levels, {} layers, format {}) is over the memory "
                        "budget; allocating it anyway",
                        ci.extent.width, ci.extent.height, ci.extent.depth, ci.mipLevels,
                        ci.arrayLayers, static_cast<u32>(ci.format));
        }
        pressure_events.fetch_add(1, std::memory_order_relaxed);
        const VmaAllocationCreateInfo relaxed = step_ci(steps[1]);
        result = vmaCreateImage(allocator, &ci, &relaxed, &handle, &allocation, &alloc_info);
    }
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && !huge) {
        // Still out of device memory: a real image in host memory (slow, but valid) beats one
        // with nothing bound, which AMD's drivers do not survive being sampled.
        const VmaAllocationCreateInfo host = step_ci(steps[2]);
        result = vmaCreateImage(allocator, &ci, &host, &handle, &allocation, &alloc_info);
        static u32 host_logged{};
        if (result == VK_SUCCESS && host_logged++ < 16) {
            // The heap as VMA sees it when even the driver refused the image: tells a full card
            // from memory VMA does not count.
            u64 usage = 0;
            u64 budget = 0;
            if (const u32 heap = MainDeviceLocalHeap(); heap != ~0U) {
                std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
                vmaGetHeapBudgets(allocator, budgets.data());
                usage = budgets[heap].usage;
                budget = budgets[heap].budget;
            }
            LOG_WARNING(Render_Vulkan,
                        "Image {}x{}x{} ({} levels, {} layers, format {}, {} MiB) is out of device "
                        "memory; placed in host memory; video memory {} of {} MiB",
                        ci.extent.width, ci.extent.height, ci.extent.depth, ci.mipLevels,
                        ci.arrayLayers, static_cast<u32>(ci.format), alloc_info.size >> 20,
                        usage >> 20, budget >> 20);
        }
    }
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && !huge) {
        pressure_events.fetch_add(1, std::memory_order_relaxed);
    }
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && ci.imageType != VK_IMAGE_TYPE_1D &&
        UsesUnbackedFallback(texels, may_fail)) {
        // Too big to back at all, e.g. title 1's sparse virtual texture arrays (8192x8192,
        // 68 layers, ~23 GB): on the console only resident tiles have memory. Use an image with
        // nothing bound, so views and copies still work, instead of aborting. A smaller image
        // gets one only for a caller that would throw otherwise.
        if (vk::Image sparse = CreateUnbackedImage(ci)) {
            LOG_WARNING(Render_Vulkan,
                        "Image {}x{}x{} ({} levels, {} layers, format {}) does not fit; using an "
                        "unbacked sparse image",
                        ci.extent.width, ci.extent.height, ci.extent.depth, ci.mipLevels,
                        ci.arrayLayers, static_cast<u32>(ci.format));
            image = std::move(sparse);
            return VK_SUCCESS;
        }
    }
    if (result != VK_SUCCESS) {
        failed_images.fetch_add(1, std::memory_order_relaxed);
        static u32 failed_logged{};
        if (failed_logged == 0) {
            // What ran out: the device's heaps or the host's commit. Before the error line,
            // which is the one that reaches the log file when the session ends here.
            LogMemoryTrace(device);
        }
        if (failed_logged++ < 32) {
            LOG_ERROR(Render_Vulkan, "Image {}x{}x{} ({} levels, {} layers, format {}) failed: {}",
                      ci.extent.width, ci.extent.height, ci.extent.depth, ci.mipLevels,
                      ci.arrayLayers, static_cast<u32>(ci.format), static_cast<int>(result));
        }
        return result;
    }
    VkMemoryPropertyFlags image_flags{};
    vmaGetAllocationMemoryProperties(allocator, allocation, &image_flags);
    if ((image_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0) {
        images_outside.fetch_add(1, std::memory_order_relaxed);
        image_bytes_outside.fetch_add(alloc_info.size, std::memory_order_relaxed);
    }

    // Log GPU memory allocation for images
    if (GPU::Logging::IsActive() &&
        Settings::values.gpu_log_memory_tracking.GetValue()) {
        GPU::Logging::GPULogger::GetInstance().LogMemoryAllocation(
            reinterpret_cast<uintptr_t>(alloc_info.deviceMemory),
            static_cast<u64>(alloc_info.size),
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        );
    }

    image = vk::Image(handle, ci.usage, *device.GetLogical(), allocator, allocation,
                      device.GetDispatchLoader());
    return VK_SUCCESS;
}

u64 MemoryAllocator::ImageBytes(const VkImageCreateInfo &ci) const {
    if (device_image_requirements != nullptr) {
        const VkDeviceImageMemoryRequirements info{
            .sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
            .pNext = nullptr,
            .pCreateInfo = &ci,
            .planeAspect = VkImageAspectFlagBits{},
        };
        VkMemoryRequirements2 requirements{
            .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
            .pNext = nullptr,
            .memoryRequirements = {},
        };
        device_image_requirements(*device.GetLogical(), &info, &requirements);
        return requirements.memoryRequirements.size;
    }
    const auto &dld = device.GetDispatchLoader();
    VkImage probe{};
    if (dld.vkCreateImage(*device.GetLogical(), &ci, nullptr, &probe) != VK_SUCCESS) {
        return 0;
    }
    const VkMemoryRequirements requirements = device.GetLogical().GetImageMemoryRequirements(probe);
    dld.vkDestroyImage(*device.GetLogical(), probe, nullptr);
    return requirements.size;
}

vk::Image MemoryAllocator::CreateUnbackedImage(const VkImageCreateInfo &ci, bool aliased) const {
    if (ci.imageType == VK_IMAGE_TYPE_1D) {
        return vk::Image{};
    }
    VkImageCreateInfo sparse_ci = ci;
    sparse_ci.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
    if (aliased) {
        sparse_ci.flags |= VK_IMAGE_CREATE_SPARSE_ALIASED_BIT;
    }
    VkImage sparse{};
    if (device.GetDispatchLoader().vkCreateImage(*device.GetLogical(), &sparse_ci, nullptr,
                                                 &sparse) != VK_SUCCESS) {
        return vk::Image{};
    }
    return vk::Image(sparse, ci.usage, *device.GetLogical(), allocator, VK_NULL_HANDLE,
                     device.GetDispatchLoader());
}

VmaAllocation MemoryAllocator::AllocateSparseMemory(const VkMemoryRequirements &requirements,
                                                   VkDeviceMemory &memory,
                                                   VkDeviceSize &offset) const {
    const VmaAllocationCreateInfo alloc_ci{
        .flags = 0,
        .usage = VMA_MEMORY_USAGE_UNKNOWN,
        .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        .preferredFlags = 0,
        .memoryTypeBits = requirements.memoryTypeBits,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
        .priority = 0.f,
    };
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    // In a pool of their own (see Device::SeparatePool); anything over half a block, or a pool
    // allocation that fails, takes the default blocks.
    u32 memory_type{};
    if (requirements.size <= Device::SeparatePoolBlockSize / 2 &&
        vmaFindMemoryTypeIndex(allocator, requirements.memoryTypeBits, &alloc_ci, &memory_type) ==
            VK_SUCCESS) {
        if (const VmaPool pool =
                device.GetSeparatePool(Device::SeparatePool::SparsePages, memory_type)) {
            VmaAllocationCreateInfo pool_ci = alloc_ci;
            pool_ci.pool = pool;
            if (vmaAllocateMemory(allocator, &requirements, &pool_ci, &allocation, &info) ==
                VK_SUCCESS) {
                memory = info.deviceMemory;
                offset = info.offset;
                return allocation;
            }
        }
    }
    if (vmaAllocateMemory(allocator, &requirements, &alloc_ci, &allocation, &info) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    memory = info.deviceMemory;
    offset = info.offset;
    return allocation;
}

void MemoryAllocator::FreeSparseMemory(VmaAllocation allocation) const {
    if (allocation != VK_NULL_HANDLE) {
        vmaFreeMemory(allocator, allocation);
    }
}

bool MemoryAllocator::CreateBufferInSeparatePool(const VkBufferCreateInfo& ci, MemoryUsage usage,
                                                 const VmaAllocationCreateInfo& alloc_ci,
                                                 VkBuffer& handle, VmaAllocation& allocation,
                                                 VmaAllocationInfo& alloc_info) const {
    // The buffer cache's buffers, churned by its collector: in a pool of their own (see
    // Device::SeparatePool). Anything over half a block takes the default blocks (dedicated).
    if (usage != MemoryUsage::DeviceLocal || ci.size > Device::SeparatePoolBlockSize / 2) {
        return false;
    }
    u32 memory_type{};
    {
        std::scoped_lock lk{buffer_type_mutex};
        const auto key = (u64{ci.usage} << 32) | ci.flags;
        if (const auto it = buffer_memory_types.find(key); it != buffer_memory_types.end()) {
            memory_type = it->second;
        } else if (vmaFindMemoryTypeIndexForBufferInfo(allocator, &ci, &alloc_ci, &memory_type) ==
                   VK_SUCCESS) {
            buffer_memory_types.emplace(key, memory_type);
        } else {
            return false;
        }
    }
    const VmaPool pool = device.GetSeparatePool(Device::SeparatePool::DeviceBuffers, memory_type);
    if (pool == VK_NULL_HANDLE) {
        return false;
    }
    VmaAllocationCreateInfo pool_ci = alloc_ci;
    pool_ci.pool = pool;
    pool_ci.memoryTypeBits = 0;
    return vmaCreateBuffer(allocator, &ci, &pool_ci, &handle, &allocation, &alloc_info) ==
           VK_SUCCESS;
}

vk::Buffer MemoryAllocator::CreateBuffer(const VkBufferCreateInfo &ci, MemoryUsage usage) const {
    // MESA will do memcpy() if not marked as host cached, so just force mark it for most buffers
    auto const anv_flags = (usage == MemoryUsage::Stream
        && device.GetDriverID() == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA)
        ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0;
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | MemoryUsageVmaFlags(usage),
        .usage = MemoryUsageVma(usage),
        .requiredFlags = 0,
        .preferredFlags = MemoryUsagePreferredVmaFlags(usage) | anv_flags,
        .memoryTypeBits = usage == MemoryUsage::Stream ? 0u : valid_memory_types,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
        .priority = 0.f,
    };

    VkBuffer handle{};
    VmaAllocationInfo alloc_info{};
    VmaAllocation allocation{};
    VkMemoryPropertyFlags property_flags{};

    if (!CreateBufferInSeparatePool(ci, usage, alloc_ci, handle, allocation, alloc_info)) {
        VkResult result =
            vmaCreateBuffer(allocator, &ci, &alloc_ci, &handle, &allocation, &alloc_info);
        if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            // Over the device memory budget, as images are in CreateImage: buffers have no
            // unbacked fallback, and the vk::Exception a failed staging or buffer cache
            // allocation throws from the GPU thread aborts the process. Phones share RAM with
            // the GPU, so the budget is often lower than the memory that is really free.
            static u32 logged{};
            if (logged++ < 32) {
                LOG_WARNING(Render_Vulkan,
                            "Buffer of {} bytes (usage {}) is over the memory budget; "
                            "allocating it anyway",
                            ci.size, static_cast<u32>(usage));
            }
            // The texture cache answers each pressure event by keeping more device memory free.
            // Upload and download buffers live in host memory on a discrete GPU, so they only
            // count when the heaps are shared or the buffer asked for device-local memory.
            if (device.IsIntegrated() || usage == MemoryUsage::DeviceLocal ||
                usage == MemoryUsage::Stream) {
                pressure_events.fetch_add(1, std::memory_order_relaxed);
            }
            VmaAllocationCreateInfo relaxed = alloc_ci;
            relaxed.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
            result = vmaCreateBuffer(allocator, &ci, &relaxed, &handle, &allocation, &alloc_info);
        }
        vk::Check(result);
    }
    vmaGetAllocationMemoryProperties(allocator, allocation, &property_flags);
    NoteDeviceLocalBuffer(usage, property_flags, alloc_info.size);

    // Log GPU memory allocation for buffers
    if (GPU::Logging::IsActive() &&
        Settings::values.gpu_log_memory_tracking.GetValue()) {
        GPU::Logging::GPULogger::GetInstance().LogMemoryAllocation(
            reinterpret_cast<uintptr_t>(alloc_info.deviceMemory),
            static_cast<u64>(alloc_info.size),
            property_flags
        );
    }

    u8 *data = reinterpret_cast<u8 *>(alloc_info.pMappedData);
    const std::span<u8> mapped_data = data ? std::span<u8>{data, ci.size} : std::span<u8>{};
    const bool is_coherent = (property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

    const vk::MemoryLocation location{
        .memory = alloc_info.deviceMemory,
        .offset = alloc_info.offset,
        .memory_type = alloc_info.memoryType,
    };
    return vk::Buffer(handle, *device.GetLogical(), allocator, allocation, mapped_data, is_coherent,
                      location, device.GetDispatchLoader());
}

vk::Buffer MemoryAllocator::CreateBuffer(const VkBufferCreateInfo &ci, MemoryUsage usage,
                                         VkDeviceSize min_alignment) const {
    if (min_alignment <= 1) {
        return CreateBuffer(ci, usage);
    }
    VkMemoryPropertyFlags anv_flags = 0;
    if (usage == MemoryUsage::Stream &&
        device.GetDriverID() == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA) {
        anv_flags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    }
    u32 memory_type_bits = valid_memory_types;
    if (usage == MemoryUsage::Stream) {
        memory_type_bits = 0u;
    }
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | MemoryUsageVmaFlags(usage),
        .usage = MemoryUsageVma(usage),
        .requiredFlags = 0,
        .preferredFlags = MemoryUsagePreferredVmaFlags(usage) | anv_flags,
        .memoryTypeBits = memory_type_bits,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
        .priority = 0.f,
    };

    VkBuffer handle{};
    VmaAllocationInfo alloc_info{};
    VmaAllocation allocation{};
    VkMemoryPropertyFlags property_flags{};

    vk::Check(vmaCreateBufferWithAlignment(allocator, &ci, &alloc_ci, min_alignment, &handle,
                                           &allocation, &alloc_info));
    vmaGetAllocationMemoryProperties(allocator, allocation, &property_flags);
    NoteDeviceLocalBuffer(usage, property_flags, alloc_info.size);

    u8 *data = reinterpret_cast<u8 *>(alloc_info.pMappedData);
    std::span<u8> mapped_data{};
    if (data) {
        mapped_data = std::span<u8>{data, ci.size};
    }
    const bool is_coherent = (property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

    const vk::MemoryLocation location{
        .memory = alloc_info.deviceMemory,
        .offset = alloc_info.offset,
        .memory_type = alloc_info.memoryType,
    };
    return vk::Buffer(handle, *device.GetLogical(), allocator, allocation, mapped_data, is_coherent,
                      location, device.GetDispatchLoader());
}

MemoryCommit MemoryAllocator::Commit(const VkMemoryRequirements &reqs, MemoryUsage usage)
{
    const auto vma_usage = MemoryUsageVma(usage);
    VmaAllocationCreateInfo ci{};
    ci.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | MemoryUsageVmaFlags(usage);
    ci.usage = vma_usage;
    ci.memoryTypeBits = reqs.memoryTypeBits & valid_memory_types;
    ci.requiredFlags = 0;
    ci.preferredFlags = MemoryUsagePreferredVmaFlags(usage);

    VmaAllocation a{};
    VmaAllocationInfo info{};

    VkResult res = vmaAllocateMemory(allocator, &reqs, &ci, &a, &info);

    if (res != VK_SUCCESS) {
        // Relax 1: drop budget constraint
        auto ci2 = ci;
        ci2.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        res = vmaAllocateMemory(allocator, &reqs, &ci2, &a, &info);

        // Relax 2: if we preferred DEVICE_LOCAL, drop that preference
        if (res != VK_SUCCESS && (ci.preferredFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            auto ci3 = ci2;
            ci3.preferredFlags &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            res = vmaAllocateMemory(allocator, &reqs, &ci3, &a, &info);
        }
    }

    vk::Check(res);
    return MemoryCommit(allocator, a, info);
}

MemoryCommit MemoryAllocator::Commit(const vk::Buffer &buffer, MemoryUsage usage) {
    // Allocate memory appropriate for this buffer automatically
    const auto vma_usage = MemoryUsageVma(usage);

    VmaAllocationCreateInfo ci{};
    ci.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | MemoryUsageVmaFlags(usage);
    ci.usage = vma_usage;
    ci.requiredFlags = 0;
    ci.preferredFlags = MemoryUsagePreferredVmaFlags(usage);
    ci.pool = VK_NULL_HANDLE;
    ci.pUserData = nullptr;
    ci.priority = 0.0f;

    const VkBuffer raw = *buffer;

    VmaAllocation a{};
    VmaAllocationInfo info{};

    // Let VMA infer memory requirements from the buffer
    VkResult res = vmaAllocateMemoryForBuffer(allocator, raw, &ci, &a, &info);

    if (res != VK_SUCCESS) {
        auto ci2 = ci;
        ci2.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        res = vmaAllocateMemoryForBuffer(allocator, raw, &ci2, &a, &info);

        if (res != VK_SUCCESS && (ci.preferredFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            auto ci3 = ci2;
            ci3.preferredFlags &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            res = vmaAllocateMemoryForBuffer(allocator, raw, &ci3, &a, &info);
        }
    }

    vk::Check(res);
    vk::Check(vmaBindBufferMemory2(allocator, a, 0, raw, nullptr));
    return MemoryCommit(allocator, a, info);
}

#ifdef _WIN32
namespace {
/// The DXGI adapter of the Vulkan device (matched by LUID), or null. Looked up again when the
/// device changes (another GPU picked between sessions).
IDXGIAdapter3* FindDxgiAdapter(const Device& device) {
    VkPhysicalDeviceIDProperties id{};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &id;
    device.GetPhysical().GetProperties2(properties);
    if (!id.deviceLUIDValid) {
        return nullptr;
    }
    LUID luid{};
    std::memcpy(&luid, id.deviceLUID, sizeof(luid));
    static LUID cached_luid{};
    static IDXGIAdapter3* cached{};
    if (cached != nullptr && cached_luid.LowPart == luid.LowPart &&
        cached_luid.HighPart == luid.HighPart) {
        return cached;
    }
    if (cached != nullptr) {
        cached->Release();
        cached = nullptr;
    }
    cached_luid = luid;
    const HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (dxgi == nullptr) {
        return nullptr;
    }
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    const auto create =
        reinterpret_cast<CreateFactory>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
    IDXGIFactory4* factory{};
    if (create == nullptr ||
        FAILED(create(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory)))) {
        return nullptr;
    }
    if (FAILED(factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter3),
                                          reinterpret_cast<void**>(&cached)))) {
        cached = nullptr;
    }
    factory->Release();
    return cached;
}
} // Anonymous namespace
#endif

void LogMemoryTrace(const Device& device) {
    const Common::ProcessMemoryUsage process = Common::GetProcessMemoryUsage();
    LOG_INFO(Render_Vulkan,
             "Memory trace: process commit {} MiB, working set {} MiB (private {} MiB), system "
             "commit {} of {} MiB",
             process.commit >> 20, process.working_set >> 20, process.private_working_set >> 20,
             process.system_commit >> 20, process.system_commit_limit >> 20);
#ifdef _WIN32
    // This process's use of the card's memory (local) and of system memory for the GPU
    // (non-local: host memory types, and what the OS moved out of video memory).
    IDXGIAdapter3* const adapter = FindDxgiAdapter(device);
    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    DXGI_QUERY_VIDEO_MEMORY_INFO non_local{};
    if (adapter != nullptr &&
        SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
        SUCCEEDED(
            adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &non_local))) {
        LOG_INFO(Render_Vulkan,
                 "Memory trace: DXGI local {} of {} MiB, non-local {} of {} MiB",
                 local.CurrentUsage >> 20, local.Budget >> 20, non_local.CurrentUsage >> 20,
                 non_local.Budget >> 20);
    }
#endif
    const VmaAllocator allocator = device.GetAllocator();
    const VkPhysicalDeviceMemoryProperties* properties{};
    vmaGetMemoryProperties(allocator, &properties);
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(allocator, budgets.data());
    std::string heaps;
    for (u32 heap = 0; heap < properties->memoryHeapCount; ++heap) {
        const VmaBudget& budget = budgets[heap];
        fmt::format_to(std::back_inserter(heaps), "{}{} ({}): driver {} of {} MiB, VMA {}/{} MiB",
                       heap == 0 ? "" : "; ", heap,
                       (properties->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0
                           ? "local"
                           : "host",
                       budget.usage >> 20, budget.budget >> 20,
                       budget.statistics.allocationBytes >> 20,
                       budget.statistics.blockBytes >> 20);
    }
    LOG_INFO(Render_Vulkan, "Memory trace: heaps {}", heaps);
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(allocator, &stats);
    std::string types;
    for (u32 type = 0; type < properties->memoryTypeCount; ++type) {
        const VmaStatistics& s = stats.memoryType[type].statistics;
        if (s.blockCount == 0) {
            continue;
        }
        fmt::format_to(std::back_inserter(types), "{}{} (heap {}, flags {:#x}): {} allocs {}/{} MiB",
                       types.empty() ? "" : "; ", type, properties->memoryTypes[type].heapIndex,
                       static_cast<u32>(properties->memoryTypes[type].propertyFlags),
                       s.allocationCount, s.allocationBytes >> 20, s.blockBytes >> 20);
    }
    LOG_INFO(Render_Vulkan, "Memory trace: types {}", types);
}

void MemoryAllocator::NoteDeviceLocalBuffer(MemoryUsage usage, VkMemoryPropertyFlags flags,
                                            VkDeviceSize size) const {
    if (usage == MemoryUsage::DeviceLocal && (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0) {
        buffers_outside.fetch_add(1, std::memory_order_relaxed);
        buffer_bytes_outside.fetch_add(size, std::memory_order_relaxed);
    }
}

bool MemoryAllocator::HasHostMemoryApartFromVideo() const noexcept {
    std::array<u32, VK_MAX_MEMORY_TYPES> type_flags{};
    for (u32 type = 0; type < properties.memoryTypeCount; ++type) {
        type_flags[type] = properties.memoryTypes[type].propertyFlags;
    }
    return Vulkan::HasHostMemoryApartFromVideo(type_flags.data(), properties.memoryTypeCount);
}

u32 MemoryAllocator::MainDeviceLocalHeap() const noexcept {
    u32 main = ~0U;
    for (u32 heap = 0; heap < properties.memoryHeapCount; ++heap) {
        if ((properties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0 &&
            (main == ~0U || properties.memoryHeaps[heap].size > properties.memoryHeaps[main].size)) {
            main = heap;
        }
    }
    return main;
}

void MemoryAllocator::LogOutsideVideoMemory() const {
    const u64 images = images_outside.load(std::memory_order_relaxed);
    const u64 image_bytes = image_bytes_outside.load(std::memory_order_relaxed);
    const u64 buffers = buffers_outside.load(std::memory_order_relaxed);
    const u64 buffer_bytes = buffer_bytes_outside.load(std::memory_order_relaxed);
    // What lives now in memory the GPU sees only over the bus and the CPU cannot map: there go
    // the images and device-local buffers VMA moved out of video memory (staging buffers are
    // host-visible and are not counted).
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(allocator, &stats);
    u64 live_bytes{};
    u32 live_allocations{};
    constexpr VkMemoryPropertyFlags device_or_mappable =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    for (u32 type = 0; type < properties.memoryTypeCount; ++type) {
        if ((properties.memoryTypes[type].propertyFlags & device_or_mappable) == 0) {
            live_bytes += stats.memoryType[type].statistics.allocationBytes;
            live_allocations += stats.memoryType[type].statistics.allocationCount;
        }
    }
    const u64 yielded = textures_yielded.load(std::memory_order_relaxed);
    const u64 yielded_bytes = texture_bytes_yielded.load(std::memory_order_relaxed);
    const std::array<u64, 7> now{images, image_bytes >> 20, buffers, buffer_bytes >> 20,
                                 live_bytes >> 20, yielded, yielded_bytes >> 20};
    if (outside_logged && now == last_outside) {
        return;
    }
    outside_logged = true;
    last_outside = now;
    u64 reserve = 0;
    if (const u32 heap = MainDeviceLocalHeap(); heap != ~0U) {
        std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
        vmaGetHeapBudgets(allocator, budgets.data());
        reserve = RenderTargetReserveBytes(budgets[heap].budget);
    }
    LOG_INFO(Render_Vulkan,
             "Outside video memory since start: {} images ({} MiB), {} device-local buffers ({} "
             "MiB); now {} allocations ({} MiB) in system memory the CPU cannot map; of those "
             "images, {} textures ({} MiB) left the last {} MiB to render targets",
             images, image_bytes >> 20, buffers, buffer_bytes >> 20, live_allocations,
             live_bytes >> 20, yielded, yielded_bytes >> 20, reserve >> 20);
}

void LogMemoryStatistics(const Device& device) {
    const VmaAllocator allocator = device.GetAllocator();
    const VkPhysicalDeviceMemoryProperties* properties{};
    vmaGetMemoryProperties(allocator, &properties);
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(allocator, &stats);
    for (u32 type = 0; type < properties->memoryTypeCount; ++type) {
        const VmaStatistics& s = stats.memoryType[type].statistics;
        if (s.blockCount == 0) {
            continue;
        }
        LOG_DEBUG(Render_Vulkan, "VMA type {} (heap {}, flags {:#x}): {} blocks {} MiB, {} allocations {} MiB",
                 type, properties->memoryTypes[type].heapIndex,
                 static_cast<u32>(properties->memoryTypes[type].propertyFlags), s.blockCount,
                 s.blockBytes >> 20, s.allocationCount, s.allocationBytes >> 20);
    }
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(allocator, budgets.data());
    for (u32 heap = 0; heap < properties->memoryHeapCount; ++heap) {
        LOG_INFO(Render_Vulkan, "VMA heap {}: usage {} MiB of budget {} MiB, VMA blocks {} MiB",
                 heap, budgets[heap].usage >> 20, budgets[heap].budget >> 20,
                 budgets[heap].statistics.blockBytes >> 20);
    }
}

} // namespace Vulkan
