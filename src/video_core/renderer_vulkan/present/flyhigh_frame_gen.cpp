// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/frame_gen/flyhigh_library.h"
#include "video_core/renderer_vulkan/present/flyhigh_frame_gen.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

namespace {

#ifdef _WIN32
constexpr VkExternalMemoryHandleTypeFlagBits MEMORY_HANDLE =
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
constexpr VkExternalSemaphoreHandleTypeFlagBits SEMAPHORE_HANDLE =
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
constexpr VkExternalMemoryHandleTypeFlagBits MEMORY_HANDLE =
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
constexpr VkExternalSemaphoreHandleTypeFlagBits SEMAPHORE_HANDLE =
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

/// A handle passed to FlyHigh: a Windows handle stays ours after the import, a file descriptor
/// becomes FlyHigh's once the import succeeds.
void ReleaseHandle(intptr_t handle, bool imported) {
#ifdef _WIN32
    (void)imported;
    CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
    if (!imported) {
        close(static_cast<int>(handle));
    }
#endif
}

[[noreturn]] void Fail(const std::string& what) {
    throw std::runtime_error(what);
}

VkImageMemoryBarrier Transition(VkImage image, VkAccessFlags src_access, VkAccessFlags dst_access,
                                VkImageLayout old_layout, VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
}

VkImageBlit WholeBlit(VkExtent2D extent) {
    const VkImageSubresourceLayers layers{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    const VkOffset3D corner{static_cast<s32>(extent.width), static_cast<s32>(extent.height), 1};
    return VkImageBlit{
        .srcSubresource = layers,
        .srcOffsets = {{0, 0, 0}, corner},
        .dstSubresource = layers,
        .dstOffsets = {{0, 0, 0}, corner},
    };
}

VkBufferImageCopy RowsCopy(VkExtent2D extent, u32 row_pitch) {
    return VkBufferImageCopy{
        .bufferOffset = 0,
        .bufferRowLength = row_pitch / 4,
        .bufferImageHeight = extent.height,
        .imageSubresource{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .imageOffset = {},
        .imageExtent = {extent.width, extent.height, 1},
    };
}

bool IsSrgb(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB ||
           format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

} // Anonymous namespace

FlyHighFrameGen::FlyHighFrameGen(const Device& device_, MemoryAllocator& memory_allocator,
                                 VkExtent2D extent_, VkFormat format, bool protect_hud)
    : fh{[] -> const VideoCore::FrameGen::FlyHighLibrary& {
          const auto* library = VideoCore::FrameGen::LoadFlyHighLibrary();
          if (library == nullptr) {
              Fail("the FlyHigh library is missing or too old");
          }
          return *library;
      }()},
      device{device_}, extent{extent_} {
    try {
        Build(memory_allocator, format, protect_hud);
    } catch (...) {
        Release();
        throw;
    }
}

void FlyHighFrameGen::Build(MemoryAllocator& memory_allocator, VkFormat format, bool protect_hud) {
    if (!VideoCore::FrameGen::FlyHighModelInstalled()) {
        Fail(fmt::format("the model is not in {}", Common::FS::PathToUTF8String(
                                                        VideoCore::FrameGen::FlyHighModelDir())));
    }
    if (!device.HasExternalInterop() || !device.HasTimelineSemaphore()) {
        Fail("this device can't share memory and timeline semaphores with another Vulkan device");
    }
    if (format == VK_FORMAT_R16G16B16A16_SFLOAT) {
        Fail("HDR output is not supported");
    }

    VkPhysicalDeviceIDProperties id{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                           .pNext = &id};
    device.GetPhysical().GetProperties2(properties);

    // The same GPU as Lichii's: memory and semaphores are shared only within one GPU and driver.
    const std::string adapter =
        fmt::format("{:04x}:{:04x}", device.GetVendorID(), device.GetDeviceID());
    const std::string tune_dir = Common::FS::PathToUTF8String(VideoCore::FrameGen::FlyHighTuneDir());
    FhEngineOptions engine_options = fh.fh_engine_options_default();
    engine_options.backend = 1;
    engine_options.adapter = adapter.c_str();
    engine_options.tune_cache = tune_dir.c_str();
    engine = fh.fh_engine_create(&engine_options);
    if (engine == nullptr) {
        Fail(fmt::format("no FlyHigh engine on {}: {}", adapter, fh.LastError()));
    }
    std::array<u8, VK_UUID_SIZE> engine_uuid{};
    if (fh.fh_engine_vulkan_interop(engine, engine_uuid.data()) != 1) {
        Fail("FlyHigh's device can't import Vulkan memory and semaphores");
    }
    if (std::memcmp(engine_uuid.data(), id.deviceUUID, VK_UUID_SIZE) != 0) {
        Fail(fmt::format("FlyHigh picked another GPU than Lichii's ({})", adapter));
    }

    const std::string model_dir =
        Common::FS::PathToUTF8String(VideoCore::FrameGen::FlyHighModelDir());
    model = fh.fh_model_load(model_dir.c_str());
    if (model == nullptr) {
        Fail(fmt::format("the model did not load: {}", fh.LastError()));
    }

    FhSessionOptions session = fh.fh_session_options_default(extent.width, extent.height);
    session.keep_static = protect_hud ? 1 : 0;
    FhStreamOptions stream_options = fh.fh_stream_options_default();
    stream_options.inputs = 3;
    stream_options.outputs = static_cast<u32>(MAX_GENERATIONS);
    stream = fh.fh_stream_create(engine, model, &session, &stream_options);
    if (stream == nullptr) {
        Fail(fmt::format("no FlyHigh stream at {}x{}: {}", extent.width, extent.height,
                         fh.LastError()));
    }
    Check(fh.fh_stream_warm_up(stream, engine), "fh_stream_warm_up");

    row_pitch = fh.fh_vk_row_pitch(extent.width);
    buffer_size = static_cast<VkDeviceSize>(row_pitch) * extent.height;

    staging = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        // The network sees the bytes as they are encoded, the way it was trained.
        .format = IsSrgb(format) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    for (SharedBuffer& shared : inputs) {
        CreateSharedBuffer(shared);
    }
    for (SharedBuffer& shared : outputs) {
        CreateSharedBuffer(shared);
    }
    CreateSharedTimeline(frames_in);
    CreateSharedTimeline(frames_out);

    char info[512]{};
    if (fh.fh_engine_info(engine, info, sizeof(info)) > 0) {
        LOG_INFO(Render_Vulkan, "FlyHigh frame generation at {}x{}: {}", extent.width,
                 extent.height, info);
    }
}

FlyHighFrameGen::~FlyHighFrameGen() {
    Release();
}

void FlyHighFrameGen::Release() noexcept {
    if (stream != nullptr) {
        fh.fh_stream_destroy(stream);
        stream = nullptr;
    }
    for (SharedBuffer& shared : inputs) {
        fh.fh_vk_buffer_destroy(std::exchange(shared.imported, nullptr));
    }
    for (SharedBuffer& shared : outputs) {
        fh.fh_vk_buffer_destroy(std::exchange(shared.imported, nullptr));
    }
    // Each waits for FlyHigh's queue, so nothing of it still uses the shared objects below.
    fh.fh_vk_semaphore_destroy(std::exchange(frames_in.imported, nullptr));
    fh.fh_vk_semaphore_destroy(std::exchange(frames_out.imported, nullptr));
    if (model != nullptr) {
        fh.fh_model_destroy(std::exchange(model, nullptr));
    }
    if (engine != nullptr) {
        fh.fh_engine_destroy(std::exchange(engine, nullptr));
    }
    const auto& dld = device.GetDispatchLoader();
    for (SharedBuffer& shared : inputs) {
        if (shared.buffer != VK_NULL_HANDLE) {
            dld.vkDestroyBuffer(*device.GetLogical(), std::exchange(shared.buffer, VkBuffer{}),
                                nullptr);
        }
    }
    for (SharedBuffer& shared : outputs) {
        if (shared.buffer != VK_NULL_HANDLE) {
            dld.vkDestroyBuffer(*device.GetLogical(), std::exchange(shared.buffer, VkBuffer{}),
                                nullptr);
        }
    }
}

void FlyHighFrameGen::Check(int result, const char* call) const {
    if (result < 0) {
        Fail(fmt::format("{} failed: {}", call, fh.LastError()));
    }
}

void FlyHighFrameGen::CreateSharedBuffer(SharedBuffer& shared) {
    const auto& dld = device.GetDispatchLoader();
    const VkDevice logical = *device.GetLogical();
    const VkExternalMemoryBufferCreateInfo external{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .handleTypes = MEMORY_HANDLE,
    };
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &external,
        .flags = 0,
        .size = buffer_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    vk::Check(dld.vkCreateBuffer(logical, &buffer_ci, nullptr, &shared.buffer));

    const VkMemoryRequirements requirements =
        device.GetLogical().GetBufferMemoryRequirements(shared.buffer);
    const VkPhysicalDeviceMemoryProperties memory_types =
        device.GetPhysical().GetMemoryProperties().memoryProperties;
    u32 type_index = memory_types.memoryTypeCount;
    for (u32 i = 0; i < memory_types.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1U << i)) != 0 &&
            (memory_types.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
            type_index = i;
            break;
        }
    }
    if (type_index == memory_types.memoryTypeCount) {
        Fail("no device-local memory can be shared");
    }
    const VkMemoryDedicatedAllocateInfo dedicated{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = nullptr,
        .image = VK_NULL_HANDLE,
        .buffer = shared.buffer,
    };
    const VkExportMemoryAllocateInfo export_info{
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated,
        .handleTypes = MEMORY_HANDLE,
    };
    shared.memory = device.GetLogical().AllocateMemory(VkMemoryAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_info,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type_index,
    });
    vk::Check(dld.vkBindBufferMemory(logical, shared.buffer, *shared.memory, 0));

#ifdef _WIN32
    const intptr_t handle = reinterpret_cast<intptr_t>(shared.memory.GetMemoryWin32HandleKHR());
#else
    const intptr_t handle = shared.memory.GetMemoryFdKHR();
#endif
    const FhVkMemory memory{
        .handle = handle,
        .allocation_size = requirements.size,
        .memory_type_index = type_index,
    };
    shared.imported = fh.fh_vk_buffer_import(engine, &memory, buffer_size);
    ReleaseHandle(handle, shared.imported != nullptr);
    if (shared.imported == nullptr) {
        Fail(fmt::format("FlyHigh could not import shared memory: {}", fh.LastError()));
    }
}

