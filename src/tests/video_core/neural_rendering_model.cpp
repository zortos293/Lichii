// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "video_core/neural_rendering/model.h"

namespace NR = VideoCore::NeuralRendering;
namespace {
/// Write a little-endian integer into a synthetic model fixture, extending its storage as
/// needed.
template <typename T>
void Put(std::vector<std::uint8_t>& bytes, std::size_t offset, T value) {
    if (bytes.size() < offset + sizeof(T)) {
        bytes.resize(offset + sizeof(T));
    }
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

/// Construct a minimal x64 PE fixture containing a weights resource without executable code.
std::vector<std::uint8_t> PeResource() {
    std::vector<std::uint8_t> bytes(0x308);
    Put<std::uint16_t>(bytes, 0, 0x5a4d);
    Put<std::uint32_t>(bytes, 0x3c, 0x80);
    Put<std::uint32_t>(bytes, 0x80, 0x4550);
    Put<std::uint16_t>(bytes, 0x84, 0x8664);
    Put<std::uint16_t>(bytes, 0x86, 1);
    Put<std::uint16_t>(bytes, 0x94, 0xf0);
    Put<std::uint16_t>(bytes, 0x98, 0x20b);
    Put<std::uint32_t>(bytes, 0x98 + 108, 16);
    Put<std::uint32_t>(bytes, 0x98 + 128, 0x1000);
    Put<std::uint32_t>(bytes, 0x98 + 132, 0x100);
    Put<std::uint32_t>(bytes, 0x188 + 12, 0x1000);
    Put<std::uint32_t>(bytes, 0x188 + 16, 0x200);
    Put<std::uint32_t>(bytes, 0x188 + 20, 0x200);
    Put<std::uint16_t>(bytes, 0x200 + 14, 1);
    Put<std::uint32_t>(bytes, 0x210, 10);
    Put<std::uint32_t>(bytes, 0x214, 0x80000020);
    Put<std::uint16_t>(bytes, 0x220 + 12, 1);
    Put<std::uint32_t>(bytes, 0x230, 0x80000060);
    Put<std::uint32_t>(bytes, 0x234, 0x80000040);
    Put<std::uint16_t>(bytes, 0x240 + 14, 1);
    Put<std::uint32_t>(bytes, 0x250, 1033);
    Put<std::uint32_t>(bytes, 0x254, 0x80);
    constexpr std::string_view name = "WEIGHTS_HT";
    Put<std::uint16_t>(bytes, 0x260, name.size());
    for (std::size_t i = 0; i < name.size(); ++i) {
        Put<std::uint16_t>(bytes, 0x262 + i * 2, name[i]);
    }
    Put<std::uint32_t>(bytes, 0x280, 0x1100);
    Put<std::uint32_t>(bytes, 0x284, 8);
    Put<std::uint64_t>(bytes, 0x300, 0x0807060504030201);
    return bytes;
}

std::size_t AddTensor(std::vector<std::uint8_t>& bytes, std::string_view name) {
    const auto start = bytes.size();
    Put<std::uint64_t>(bytes, start, name.size());
    bytes.insert(bytes.end(), name.begin(), name.end());
    Put<std::uint64_t>(bytes, bytes.size(), 46);
    const auto record = bytes.size();
    Put<std::uint64_t>(bytes, record, 46);
    Put<std::uint64_t>(bytes, record + 8, 6);
    Put<std::uint32_t>(bytes, record + 16, 1);
    bytes.insert(bytes.end(), {1, 2, 3, 4, 5, 6});
    Put<std::uint32_t>(bytes, record + 26, 0);
    Put<std::uint32_t>(bytes, record + 30, 0);
    Put<std::uint64_t>(bytes, record + 34, 1);
    Put<std::uint32_t>(bytes, record + 42, 3);
    Put<std::uint64_t>(bytes, 0, bytes.size());
    return record;
}
} // namespace

/// Verify resource extraction and rejection of truncated PE data without loading the DLL.
TEST_CASE("NR model resource parsing accepts data without loading executable code", "[nr_model]") {
    const auto dll = PeResource();
    const auto resource = NR::ReadWeightsResource(dll);
    const std::array<std::uint8_t, 8> expected{1, 2, 3, 4, 5, 6, 7, 8};
    REQUIRE(std::ranges::equal(resource, expected));
    for (std::size_t size = 0; size < dll.size(); ++size) {
        REQUIRE_THROWS_AS(NR::ReadWeightsResource(NR::Bytes{dll}.first(size)), std::runtime_error);
    }
}

TEST_CASE("NR rejects malformed resource pointers and ambiguous payloads", "[nr_model]") {
    auto dll = PeResource();
    SECTION("resource data is not file backed") {
        Put<std::uint32_t>(dll, 0x280, 0xffffffff);
    }
    SECTION("directory cannot point outside its resource") {
        Put<std::uint32_t>(dll, 0x234, 0xfffffff0);
    }
    SECTION("language entry cannot introduce a directory cycle") {
        Put<std::uint32_t>(dll, 0x254, 0x80000000);
    }
    SECTION("language selection must be unambiguous") {
        Put<std::uint16_t>(dll, 0x240 + 14, 2);
    }
    SECTION("resource name must be bounded") {
        Put<std::uint16_t>(dll, 0x260, 0xffff);
    }
    REQUIRE_THROWS_AS(NR::ReadWeightsResource(dll), std::runtime_error);
}

/// Verify packed tensor bytes survive parsing and truncated records are rejected.
TEST_CASE("NR packed tensor records retain their original bytes", "[nr_model]") {
    std::vector<std::uint8_t> bytes(8);
    AddTensor(bytes, "block0.layer0.layer");
    const auto tensors = NR::ReadPackedWeights(bytes);
    REQUIRE(tensors.size() == 1);
    const std::array<std::uint8_t, 6> expected{1, 2, 3, 4, 5, 6};
    REQUIRE(std::ranges::equal(tensors.at("block0.layer0.layer"), expected));
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        auto truncated = std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + size);
        if (size >= 8) {
            Put<std::uint64_t>(truncated, 0, size);
        }
        REQUIRE_THROWS_AS(NR::ReadPackedWeights(truncated), std::runtime_error);
    }
}

TEST_CASE("NR rejects invalid packed tensor metadata", "[nr_model]") {
    std::vector<std::uint8_t> bytes(8);
    const auto record = AddTensor(bytes, "block0.layer0.layer");
    SECTION("duplicate names") {
        AddTensor(bytes, "block0.layer0.layer");
    }
    SECTION("oversized payload") {
        Put<std::uint64_t>(bytes, record + 8, std::numeric_limits<std::uint64_t>::max());
    }
    SECTION("unsupported datatype") {
        Put<std::uint32_t>(bytes, record + 16, 2);
    }
    SECTION("record size disagreement") {
        Put<std::uint64_t>(bytes, record, 45);
    }
    SECTION("shape disagrees with payload") {
        Put<std::uint32_t>(bytes, record + 42, 4);
    }
    SECTION("dimension count overflow") {
        Put<std::uint64_t>(bytes, record + 34, std::numeric_limits<std::uint64_t>::max());
    }
    SECTION("unexpected record name") {
        bytes[16] = '/';
    }
    REQUIRE_THROWS_AS(NR::ReadPackedWeights(bytes), std::runtime_error);
}
