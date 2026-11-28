// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "core/file_sys/kernel_executable.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"

namespace {

using namespace FileSys;

void Put32(std::vector<u8>& out, std::size_t offset, u32 value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

struct Section {
    u32 offset;
    u32 size;
    u32 stored;
};

/// A KIP with these .text, .rodata, .data and .bss sections, none compressed. A section stores
/// `stored` bytes of 0x5A.
KIP MakeKip(const Section& text, const Section& rodata, const Section& data, const Section& bss) {
    std::vector<u8> bytes(0x100);
    Put32(bytes, 0, Common::MakeMagic('K', 'I', 'P', '1'));
    const Section sections[]{text, rodata, data, bss};
    for (std::size_t i = 0; i < 4; ++i) {
        Put32(bytes, 0x20 + i * 0x10, sections[i].offset);
        Put32(bytes, 0x24 + i * 0x10, sections[i].size);
        Put32(bytes, 0x28 + i * 0x10, i < 3 ? sections[i].stored : 0);
    }
    for (std::size_t i = 0; i < 3; ++i) {
        bytes.resize(bytes.size() + sections[i].stored, 0x5A);
    }
    return KIP{std::make_shared<VectorVfsFile>(std::move(bytes), "test.kip")};
}

constexpr Section Text{0, 4, 4};
constexpr Section RoData{0x1000, 0x10, 0x10};
constexpr Section Data{0x2000, 0x10, 0x10};
constexpr Section Bss{0x3000, 0x2000, 0};

} // Anonymous namespace

TEST_CASE("KIP keeps a valid layout and its stored sections", "[core][file_sys]") {
    const auto kip = MakeKip(Text, RoData, Data, Bss);
    REQUIRE(kip.GetStatus() == Loader::ResultStatus::Success);
    CHECK(kip.GetTextSection().size() == 4);
    CHECK(kip.GetRODataSection().size() == 0x10);
    CHECK(kip.GetDataSection().size() == 0x10);
    CHECK(kip.GetSectionSize(2) == 0x10);
    // .text to the end of .bss, page aligned.
    CHECK(kip.GetImageSize() == 0x5000);
    // The extents the loader copies into: page aligned, .data's running over .bss to the end.
    const auto& segments = kip.GetSegments();
    CHECK(segments[0].offset == 0);
    CHECK(segments[0].size == 0x1000);
    CHECK(segments[1].offset == 0x1000);
    CHECK(segments[1].size == 0x1000);
    CHECK(segments[2].offset == 0x2000);
    CHECK(segments[2].size == 0x3000);
    // Only .text, .rodata and .data are kept.
    for (u8 index = 3; index < 6; ++index) {
        CHECK(kip.GetSectionDecompressed(index).empty());
    }
}

TEST_CASE("KIP does not allocate a .bss the file holds no data for", "[core][file_sys]") {
    constexpr u32 bss_size = 0x10000000;
    const auto kip = MakeKip(Text, RoData, Data, {0x3000, bss_size, 0});
    REQUIRE(kip.GetStatus() == Loader::ResultStatus::Success);
    CHECK(kip.GetBSSOffset() == 0x3000);
    CHECK(kip.GetBSSSize() == bss_size);
    CHECK(kip.GetSectionDecompressed(3).empty());
}

TEST_CASE("KIP accepts a section stored with fewer bytes than its size, without allocating it",
          "[core][file_sys]") {
    // The kernel's loader and hactool copy the stored bytes and leave the rest of the segment
    // zero. Such a section used to be allocated at its size from the header as soon as the game
    // list read the .kip.
    constexpr u32 size = 0xABABAB94;
    constexpr u32 next = 0xABABC000;
    const auto kip = MakeKip({0, size, 0}, {next, 0, 0}, {next, 0, 0}, {next, 0, 0});
    REQUIRE(kip.GetStatus() == Loader::ResultStatus::Success);
    CHECK(kip.GetTextSection().empty());
    CHECK(kip.GetSectionSize(0) == size);
    CHECK(kip.GetImageSize() == next);

    const auto partly = MakeKip({0, 0x100, 4}, RoData, Data, Bss);
    REQUIRE(partly.GetStatus() == Loader::ResultStatus::Success);
    CHECK(partly.GetTextSection().size() == 4);
    CHECK(partly.GetSectionSize(0) == 0x100);
}

TEST_CASE("KIP keeps an empty section that is empty in memory too", "[core][file_sys]") {
    const auto kip = MakeKip(Text, {0x1000, 0, 0}, {0x1000, 0, 0}, {0x1000, 0, 0});
    REQUIRE(kip.GetStatus() == Loader::ResultStatus::Success);
    CHECK(kip.GetRODataSection().empty());
    CHECK(kip.GetDataSection().empty());
    CHECK(kip.GetImageSize() == 0x1000);
    // An empty segment: the loader leaves it unprotected, as the kernel's loader skips it.
    CHECK(kip.GetSegments()[1].size == 0);

    const auto no_text = MakeKip({0, 0, 0}, {0, 0x10, 0x10}, Data, Bss);
    REQUIRE(no_text.GetStatus() == Loader::ResultStatus::Success);
    CHECK(no_text.GetSegments()[0].size == 0);
    CHECK(no_text.GetSegments()[1].size == 0x1000);
}

TEST_CASE("KIP rejects a layout the kernel would refuse", "[core][file_sys]") {
    const auto rejects = [](const KIP& kip) {
        CHECK(kip.GetStatus() == Loader::ResultStatus::ErrorBadKIPHeader);
        // Whole but rejected: an INI1 can still step over it.
        CHECK(kip.IsSizeKnown());
    };
    SECTION(".text not at the start") {
        rejects(MakeKip({0x1000, 4, 4}, {0x2000, 0, 0}, {0x2000, 0, 0}, {0x2000, 0, 0}));
    }
    SECTION(".rodata or .data not on a page") {
        rejects(MakeKip(Text, {0x1004, 0x10, 0x10}, Data, Bss));
        rejects(MakeKip(Text, RoData, {0x2004, 0x10, 0x10}, Bss));
    }
    SECTION("a section running into the next") {
        rejects(MakeKip({0, 0x1001, 4}, RoData, Data, Bss));
        rejects(MakeKip(Text, {0x1000, 0x1001, 0x10}, Data, Bss));
        rejects(MakeKip(Text, RoData, {0x2000, 0x1001, 0x10}, Bss));
    }
    SECTION("sections out of order") {
        rejects(MakeKip(Text, {0x2000, 0x10, 0x10}, {0x1000, 0x10, 0x10}, Bss));
        rejects(MakeKip(Text, RoData, Data, {0x1000, 0x2000, 0}));
    }
    SECTION("sizes near 4 GiB, whose 32-bit sums would wrap") {
        rejects(MakeKip({0, 0xFFFFF001, 4}, RoData, Data, Bss));
        rejects(MakeKip(Text, RoData, {0x2000, 0xFFFFFFFF, 0x10}, Bss));
    }
    SECTION("more stored bytes than the section's size") {
        rejects(MakeKip({0, 4, 8}, RoData, Data, Bss));
    }
    SECTION("an image larger than a code set segment can describe") {
        rejects(MakeKip(Text, RoData, Data, {0xFFFFF000, 0x2000, 0}));
    }
}

TEST_CASE("KIP keeps nothing of a section it rejects", "[core][file_sys]") {
    // Read, then found to hold more bytes than its size.
    const auto kip = MakeKip({0, 4, 8}, RoData, Data, Bss);
    REQUIRE(kip.GetStatus() == Loader::ResultStatus::ErrorBadKIPHeader);
    CHECK(kip.GetTextSection().empty());
    CHECK(kip.GetRODataSection().empty());
    CHECK(kip.GetImageSize() == 0);
    CHECK(kip.GetSegments()[2].size == 0);
}