void FlyHighFrameGen::CreateSharedTimeline(SharedTimeline& shared) {
    const VkExportSemaphoreCreateInfo export_info{
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .pNext = nullptr,
        .handleTypes = SEMAPHORE_HANDLE,
    };
    const VkSemaphoreTypeCreateInfo type{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = &export_info,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    shared.semaphore = device.GetLogical().CreateSemaphore(VkSemaphoreCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &type,
        .flags = 0,
    });

    const auto& dld = device.GetDispatchLoader();
    const VkDevice logical = *device.GetLogical();
#ifdef _WIN32
    const auto get_handle = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
        dld.vkGetDeviceProcAddr(logical, "vkGetSemaphoreWin32HandleKHR"));
    if (get_handle == nullptr) {
        Fail("vkGetSemaphoreWin32HandleKHR is missing");
    }
    const VkSemaphoreGetWin32HandleInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR,
        .pNext = nullptr,
        .semaphore = *shared.semaphore,
        .handleType = SEMAPHORE_HANDLE,
    };
    HANDLE win32_handle{};
    vk::Check(get_handle(logical, &info, &win32_handle));
    const intptr_t handle = reinterpret_cast<intptr_t>(win32_handle);
#else
    const auto get_fd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        dld.vkGetDeviceProcAddr(logical, "vkGetSemaphoreFdKHR"));
    if (get_fd == nullptr) {
        Fail("vkGetSemaphoreFdKHR is missing");
    }
    const VkSemaphoreGetFdInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .pNext = nullptr,
        .semaphore = *shared.semaphore,
        .handleType = SEMAPHORE_HANDLE,
    };
    int fd{};
    vk::Check(get_fd(logical, &info, &fd));
    const intptr_t handle = fd;
