// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "core/loader/nro.h"

namespace {

struct Segment {
    u32 offset;
    u32 size;
};

void Put32(std::vector<u8>& out, std::size_t offset, u32 value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

constexpr Segment Text{0, 0x1000};
constexpr Segment RoData{0x1000, 0x1000};
constexpr Segment Data{0x2000, 0x1000};

// An NRO's header for the given segments, followed by zeros up to `stored` bytes in all.
std::vector<u8> MakeNro(Segment text, Segment rodata, Segment data, u32 file_size, u32 bss_size,
                        std::size_t stored) {
    std::vector<u8> bytes(stored);
    Put32(bytes, 0x10, Common::MakeMagic('N', 'R', 'O', '0'));
    Put32(bytes, 0x18, file_size);
    const Segment segments[] = {text, rodata, data};
    for (std::size_t i = 0; i < 3; ++i) {
        Put32(bytes, 0x20 + i * 8, segments[i].offset);
        Put32(bytes, 0x24 + i * 8, segments[i].size);
    }
    Put32(bytes, 0x38, bss_size);
    return bytes;
}

std::vector<u8> MakeNro(u32 bss_size = 0x2000) {
    return MakeNro(Text, RoData, Data, 0x3000, bss_size, 0x3000);
}

void PutMod0(std::vector<u8>& bytes, u32 offset, u32 bss_start, u32 bss_end) {
    Put32(bytes, 0x4, offset);
    Put32(bytes, offset, Common::MakeMagic('M', 'O', 'D', '0'));
    Put32(bytes, offset + 0x8, bss_start);
    Put32(bytes, offset + 0xC, bss_end);
}

} // Anonymous namespace

TEST_CASE("NRO layout of a valid NRO", "[core][loader]") {
    // No module header: .bss from the NRO's header. The same segments as the loader always gave.
    const auto bytes = MakeNro();
    const auto layout = Loader::ValidateNroLayout(bytes);
    REQUIRE(layout);
    CHECK(layout->file_size == 0x3000);
    CHECK(layout->image_size == 0x5000);
    CHECK(layout->segments[0].offset == 0);
    CHECK(layout->segments[0].size == 0x1000);
    CHECK(layout->segments[1].offset == 0x1000);
    CHECK(layout->segments[1].size == 0x1000);
    // .data and .bss.
    CHECK(layout->segments[2].offset == 0x2000);
    CHECK(layout->segments[2].size == 0x3000);
}

TEST_CASE("NRO layout takes .bss from the module header", "[core][loader]") {
    auto bytes = MakeNro();
    PutMod0(bytes, 0x100, 0x2F00, 0x2F00 + 0x3800);
    const auto layout = Loader::ValidateNroLayout(bytes);
    REQUIRE(layout);
    CHECK(layout->image_size == 0x3000 + 0x4000);
    CHECK(layout->segments[2].size == 0x1000 + 0x4000);
}

TEST_CASE("NRO layout passes over a module header it cannot use", "[core][loader]") {
    // .bss then comes from the NRO's header, as without one, and the layout says why.
    SECTION("outside the file") {
        auto bytes = MakeNro();
        Put32(bytes, 0x4, 0x3000 - 0x10);
        const auto layout = Loader::ValidateNroLayout(bytes);
        REQUIRE(layout);
        CHECK(layout->image_size == 0x5000);
        CHECK(layout->module_header_unused == "its offset is outside the file");
    }
    SECTION("with a .bss that ends before it starts") {
        auto bytes = MakeNro();
        PutMod0(bytes, 0x100, 0x2000, 0);
        const auto layout = Loader::ValidateNroLayout(bytes);
        REQUIRE(layout);
        CHECK(layout->image_size == 0x5000);
        CHECK(layout->module_header_unused == "its .bss ends before it starts");
    }
    SECTION("a usable one says nothing") {
        auto bytes = MakeNro();
        PutMod0(bytes, 0x100, 0x2F00, 0x2F00 + 0x1000);
        const auto layout = Loader::ValidateNroLayout(bytes);
        REQUIRE(layout);
        CHECK(layout->module_header_unused.empty());
    }
}

TEST_CASE("NRO layout takes empty segments", "[core][loader]") {
    // Everything in .text, .rodata and .data described as {0, 0}, as the loader always took.
    const auto bytes = MakeNro({0, 0x3000}, {0, 0}, {0, 0}, 0x3000, 0x1000, 0x3000);
    const auto layout = Loader::ValidateNroLayout(bytes);
    REQUIRE(layout);
    CHECK(layout->segments[0].size == 0x3000);
    CHECK(layout->segments[1].size == 0);
    // An empty .data leaves .bss alone, after the file's part.
    CHECK(layout->segments[2].offset == 0x3000);
    CHECK(layout->segments[2].size == 0x1000);
}

TEST_CASE("NRO layout bounds the image at what a u32 describes", "[core][loader]") {
    // At the limit, and one page over.
    const auto at_limit = Loader::ValidateNroLayout(MakeNro(0xFFFFF000 - 0x3000));
    REQUIRE(at_limit);
    CHECK(at_limit->image_size == 0xFFFFF000);
    std::string_view reason;
    CHECK_FALSE(Loader::ValidateNroLayout(MakeNro(0xFFFFF000 - 0x2000), &reason));
    CHECK(reason == "its image is larger than 4 GiB");
}

TEST_CASE("NRO layout of a file whose size is not page aligned", "[core][loader]") {
    // The file holds exactly its header's size: the image is page aligned, the copy is not.
    const auto bytes = MakeNro(Text, RoData, {0x2000, 0x800}, 0x2800, 0, 0x2800);
    const auto layout = Loader::ValidateNroLayout(bytes);
    REQUIRE(layout);
    CHECK(layout->file_size == 0x2800);
    CHECK(layout->image_size == 0x3000);
}

TEST_CASE("NRO layout rejects a damaged NRO and says why", "[core][loader]") {
    const auto rejects = [](const std::vector<u8>& bytes, std::string_view expected) {
        std::string_view reason;
        CHECK_FALSE(Loader::ValidateNroLayout(bytes, &reason));
        CHECK(reason == expected);
    };
    SECTION("a size larger than the file") {
        rejects(MakeNro(Text, RoData, Data, 0x10000, 0, 0x3000), "its size is larger than the file");
    }
    SECTION("a size smaller than its header") {
        rejects(MakeNro({0, 0}, {0, 0}, {0, 0}, 0x40, 0, 0x3000),
                "its size is smaller than its header");
    }
    SECTION("a segment past the NRO's size") {
        rejects(MakeNro(Text, RoData, {0x2000, 0x1001}, 0x3000, 0, 0x4000),
                "a segment ends past the NRO's size");
    }
    SECTION(".text not at 0") {
        rejects(MakeNro({0x1000, 0x1000}, {0x2000, 0x800}, {0x2800, 0x800}, 0x3000, 0, 0x3000),
                ".text does not start at 0");
    }
    SECTION("a segment off a page") {
        rejects(MakeNro(Text, {0x1800, 0x800}, Data, 0x3000, 0, 0x3000),
                "a segment does not start on a page");
    }
    SECTION("a segment that runs into the next") {
        rejects(MakeNro({0, 0x1800}, RoData, Data, 0x3000, 0, 0x3000),
                "a segment starts before the one ahead of it ends");
    }
    SECTION("segments out of order") {
        rejects(MakeNro(Text, Data, RoData, 0x3000, 0, 0x3000),
                "a segment starts before the one ahead of it ends");
    }
    SECTION("too short for a header, or not an NRO") {
        rejects(std::vector<u8>(0x40), "the file is shorter than an NRO header");
        auto bytes = MakeNro();
        Put32(bytes, 0x10, 0);
        rejects(bytes, "the file is not an NRO");
    }
}
