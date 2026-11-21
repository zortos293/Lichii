// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "common/slot_vector.h"
#include "video_core/texture_cache/image_base.h"

namespace {

using VideoCommon::AliasedImage;
using VideoCommon::ImageBase;
using VideoCommon::ImageId;
using VideoCommon::ImageInfo;
using VideoCommon::ImageFlagBits;
using VideoCommon::ImageType;
using VideoCommon::SetModificationTick;
using VideoCommon::SubresourceRange;
using VideoCore::Surface::PixelFormat;

ImageInfo Info() {
    ImageInfo info;
    info.format = PixelFormat::A8B8G8R8_UNORM;
    info.type = ImageType::e2D;
    info.size = {64, 64, 1};
    info.resources.levels = 1;
    info.resources.layers = 1;
    return info;
}

/// What TextureCache::SynchronizeAliases relies on to return early.
bool NewestAliasTickHolds(Common::SlotVector<ImageBase>& images, ImageId id) {
    const ImageBase& image = images[id];
    return std::ranges::all_of(image.aliased_images, [&](const AliasedImage& alias) {
        return images[alias.id].modification_tick <= image.newest_alias_tick;
    });
}

} // namespace

TEST_CASE("Aliases see the tick a joined image takes from its copies",
          "[video_core][texture_cache]") {
    Common::SlotVector<ImageBase> images;
    const ImageId newer = images.insert(Info(), 0x10000, 0x10000);
    const ImageId older = images.insert(Info(), 0x10000, 0x10000);
    const ImageId joined = images.insert(Info(), 0x10000, 0x10000);
    SetModificationTick(images, images[older], 3);
    SetModificationTick(images, images[newer], 5);

    // As TextureCache::JoinImages: aliases are added first, then the copies set the new image's
    // tick from its overlaps, oldest first.
    REQUIRE(AddImageAlias(images[joined], images[older], joined, older));
    REQUIRE(AddImageAlias(images[joined], images[newer], joined, newer));
    SetModificationTick(images, images[joined], images[older].modification_tick);
    SetModificationTick(images, images[joined], images[newer].modification_tick);

    for (const ImageId id : {newer, older, joined}) {
        CHECK(NewestAliasTickHolds(images, id));
    }
    // The older alias must not skip its copy from the joined image.
    CHECK(images[older].newest_alias_tick > images[older].modification_tick);
    CHECK(images[newer].newest_alias_tick <= images[newer].modification_tick);
}

TEST_CASE("Adding an alias raises both images' newest alias tick",
          "[video_core][texture_cache]") {
    Common::SlotVector<ImageBase> images;
    const ImageId a = images.insert(Info(), 0x10000, 0x10000);
    const ImageId b = images.insert(Info(), 0x10000, 0x10000);
    SetModificationTick(images, images[a], 7);
    SetModificationTick(images, images[b], 2);
    REQUIRE(AddImageAlias(images[a], images[b], a, b));
    CHECK(images[a].newest_alias_tick == 2);
    CHECK(images[b].newest_alias_tick == 7);
    SetModificationTick(images, images[b], 9);
    CHECK(images[a].newest_alias_tick == 9);
    CHECK(NewestAliasTickHolds(images, a));
    CHECK(NewestAliasTickHolds(images, b));
}

TEST_CASE("Only storage writes over every layer of a level make it hold shader results",
          "[video_core][texture_cache]") {
    ImageInfo info = Info();
    info.resources.levels = 3;
    info.resources.layers = 4;
    ImageBase image(info, 0x10000, 0x10000);
    const auto range = [](s32 level, s32 levels, s32 layer, s32 layers) {
        return SubresourceRange{.base = {.level = level, .layer = layer},
                                .extent = {.levels = levels, .layers = layers}};
    };
    // As TextureCache::MarkStorageWrite: the write marks the image GPU-modified first.
    const auto storage_write = [&](const SubresourceRange& written) {
        const u32 before = image.storage_written_levels;
        image.flags = ImageFlagBits::GpuModified;
        image.storage_written_levels = 0;
        VideoCommon::NoteStorageWrite(image, before, written);
    };

    storage_write(range(0, 1, 0, 2));
    CHECK_FALSE(HoldsStorageWrites(image, range(0, 1, 0, 2)));
    storage_write(range(0, 1, 0, 4));
    CHECK(HoldsStorageWrites(image, range(0, 1, 0, 4)));
    CHECK(HoldsStorageWrites(image, range(0, 1, 2, 2)));
    // A level no storage write reached still holds whatever was there.
    CHECK_FALSE(HoldsStorageWrites(image, range(1, 1, 0, 4)));
    CHECK_FALSE(HoldsStorageWrites(image, range(0, 2, 0, 4)));
    // A partial write keeps what the level held.
    storage_write(range(0, 1, 1, 1));
    CHECK(HoldsStorageWrites(image, range(0, 1, 0, 4)));

    // As TextureCache::MarkModification for a clear, render pass or copy.
    image.storage_written_levels = 0;
    CHECK_FALSE(HoldsStorageWrites(image, range(0, 1, 0, 4)));

    storage_write(range(1, 1, 0, 4));
    CHECK(HoldsStorageWrites(image, range(1, 1, 0, 4)));
    image.flags |= ImageFlagBits::CpuModified;
    CHECK_FALSE(HoldsStorageWrites(image, range(1, 1, 0, 4)));
}

