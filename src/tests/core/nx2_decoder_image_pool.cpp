// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <memory>
#include <mutex>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_gsv.h"

namespace {

using Service::Nx2CheckedDecoderPool;

// A preference with layout code `code` and the given dimensions (word 2: (w - 1) | (h - 1) << 16).
std::array<u32, 16> Preference(u32 code, u32 width, u32 height) {
    std::array<u32, 16> preference{};
    preference[1] = code;
    preference[2] = (width - 1) | ((height - 1) << 16);
    return preference;
}

} // Anonymous namespace

TEST_CASE("A video's decoder image pool keeps its layout", "[nx2_gsv]") {
    const auto preference = Preference(4, 1920, 1080);
    const auto checked = Nx2CheckedDecoderPool(preference.data(), 8);
    REQUIRE(checked);
    CHECK(checked->width == 1920);
    CHECK(checked->height == 1080);
    // NV12 in 128-row blocks: 1920 x 1152 luma, then 1920 x 640 chroma.
    CHECK(checked->size == 1920 * 1152 + 1920 * 640);
}

TEST_CASE("A decoder image pool past its limits is refused", "[nx2_gsv]") {
    constexpr u32 MaxDimension = Service::Nx2MaxDecoderImageDimension;
    using Service::Nx2MaxDecoderImages;
    // The largest dimensions and count the 16-bit fields and the count word can carry.
    CHECK_FALSE(Nx2CheckedDecoderPool(Preference(2, 65536, 65536).data(), 1));
    CHECK_FALSE(Nx2CheckedDecoderPool(Preference(2, MaxDimension + 1, 16).data(), 1));
    CHECK_FALSE(Nx2CheckedDecoderPool(Preference(4, 16, MaxDimension + 1).data(), 1));
    CHECK(Nx2CheckedDecoderPool(Preference(4, 1920, 1080).data(), Nx2MaxDecoderImages));
    CHECK_FALSE(Nx2CheckedDecoderPool(Preference(4, 1920, 1080).data(), Nx2MaxDecoderImages + 1));
    // The largest count the word carries, refused by the count limit.
    CHECK_FALSE(Nx2CheckedDecoderPool(Preference(4, 1920, 1080).data(), 0xFFFFFFFF));
    // Within each limit alone, but over the pool's total: 9 images of 256 MiB.
    const auto largest = Preference(2, MaxDimension, MaxDimension);
    CHECK(Nx2CheckedDecoderPool(largest.data(), 8));
    CHECK_FALSE(Nx2CheckedDecoderPool(largest.data(), 9));
}

TEST_CASE("Image ids are taken without wrapping", "[nx2_gsv]") {
    Service::Nx2NativeObjects objects;
    CHECK(objects.TakeImageIds(0) == std::nullopt);
    CHECK(objects.TakeImageIds(3) == 1U);
    CHECK(objects.next_image == 4);
    // Up to the top of the range: the last id taken is 0xFFFFFFFE, next_image ends at the top.
    objects.next_image = 0xFFFFFFF0;
    CHECK(objects.TakeImageIds(0x10) == std::nullopt);
    CHECK(objects.next_image == 0xFFFFFFF0);
    CHECK(objects.TakeImageIds(0xF) == 0xFFFFFFF0U);
    CHECK(objects.next_image == 0xFFFFFFFF);
    CHECK(objects.TakeImageIds(1) == std::nullopt);
}

TEST_CASE("Memory ids are taken without wrapping", "[nx2_gsv]") {
    Service::Nx2NativeObjects objects;
    CHECK(objects.TakeMemoryId() == 1U);
    CHECK(objects.TakeMemoryId() == 2U);
    objects.next_memory = 0xFFFFFFFE;
    CHECK(objects.TakeMemoryId() == 0xFFFFFFFEU);
    CHECK(objects.TakeMemoryId() == std::nullopt);
    CHECK(objects.next_memory == 0xFFFFFFFF);
}

TEST_CASE("Only an existing image of another kind is refused decoded frames", "[nx2_gsv]") {
    Service::Nx2NativeObjects objects;
    auto& decoder = objects.images[1];
    decoder.decoder = true;
    decoder.decoder_mutex = std::make_shared<std::mutex>();
    objects.images[2]; // a native image (429): no decoder mutex
    auto& marked = objects.images[3];
    marked.decoder = true; // marked a decoder image without its mutex
    CHECK_FALSE(objects.IsOtherImage(1));
    CHECK(objects.IsOtherImage(2));
    CHECK(objects.IsOtherImage(3));
    CHECK_FALSE(objects.IsOtherImage(4));
    CHECK(objects.DecoderMutex(2) == nullptr);
}

