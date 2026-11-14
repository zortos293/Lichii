// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/engines/maxwell_dma_const_a.h"

TEST_CASE("CONST_A DMA remap repeats exactly one component per element",
          "[video_core][maxwell_dma]") {
    STATIC_REQUIRE(std::endian::native == std::endian::little);
    STATIC_REQUIRE(Tegra::Engines::ExpandConstA(0x12345678U, 1) == 0x78787878U);
    STATIC_REQUIRE(Tegra::Engines::ExpandConstA(0x12345678U, 2) == 0x56785678U);
    STATIC_REQUIRE(Tegra::Engines::ExpandConstA(0x12345678U, 4) == 0x12345678U);

    constexpr std::array<std::uint32_t, 8> values{
        0, 1, 0xff, 0x01020304, 0x12345678, 0xabcd0001, 0xff00ff00, 0xffffffff,
    };
    for (const auto width : {1U, 2U, 4U}) {
        for (const auto count : {0U, 1U, 2U, 3U, 7U, 31U, 1024U}) {
            for (const auto value : values) {
                INFO("width=" << width << " count=" << count << " value=" << value);
                const std::vector<std::uint32_t> words(
                    count, Tegra::Engines::ExpandConstA(value, width));
                std::vector<std::uint8_t> actual(count * width);
                if (!actual.empty()) {
                    std::memcpy(actual.data(), words.data(), actual.size());
                }
                std::vector<std::uint8_t> expected(count * width);
                for (std::size_t byte = 0; byte < expected.size(); ++byte) {
                    expected[byte] =
                        static_cast<std::uint8_t>(value >> (8 * (byte % width)));
                }
                CHECK(actual == expected);
            }
        }
    }
}
