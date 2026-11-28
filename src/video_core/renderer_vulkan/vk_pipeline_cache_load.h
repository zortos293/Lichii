// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan.h"

namespace Vulkan {

/// Who driver pipeline cache data says it was made by: Vulkan starts it with a header naming the
/// vendor, device and pipelineCacheUUID (MoltenVK's changes with each build of it). Whether the
/// driver kept a file is asked of the driver; the header only confirms a drop, and says why.
enum class PipelineCacheDataOrigin { ThisDriver, OtherDriver, NoData, Unknown };

/// NoData for data shorter than a version one header (a file of Lichii's header alone among
/// them), Unknown for a header of another version.
[[nodiscard]] inline PipelineCacheDataOrigin PipelineCacheDataMadeBy(
    std::span<const char> data, u32 vendor_id, u32 device_id,
    std::span<const u8, VK_UUID_SIZE> uuid) {
    // The header's fields are little endian whatever the host.
    static_assert(std::endian::native == std::endian::little);
    VkPipelineCacheHeaderVersionOne header;
    if (data.size() < sizeof(header)) {
        return PipelineCacheDataOrigin::NoData;
    }
    std::memcpy(&header, data.data(), sizeof(header));
    if (header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
        header.headerSize < sizeof(header)) {
        return PipelineCacheDataOrigin::Unknown;
    }
    const bool same = header.vendorID == vendor_id && header.deviceID == device_id &&
                      std::ranges::equal(header.pipelineCacheUUID, uuid);
    return same ? PipelineCacheDataOrigin::ThisDriver : PipelineCacheDataOrigin::OtherDriver;
}

/// What loading the driver pipeline cache file came to.
enum class VulkanCacheLoad {
    /// No file.
    NoFile,
    /// The driver holds the file's data.
    Restored,
    /// Cut short, not a Lichii cache file, or unreadable.
    Damaged,
    /// A file of another Lichii cache version.
    OtherVersion,
    /// The driver refused the data with an error (vkCreatePipelineCache failed).
    Rejected,
    /// Driver data shorter than a version one header: what clearing the cache writes on purpose
    /// (ClearPipelineCache, so an older version's cache is not carried over again), or what a
    /// save that read nothing left.
    NoData,
    /// The driver was given more than an empty cache of its own and holds none of it, and the
    /// header does not say the data is its own: made by another driver version (MoltenVK's
    /// pipelineCacheUUID changes with each build of it) or device, or of an unknown header
    /// version the driver dropped. vkCreatePipelineCache succeeds either way.
    DriverKeptNothing,
};

/// For data the driver took: `data_size` bytes of the file were given to it, after which it
/// reports holding `held_after_create`; an empty cache it creates reports `empty_size` (each
/// empty when not asked, or when the query failed: the data is then taken as kept, as it always
/// was). `origin` is what the header says. Data the header says is this driver's is restored
/// without asking. Otherwise two signals drop it: the driver holds no more than an empty cache
/// although it was given more (a file holding an empty cache of this driver, as a boot save
/// writes where no pipeline went through the cache, is kept), and the header does not say the
/// data is this driver's. The header never turns a kept cache into a dropped one.
[[nodiscard]] inline VulkanCacheLoad DecideVulkanCacheLoad(size_t data_size,
                                                           std::optional<size_t> held_after_create,
                                                           std::optional<size_t> empty_size,
                                                           PipelineCacheDataOrigin origin) {
    if (data_size < sizeof(VkPipelineCacheHeaderVersionOne)) {
        return VulkanCacheLoad::NoData;
    }
    if (origin == PipelineCacheDataOrigin::ThisDriver || !held_after_create || !empty_size ||
        *held_after_create > *empty_size || data_size <= *empty_size) {
        return VulkanCacheLoad::Restored;
    }
    return VulkanCacheLoad::DriverKeptNothing;
}

/// What the load does about it.
struct VulkanCacheLoadActions {
    /// Whether the cache counts as restored (vulkan_pipeline_cache_restored). One that is not
    /// drops the precache's progress, whose builds it no longer holds.
    bool restored;
    /// Whether the precache's "done" marker goes too, so the precache runs again and refills the
    /// cache. Only where the driver was given the data and kept none of it, or the file held no
    /// data: the marker's stamp (driver version, device) does not change with the
    /// pipelineCacheUUID, so after a MoltenVK update it still matched and the precache built
    /// nothing into the emptied cache. Not where the driver rejected the file with an error: a
    /// driver that refuses its own cache refuses it again (MoltenVK fails a whole cache holding
    /// MSL it cannot compile, which the precache builds), so a rerun there would poison the cache
    /// at every boot instead of converging on one without those builds. Nor where there was no
    /// file, a damaged one or one of another Lichii cache version, as on nx2.
    bool drop_precache_marker;
    /// Whether the file goes (after the marker): so it is not read again at every boot until a
    /// save replaces it.
    bool remove_file;
};

[[nodiscard]] constexpr VulkanCacheLoadActions ActionsFor(VulkanCacheLoad load) {
    switch (load) {
    case VulkanCacheLoad::Restored:
        return {.restored = true, .drop_precache_marker = false, .remove_file = false};
    case VulkanCacheLoad::NoFile:
        return {.restored = false, .drop_precache_marker = false, .remove_file = false};
    case VulkanCacheLoad::Damaged:
    case VulkanCacheLoad::OtherVersion:
    case VulkanCacheLoad::Rejected:
        return {.restored = false, .drop_precache_marker = false, .remove_file = true};
    case VulkanCacheLoad::NoData:
        // The file stays until a save replaces it: removed, no file of this version would be
        // there until the boot save, and a boot that died before it would let the cleared older
        // version's cache be carried over again.
        return {.restored = false, .drop_precache_marker = true, .remove_file = false};
    case VulkanCacheLoad::DriverKeptNothing:
        return {.restored = false, .drop_precache_marker = true, .remove_file = true};
    }
    return {.restored = false, .drop_precache_marker = false, .remove_file = false};
}

} // namespace Vulkan