TEST_CASE("Every image of a native image pool is refused decoded frames", "[nx2_gsv]") {
    Service::Nx2NativeObjects objects;
    // A native image pool of three (429): one entry, at its first id, for ids 5 to 7.
    objects.images[5].count = 3;
    // A decoder pool of two (911) right after it: an entry per image.
    for (const u32 id : {8U, 9U}) {
        auto& decoder = objects.images[id];
        decoder.decoder = true;
        decoder.decoder_mutex = std::make_shared<std::mutex>();
    }
    CHECK(objects.IsOtherImage(5));
    CHECK(objects.IsOtherImage(6));
    CHECK(objects.IsOtherImage(7));
    CHECK_FALSE(objects.IsOtherImage(8));
    CHECK_FALSE(objects.IsOtherImage(9));
    // Missing ids, before the first image and past the last pool, are still not refused.
    CHECK_FALSE(objects.IsOtherImage(4));
    CHECK_FALSE(objects.IsOtherImage(10));
    // A missing id right after a pool, with nothing after it.
    Service::Nx2NativeObjects alone;
    alone.images[1].count = 2;
    CHECK(alone.IsOtherImage(2));
    CHECK_FALSE(alone.IsOtherImage(3));
    CHECK_FALSE(alone.IsOtherImage(0));
}

TEST_CASE("Ids no image has are not refused decoded frames, wherever they fall", "[nx2_gsv]") {
    // No image at all.
    Service::Nx2NativeObjects empty;
    CHECK_FALSE(empty.IsOtherImage(0));
    CHECK_FALSE(empty.IsOtherImage(1));
    CHECK_FALSE(empty.IsOtherImage(0xFFFFFFFF));
    // A native pool of three at 5 (ids 5 to 7) and a decoder image at 9: 8 lies between them.
    Service::Nx2NativeObjects objects;
    objects.images[5].count = 3;
    auto& decoder = objects.images[9];
    decoder.decoder = true;
    decoder.decoder_mutex = std::make_shared<std::mutex>();
    CHECK_FALSE(objects.IsOtherImage(8));
    CHECK_FALSE(objects.IsOtherImage(9));
    // The decoder image right after the pool, released (erased): its id is missing again.
    objects.images.erase(9);
    CHECK_FALSE(objects.IsOtherImage(8));
    CHECK_FALSE(objects.IsOtherImage(9));
    // A pool ending at the top of the id range: the last id is past it, not wrapped into it.
    Service::Nx2NativeObjects top;
    top.images[0xFFFFFFF0].count = 0xF;
    CHECK(top.IsOtherImage(0xFFFFFFFE));
    CHECK_FALSE(top.IsOtherImage(0xFFFFFFFF));
}

TEST_CASE("A decoder image is written only within its memory", "[nx2_gsv]") {
    using Service::Nx2NativeObjects;
    const Nx2NativeObjects::Memory memory{.size = 0x10000};
    CHECK(Nx2NativeObjects::Holds(memory, 0, 0x10000));
    CHECK(Nx2NativeObjects::Holds(memory, 0x8000, 0x8000));
    CHECK_FALSE(Nx2NativeObjects::Holds(memory, 0x8000, 0x8001));
    CHECK_FALSE(Nx2NativeObjects::Holds(memory, 0x10001, 0));
    // An offset and size whose sum wraps.
    CHECK_FALSE(Nx2NativeObjects::Holds(memory, 0x8000, ~u64{0}));
    // A memory 111 gave no size holds nothing.
    CHECK_FALSE(Nx2NativeObjects::Holds(Nx2NativeObjects::Memory{}, 0, 1));
    // Declared large at 111, bound smaller at 114: the bound range is the limit.
    const Nx2NativeObjects::Memory bound{.size = 0x10000, .bound_size = 0x4000, .bound = true};
    CHECK(Nx2NativeObjects::Holds(bound, 0, 0x4000));
    CHECK_FALSE(Nx2NativeObjects::Holds(bound, 0, 0x4001));
    // Bound larger than declared: the declared size is the limit.
    const Nx2NativeObjects::Memory wide{.size = 0x4000, .bound_size = 0x10000, .bound = true};
    CHECK_FALSE(Nx2NativeObjects::Holds(wide, 0x2000, 0x2001));
    // Bound with size 0: the bound range is empty, though 111 declared a size.
    const Nx2NativeObjects::Memory empty{.size = 0x4000, .bound_size = 0, .bound = true};
    CHECK_FALSE(Nx2NativeObjects::Holds(empty, 0, 1));
    CHECK(Nx2NativeObjects::Limit(bound) == 0x4000);
}

TEST_CASE("An image pool's size is refused when it does not fit in 64 bits", "[nx2_gsv]") {
    using Service::Nx2ImagesSize;
    CHECK(Nx2ImagesSize(0x1000, 3) == 0x3000U);
    CHECK(Nx2ImagesSize(0, 0xFFFFFFFF) == 0U);
    CHECK(Nx2ImagesSize(u64{1} << 32, 0xFFFFFFFF) == (u64{1} << 32) * 0xFFFFFFFFULL);
    CHECK(Nx2ImagesSize(u64{1} << 33, 0xFFFFFFFF) == std::nullopt);
    CHECK(Nx2ImagesSize(~u64{0}, 1) == ~u64{0});
    CHECK(Nx2ImagesSize(~u64{0}, 2) == std::nullopt);
    // Exactly 2^64, and 2^64 - 2^31.
    CHECK(Nx2ImagesSize(u64{1} << 33, 0x80000000) == std::nullopt);
    CHECK(Nx2ImagesSize((u64{1} << 33) - 1, 0x80000000) ==
          ((u64{1} << 33) - 1) * 0x80000000ULL);
}
