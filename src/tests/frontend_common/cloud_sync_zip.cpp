// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "frontend_common/cloud_sync/zip_archive.h"

using namespace FrontendCommon::CloudSync;

namespace {

std::vector<u8> Bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

} // Anonymous namespace

TEST_CASE("CloudSync: a zip round-trips its entries", "[cloud_sync]") {
    const std::vector<ZipEntry> entries{
        {"save.bin", Bytes(std::string(10000, 'x') + "end")},
        {"empty/", {}},
        {"sub/dir/file.txt", Bytes("hello")},
        {"sub/zero", {}},
    };
    const auto zip = WriteZip(entries);
    REQUIRE(zip.has_value());
    REQUIRE(zip->size() < 10000);
    REQUIRE(ReadZip(*zip) == entries);
    REQUIRE(WriteZip(entries) == zip);
}

TEST_CASE("CloudSync: a zip with no entries round-trips", "[cloud_sync]") {
    const auto zip = WriteZip({});
    REQUIRE(zip.has_value());
    REQUIRE(ReadZip(*zip) == std::vector<ZipEntry>{});
}

TEST_CASE("CloudSync: damaged zips are refused", "[cloud_sync]") {
    REQUIRE_FALSE(ReadZip(Bytes("not a zip")).has_value());

    auto zip = WriteZip(std::vector<ZipEntry>{{"a.txt", Bytes("some data to compress")}});
    REQUIRE(zip.has_value());
    // The deflated data starts after the 30-byte local header and the name.
    REQUIRE(zip->size() > 30 + 5);
    (*zip)[30 + 5] ^= 0xFF;
    REQUIRE_FALSE(ReadZip(*zip).has_value());
}

TEST_CASE("CloudSync: ZIP64 is chosen when deflate could reach 4 GiB", "[cloud_sync]") {
    REQUIRE_FALSE(NeedsZip64(0));
    REQUIRE_FALSE(NeedsZip64(100 << 20));
    REQUIRE_FALSE(NeedsZip64(0xFF000000));
    // Incompressible data this size can deflate past 4 GiB: 64 KiB of room isn't enough.
    REQUIRE(NeedsZip64(0xFFF00000));
    REQUIRE(NeedsZip64(0xFFFE0000));
    REQUIRE(NeedsZip64(0xFFFFFFFF));
    REQUIRE(NeedsZip64(u64{8} << 30));
}
