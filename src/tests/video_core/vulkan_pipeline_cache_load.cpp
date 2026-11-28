// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/vk_pipeline_cache_load.h"

namespace {

using Vulkan::PipelineCacheDataOrigin;
using Vulkan::VulkanCacheLoad;

constexpr size_t Header = sizeof(VkPipelineCacheHeaderVersionOne);

constexpr u32 Vendor = 0x106B;
constexpr u32 DeviceId = 0x1A020209;
constexpr std::array<u8, VK_UUID_SIZE> Uuid{0x38, 0x71, 0x93, 0x85, 1, 2,  3,  4,
                                            5,    6,    7,    8,    9, 10, 11, 12};

// Driver data as vkGetPipelineCacheData returns it: the version one header, then the driver's own.
std::vector<char> Data(u32 vendor, u32 device, const std::array<u8, VK_UUID_SIZE>& uuid) {
    const VkPipelineCacheHeaderVersionOne header{
        .headerSize = sizeof(VkPipelineCacheHeaderVersionOne),
        .headerVersion = VK_PIPELINE_CACHE_HEADER_VERSION_ONE,
        .vendorID = vendor,
        .deviceID = device,
    };
    std::vector<char> data(sizeof(header) + 16, 0x5A);
    std::memcpy(data.data(), &header, sizeof(header));
    std::memcpy(data.data() + offsetof(VkPipelineCacheHeaderVersionOne, pipelineCacheUUID),
                uuid.data(), uuid.size());
    return data;
}

PipelineCacheDataOrigin Origin(const std::vector<char>& data) {
    return Vulkan::PipelineCacheDataMadeBy(data, Vendor, DeviceId, Uuid);
}

} // Anonymous namespace

TEST_CASE("Driver pipeline cache data from this driver is this driver's",
          "[video_core][vulkan_pipeline_cache]") {
    CHECK(Origin(Data(Vendor, DeviceId, Uuid)) == PipelineCacheDataOrigin::ThisDriver);
}

TEST_CASE("Driver pipeline cache data from another driver build is another driver's",
          "[video_core][vulkan_pipeline_cache]") {
    // Another MoltenVK build: the same vendor and device, its UUID's first bytes changed.
    auto other_build = Uuid;
    other_build[0] = 0xBF;
    CHECK(Origin(Data(Vendor, DeviceId, other_build)) == PipelineCacheDataOrigin::OtherDriver);
    CHECK(Origin(Data(Vendor, DeviceId + 1, Uuid)) == PipelineCacheDataOrigin::OtherDriver);
    CHECK(Origin(Data(0x10DE, DeviceId, Uuid)) == PipelineCacheDataOrigin::OtherDriver);
}

TEST_CASE("Driver pipeline cache data shorter than its header holds nothing",
          "[video_core][vulkan_pipeline_cache]") {
    // A file of Lichii's header alone, as a save that read nothing from the driver left it.
    CHECK(Origin({}) == PipelineCacheDataOrigin::NoData);
    const auto data = Data(Vendor, DeviceId, Uuid);
    CHECK(Origin(std::vector<char>(data.begin(), data.begin() + 31)) ==
          PipelineCacheDataOrigin::NoData);
    CHECK(Origin(std::vector<char>(data.begin(), data.begin() + 32)) ==
          PipelineCacheDataOrigin::ThisDriver);
}

TEST_CASE("Driver pipeline cache data with a header of another version is left to the driver",
          "[video_core][vulkan_pipeline_cache]") {
    auto data = Data(Vendor, DeviceId, Uuid);
    auto other_version = data;
    const u32 version = 2;
    std::memcpy(other_version.data() + 4, &version, sizeof(version));
    CHECK(Origin(other_version) == PipelineCacheDataOrigin::Unknown);
    auto short_header = data;
    const u32 size = 16;
    std::memcpy(short_header.data(), &size, sizeof(size));
    CHECK(Origin(short_header) == PipelineCacheDataOrigin::Unknown);
}

namespace {

constexpr auto Mine = PipelineCacheDataOrigin::ThisDriver;
constexpr auto Other = PipelineCacheDataOrigin::OtherDriver;

} // Anonymous namespace