TEST_CASE("Small GPU-written images download on every CPU read on Switch 2",
          "[video_core][texture_cache]") {
    Common::SlotVector<ImageBase> images;
    ImageInfo exposure = Info();
    exposure.format = PixelFormat::R32G32_FLOAT;
    exposure.size = {8, 1, 1};
    const ImageId small = images.insert(exposure, 0x10000, 0x10000);
    const ImageId large = images.insert(Info(), 0x20000, 0x20000);
    REQUIRE(images[small].guest_size_bytes <= 0x1000);
    REQUIRE(images[large].guest_size_bytes > 0x1000);

    // Before its first read, every image downloads (GetFlushArea sets forced_flushed after).
    CHECK(VideoCommon::CpuReadDownloads(images[small], false));
    CHECK(VideoCommon::CpuReadDownloads(images[large], true));
    images[small].info.forced_flushed = true;
    images[large].info.forced_flushed = true;

    CHECK(VideoCommon::CpuReadDownloads(images[small], true));
    CHECK_FALSE(VideoCommon::CpuReadDownloads(images[large], true));
    CHECK_FALSE(VideoCommon::CpuReadDownloads(images[small], false));
}

TEST_CASE("A small readback image's async download clears GpuModified only when current",
          "[video_core][texture_cache]") {
    Common::SlotVector<ImageBase> images;
    ImageInfo exposure = Info();
    exposure.format = PixelFormat::R32G32_FLOAT;
    exposure.size = {8, 1, 1};
    const ImageId small = images.insert(exposure, 0x10000, 0x10000);
    const ImageId large = images.insert(Info(), 0x20000, 0x20000);
    REQUIRE(images[large].guest_size_bytes > 0x1000);
    // As PrepareImage after RefreshContents (load_tick = the counter) and MarkModification
    // (++counter), then CommitAsyncFlushes.
    for (const ImageId id : {small, large}) {
        images[id].flags = ImageFlagBits::GpuModified;
        images[id].load_tick = 6;
        images[id].modification_tick = 7;
    }
    ImageBase& image = images[small];
    const u64 staged = 7;

    CHECK(VideoCommon::AsyncReadbackLands(image, staged, true));
    CHECK_FALSE(VideoCommon::AsyncReadbackLands(image, staged, false));
    CHECK_FALSE(VideoCommon::AsyncReadbackLands(images[large], staged, true));

    // Written again by the GPU after the copy was staged: the copy is older.
    image.modification_tick = 8;
    CHECK_FALSE(VideoCommon::AsyncReadbackLands(image, staged, true));

    // The guest wrote its memory: what landed is not what the guest now holds.
    image.modification_tick = staged;
    image.flags |= ImageFlagBits::CpuModified;
    CHECK_FALSE(VideoCommon::AsyncReadbackLands(image, staged, true));

    // Uploaded again after the commit (RefreshContents cleared CpuModified): the GPU image holds
    // the guest's bytes, which the write-back overwrote.
    image.flags &= ~ImageFlagBits::CpuModified;
    image.load_tick = staged;
    CHECK_FALSE(VideoCommon::AsyncReadbackLands(image, staged, true));
    image.load_tick = staged + 3;
    CHECK_FALSE(VideoCommon::AsyncReadbackLands(image, staged, true));
    image.load_tick = staged - 1;
    CHECK(VideoCommon::AsyncReadbackLands(image, staged, true));
}

TEST_CASE("A small readback image's stale async download is not written back",
          "[video_core][texture_cache]") {
    Common::SlotVector<ImageBase> images;
    ImageInfo exposure = Info();
    exposure.format = PixelFormat::R32G32_FLOAT;
    exposure.size = {8, 1, 1};
    const ImageId small = images.insert(exposure, 0x10000, 0x10000);
    const ImageId large = images.insert(Info(), 0x20000, 0x20000);
    REQUIRE(images[large].guest_size_bytes > 0x1000);
    const u64 staged = 7;
    for (const ImageId id : {small, large}) {
        images[id].load_tick = 6;
        images[id].modification_tick = staged;
    }
    ImageBase& image = images[small];

    CHECK_FALSE(VideoCommon::SkipStaleReadbackWriteBack(image, staged, true));

    // Written again by the GPU after the copy was staged: the snapshot is older.
    image.modification_tick = 8;
    images[large].modification_tick = 8;
    CHECK(VideoCommon::SkipStaleReadbackWriteBack(image, staged, true));
    CHECK_FALSE(VideoCommon::SkipStaleReadbackWriteBack(image, staged, false));
    CHECK_FALSE(VideoCommon::SkipStaleReadbackWriteBack(images[large], staged, true));

    // Uploaded again since (a synchronous read cleared GpuModified, then the guest wrote its
    // memory): the snapshot would overwrite the guest's newer bytes.
    for (const ImageId id : {small, large}) {
        images[id].modification_tick = staged;
        images[id].load_tick = staged;
    }
    CHECK(VideoCommon::SkipStaleReadbackWriteBack(image, staged, true));
    CHECK_FALSE(VideoCommon::SkipStaleReadbackWriteBack(image, staged, false));
    CHECK_FALSE(VideoCommon::SkipStaleReadbackWriteBack(images[large], staged, true));
    image.load_tick = staged + 3;
    CHECK(VideoCommon::SkipStaleReadbackWriteBack(image, staged, true));
    image.load_tick = staged - 1;
    CHECK_FALSE(VideoCommon::SkipStaleReadbackWriteBack(image, staged, true));
}
