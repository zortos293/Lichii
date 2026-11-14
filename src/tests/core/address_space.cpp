// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/literals.h"
#include "core/hle/kernel/k_address_space_info.h"

using namespace Common::Literals;

TEST_CASE("Switch 2 alias space holds both Xenoblade 2 address reservations", "[kernel][alias]") {
    using Space = Kernel::KAddressSpaceInfo;
    using Type = Space::Type;

    const auto alias_size = Space::GetAddressSpaceSize(39, Type::Alias, true);
    REQUIRE(alias_size == 128_GiB);
    // The SDK reserves 60 GiB during startup and another 60 GiB in nnMain.
    REQUIRE(alias_size > 2 * 60_GiB);
    REQUIRE(Space::GetAddressSpaceSize(39, Type::Alias, false) == 64_GiB);
    REQUIRE(Space::GetAddressSpaceSize(39, Type::Alias) == 64_GiB);
}

TEST_CASE("Switch 2 alias expansion preserves the other address layouts", "[kernel][alias]") {
    using Space = Kernel::KAddressSpaceInfo;
    using Type = Space::Type;

    REQUIRE(Space::GetAddressSpaceSize(32, Type::Alias, true) == 1_GiB);
    REQUIRE(Space::GetAddressSpaceSize(36, Type::Alias, true) == 6_GiB);
    REQUIRE(Space::GetAddressSpaceSize(39, Type::Heap, true) == 16_GiB);
    REQUIRE(Space::GetAddressSpaceSize(39, Type::Heap, false) == 16_GiB);
    for (const auto type : {Type::Map39Bit, Type::MapSmall, Type::Stack}) {
        REQUIRE(Space::GetAddressSpaceSize(39, type, true) ==
                Space::GetAddressSpaceSize(39, type, false));
    }
}

TEST_CASE("Switch 2 address regions fit the reduced NCE address space", "[kernel][alias]") {
    using Space = Kernel::KAddressSpaceInfo;
    using Type = Space::Type;

    const auto reserved_size = Space::GetAddressSpaceSize(39, Type::Alias, true) +
                               Space::GetAddressSpaceSize(39, Type::Heap, true) +
                               Space::GetAddressSpaceSize(39, Type::Stack, true) +
                               Space::GetAddressSpaceSize(39, Type::MapSmall, true);
    REQUIRE(reserved_size < 256_GiB - 128_MiB);
}