#endif
    shared.imported = fh.fh_vk_semaphore_import(engine, handle, 1);
    ReleaseHandle(handle, shared.imported != nullptr);
    if (shared.imported == nullptr) {
        Fail(fmt::format("FlyHigh could not import the semaphore: {}", fh.LastError()));
    }
}

size_t FlyHighFrameGen::Process(Scheduler& scheduler, VkImage source, size_t generations) {
    const SharedBuffer& input = inputs[frame_count % inputs.size()];
    ++frame_count;

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([source, staging_image = *staging, buffer = input.buffer, extent = extent,
                      row_pitch = row_pitch](vk::CommandBuffer cmdbuf) {
        const std::array before{
            Transition(source, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
            Transition(staging_image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, before);
        cmdbuf.BlitImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging_image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, WholeBlit(extent),
                         VK_FILTER_NEAREST);
        const std::array after{
            Transition(source, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Transition(staging_image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               0, {}, {}, after);
        cmdbuf.CopyImageToBuffer(staging_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer,
                                 RowsCopy(extent, row_pitch));
    });
    const u64 frame_ready = ++frames_in.value;
    scheduler.Flush(SemaphoreValue{*frames_in.semaphore, frame_ready}, SemaphoreValue{});

    const int slot = fh.fh_stream_next_input(stream, nullptr, 0);
    Check(slot, "fh_stream_next_input");
    Check(fh.fh_stream_copy_in(stream, engine, static_cast<u32>(slot), input.imported, row_pitch,
                               frames_in.imported, frame_ready),
          "fh_stream_copy_in");
    const double pts =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    FhPush push{};
    Check(fh.fh_stream_push(stream, engine, static_cast<u32>(slot), pts, &push), "fh_stream_push");

    generations = std::min(generations, MAX_GENERATIONS);
    if (push.ready == 0 || generations == 0) {
        return 0;
    }
    for (size_t generation = 0; generation < generations; ++generation) {
        const float t =
            static_cast<float>(generation + 1) / static_cast<float>(generations + 1);
        Check(fh.fh_stream_generate(stream, engine, t, static_cast<u32>(generation)),
              "fh_stream_generate");
        const bool last = generation + 1 == generations;
        const u64 written = last ? ++frames_out.value : 0;
        Check(fh.fh_stream_copy_out(stream, engine, static_cast<u32>(generation),
                                    outputs[generation].imported, row_pitch,
                                    last ? frames_out.imported : nullptr, written),
              "fh_stream_copy_out");
        if (last) {
            pending_wait = written;
        }
    }
    return generations;
}

void FlyHighFrameGen::Generate(Scheduler& scheduler, VkImage destination, size_t generation) {
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([destination, staging_image = *staging,
                      buffer = outputs[generation].buffer, extent = extent,
                      row_pitch = row_pitch](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            Transition(staging_image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
        cmdbuf.CopyBufferToImage(buffer, staging_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 RowsCopy(extent, row_pitch));
        const std::array before{
            Transition(staging_image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
            Transition(destination, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, before);
        cmdbuf.BlitImage(staging_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, WholeBlit(extent),
                         VK_FILTER_NEAREST);
        cmdbuf.PipelineBarrier(
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
            Transition(destination, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL));
    });
}

SemaphoreValue FlyHighFrameGen::TakeWait() {
    const u64 value = std::exchange(pending_wait, 0);
    if (value == 0) {
        return {};
    }
    return SemaphoreValue{*frames_out.semaphore, value};
}

} // namespace Vulkan