TEST_CASE("Every way a driver pipeline cache load ends does what nx2 did, or says why not",
          "[video_core][vulkan_pipeline_cache]") {
    struct Row {
        VulkanCacheLoad load;
        bool restored;
        bool drop_precache_marker;
        bool remove_file;
    };
    // The marker goes only where the driver was given the data and kept none of it, or the file
    // held none: a driver that rejects its own cache would reject it again after a rerun.
    for (const Row row : {
             Row{VulkanCacheLoad::Restored, true, false, false},
             Row{VulkanCacheLoad::NoFile, false, false, false},
             Row{VulkanCacheLoad::Damaged, false, false, true},
             Row{VulkanCacheLoad::OtherVersion, false, false, true},
             Row{VulkanCacheLoad::Rejected, false, false, true},
             Row{VulkanCacheLoad::NoData, false, true, false},
             Row{VulkanCacheLoad::DriverKeptNothing, false, true, true},
         }) {
        INFO(static_cast<int>(row.load));
        const auto actions = Vulkan::ActionsFor(row.load);
        CHECK(actions.restored == row.restored);
        CHECK(actions.drop_precache_marker == row.drop_precache_marker);
        CHECK(actions.remove_file == row.remove_file);
    }
}

TEST_CASE("A driver pipeline cache the driver kept is restored",
          "[video_core][vulkan_pipeline_cache]") {
    // The driver reports more than an empty cache of its own (MoltenVK: 36 bytes).
    CHECK(Vulkan::DecideVulkanCacheLoad(127845708, 127845708, 36, Other) ==
          VulkanCacheLoad::Restored);
    // The header never turns a kept cache into a dropped one, and this driver's data is restored
    // without asking: the first pass the load makes, before any probe.
    CHECK(Vulkan::DecideVulkanCacheLoad(127845708, std::nullopt, std::nullopt, Mine) ==
          VulkanCacheLoad::Restored);
    CHECK(Vulkan::DecideVulkanCacheLoad(Header, std::nullopt, std::nullopt, Other) ==
          VulkanCacheLoad::Restored);
}

TEST_CASE("A driver pipeline cache whose size queries failed is restored",
          "[video_core][vulkan_pipeline_cache]") {
    // Nothing to tell by: taken as kept, as it always was.
    CHECK(Vulkan::DecideVulkanCacheLoad(4096, std::nullopt, 36, Other) ==
          VulkanCacheLoad::Restored);
    CHECK(Vulkan::DecideVulkanCacheLoad(4096, 36, std::nullopt, Other) ==
          VulkanCacheLoad::Restored);
}

TEST_CASE("A driver pipeline cache made by another driver version is not restored",
          "[video_core][vulkan_pipeline_cache]") {
    // MoltenVK given 128 MB made by another build of it holds an empty cache's 36 bytes.
    CHECK(Vulkan::DecideVulkanCacheLoad(127845708, 36, 36, Other) ==
          VulkanCacheLoad::DriverKeptNothing);
    // Held below an empty cache's size, or a header of an unknown version the driver dropped.
    CHECK(Vulkan::DecideVulkanCacheLoad(4096, 32, 36, Other) == VulkanCacheLoad::DriverKeptNothing);
    CHECK(Vulkan::DecideVulkanCacheLoad(4096, 36, 36, PipelineCacheDataOrigin::Unknown) ==
          VulkanCacheLoad::DriverKeptNothing);
}

TEST_CASE("A driver pipeline cache this driver's header says is its own is restored",
          "[video_core][vulkan_pipeline_cache]") {
    // A driver that loads initial data lazily, or reports a size that does not follow its
    // contents: one signal does not delete a good cache, held at or below an empty cache's size.
    CHECK(Vulkan::DecideVulkanCacheLoad(127845708, 36, 36, Mine) == VulkanCacheLoad::Restored);
    CHECK(Vulkan::DecideVulkanCacheLoad(127845708, 32, 36, Mine) == VulkanCacheLoad::Restored);
}

TEST_CASE("A driver pipeline cache file holding an empty cache is restored",
          "[video_core][vulkan_pipeline_cache]") {
    // What a boot save writes where pipelines go through VK_KHR_pipeline_binary, not the cache:
    // the driver holding no more than it was given is no drop, whatever the header says.
    CHECK(Vulkan::DecideVulkanCacheLoad(36, 36, 36, Mine) == VulkanCacheLoad::Restored);
    CHECK(Vulkan::DecideVulkanCacheLoad(36, 36, 36, Other) == VulkanCacheLoad::Restored);
}

TEST_CASE("A driver pipeline cache file with no driver data is not restored",
          "[video_core][vulkan_pipeline_cache]") {
    // Lichii's header alone, as a save that read nothing left it, or less than Vulkan's header:
    // decided without giving it to the driver, whatever the bytes say.
    for (const size_t size : {size_t{0}, Header - 1}) {
        for (const auto origin : {Mine, Other, PipelineCacheDataOrigin::NoData,
                                  PipelineCacheDataOrigin::Unknown}) {
            CHECK(Vulkan::DecideVulkanCacheLoad(size, std::nullopt, std::nullopt, origin) ==
                  VulkanCacheLoad::NoData);
        }
    }
    CHECK(Vulkan::DecideVulkanCacheLoad(Header, 64, 36, Mine) == VulkanCacheLoad::Restored);
}
