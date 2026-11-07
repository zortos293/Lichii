// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <optional>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "common/host_memory.h"
#include "common/literals.h"

using Common::HostMemory;
using namespace Common::Literals;

static constexpr size_t VIRTUAL_SIZE = 1ULL << 39;
static constexpr size_t BACKING_SIZE = 4_GiB;
// The move tests hold two buffers at once; Windows commits each backing buffer in full.
static constexpr size_t MOVE_VIRTUAL_SIZE = 1_GiB;
static constexpr size_t MOVE_BACKING_SIZE = 64_MiB;
static constexpr auto PERMS = Common::MemoryPermission::ReadWrite;
static constexpr auto HEAP = false;

static bool MapsSingleGuestPages() {
#ifdef _WIN32
    return true;
#else
    return sysconf(_SC_PAGESIZE) == 0x1000;
#endif
}

TEST_CASE("HostMemory: Initialize and deinitialize", "[common]") {
    {
        HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
        REQUIRE(mem.BackingBasePointer() != nullptr);
    }
    {
        HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
        REQUIRE(mem.BackingBasePointer() != nullptr);
    }
}

TEST_CASE("HostMemory: Simple map", "[common]") {
    if (!MapsSingleGuestPages()) {
        SKIP("Host pages larger than 4 KiB only map whole host pages");
    }
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x8000, 0x1000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x5000;
    data[0] = 50;
    REQUIRE(data[0] == 50);
}

TEST_CASE("HostMemory: Aligned contiguous map", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x40000, 0x80000, 0x10000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x40000;
    data[0] = 1;
    data[0x5fff] = 2;
    data[0xffff] = 3;
    REQUIRE(mem.BackingBasePointer()[0x80000] == 1);
    REQUIRE(mem.BackingBasePointer()[0x85fff] == 2);
    REQUIRE(mem.BackingBasePointer()[0x8ffff] == 3);
}

TEST_CASE("HostMemory: Simple mirror map", "[common]") {
    if (!MapsSingleGuestPages()) {
        SKIP("Host pages larger than 4 KiB only map whole host pages");
    }
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);
    mem.Map(0x8000, 0x4000, 0x1000, PERMS, HEAP);

    volatile u8* const mirror_a = mem.VirtualBasePointer() + 0x5000;
    volatile u8* const mirror_b = mem.VirtualBasePointer() + 0x8000;
    mirror_b[0] = 76;
    REQUIRE(mirror_a[0x1000] == 76);
}

TEST_CASE("HostMemory: Simple unmap", "[common]") {
    if (!MapsSingleGuestPages()) {
        SKIP("Host pages larger than 4 KiB only map whole host pages");
    }
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x5000;
    data[75] = 50;
    REQUIRE(data[75] == 50);

    mem.Unmap(0x5000, 0x2000, HEAP);
}

TEST_CASE("HostMemory: Simple unmap and remap", "[common]") {
    if (!MapsSingleGuestPages()) {
        SKIP("Host pages larger than 4 KiB only map whole host pages");
    }
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x5000;
    data[0] = 50;
    REQUIRE(data[0] == 50);

    mem.Unmap(0x5000, 0x2000, HEAP);

    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);
    REQUIRE(data[0] == 50);

    mem.Map(0x7000, 0x2000, 0x5000, PERMS, HEAP);
    REQUIRE(data[0x3000] == 50);
}

TEST_CASE("HostMemory: Nieche allocation", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x20000, PERMS, HEAP);
    mem.Unmap(0x0000, 0x4000, HEAP);
    mem.Map(0x1000, 0, 0x2000, PERMS, HEAP);
    mem.Map(0x3000, 0, 0x1000, PERMS, HEAP);
    mem.Map(0, 0, 0x1000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Full unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x8000, 0x4000, HEAP);
    mem.Map(0x6000, 0, 0x16000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Right out of bounds unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x2000, 0x4000, HEAP);
    mem.Map(0x2000, 0x80000, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Left out of bounds unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);

    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x6000, 0x4000, HEAP);
    mem.Map(0x8000, 0, 0x2000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Multiple placeholder unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x4000, 0, 0x1b000, PERMS, HEAP);
    mem.Unmap(0x3000, 0x1c000, HEAP);
    mem.Map(0x3000, 0, 0x20000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Unmap between placeholders", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x2000, 0x4000, HEAP);
    mem.Map(0x2000, 0, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Unmap to origin", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x4000, 0x4000, HEAP);
    mem.Map(0, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Unmap to right", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x8000, 0x4000, HEAP);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Partial right unmap check bindings", "[common]") {
    if (!MapsSingleGuestPages()) {
        SKIP("Host pages larger than 4 KiB only map whole host pages");
    }
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x4000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x1000] = 17;

    mem.Unmap(0x6000, 0x2000, HEAP);

    REQUIRE(ptr[0x1000] == 17);
}

TEST_CASE("HostMemory: Partial left unmap check bindings", "[common]") {
    if (!MapsSingleGuestPages()) {
        SKIP("Host pages larger than 4 KiB only map whole host pages");
    }
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x4000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x3000] = 19;
    ptr[0x3fff] = 12;

    mem.Unmap(0x4000, 0x2000, HEAP);

    REQUIRE(ptr[0x3000] == 19);
    REQUIRE(ptr[0x3fff] == 12);
}

TEST_CASE("HostMemory: Partial middle unmap check bindings", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x4000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x0000] = 19;
    ptr[0x3fff] = 12;

    mem.Unmap(0x1000, 0x2000, HEAP);

    REQUIRE(ptr[0x0000] == 19);
    REQUIRE(ptr[0x3fff] == 12);
}

TEST_CASE("HostMemory: Partial sparse middle unmap and check bindings", "[common]") {
    if (!MapsSingleGuestPages()) {
        SKIP("Host pages larger than 4 KiB only map whole host pages");
    }
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x2000, PERMS, HEAP);
    mem.Map(0x6000, 0x20000, 0x2000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x0000] = 19;
    ptr[0x3fff] = 12;

    mem.Unmap(0x5000, 0x2000, HEAP);

    REQUIRE(ptr[0x0000] == 19);
    REQUIRE(ptr[0x3fff] == 12);
}

TEST_CASE("HostMemory: Move construction transfers the buffer and its mappings", "[common][host_memory_move]") {
    HostMemory source(MOVE_BACKING_SIZE, MOVE_VIRTUAL_SIZE);
    u8* const backing = source.BackingBasePointer();
    u8* const view = source.VirtualBasePointer();
    REQUIRE(backing != nullptr);
    backing[0x1234] = 0x5a;

    HostMemory moved(std::move(source));
    CHECK(moved.BackingBasePointer() == backing);
    CHECK(moved.VirtualBasePointer() == view);
    CHECK(moved.VirtualSize() == MOVE_VIRTUAL_SIZE);
    CHECK(moved.BackingBasePointer()[0x1234] == 0x5a);
    // The moved-from object owns nothing, so destroying both releases the buffer once.
    CHECK(source.BackingBasePointer() == nullptr);
    CHECK(source.VirtualBasePointer() == nullptr);
    CHECK(source.VirtualSize() == 0);
    if (view != nullptr) {
        moved.Map(0x40000, 0x80000, 0x10000, PERMS, HEAP);
        moved.VirtualBasePointer()[0x40000] = 7;
        CHECK(moved.BackingBasePointer()[0x80000] == 7);
    }
}

TEST_CASE("HostMemory: Move assignment releases the old buffer and keeps the new one", "[common][host_memory_move]") {
    HostMemory target(MOVE_BACKING_SIZE, MOVE_VIRTUAL_SIZE);
    HostMemory source(MOVE_BACKING_SIZE, MOVE_VIRTUAL_SIZE);
    u8* const backing = source.BackingBasePointer();
    REQUIRE(backing != nullptr);
    REQUIRE(target.BackingBasePointer() != backing);
    backing[0] = 0x11;

    target = std::move(source);
    CHECK(target.BackingBasePointer() == backing);
    CHECK(target.BackingBasePointer()[0] == 0x11);
    CHECK(source.BackingBasePointer() == nullptr);

    // Assigning to itself keeps the buffer.
    HostMemory& alias = target;
    target = std::move(alias);
    CHECK(target.BackingBasePointer() == backing);
    CHECK(target.BackingBasePointer()[0] == 0x11);
}

TEST_CASE("HostMemory: Repeated create and destroy cycles succeed", "[common][host_memory_move]") {
    // ShutdownMainProcess destroys the guest RAM after every Switch 2 game and the next Load
    // creates it again; each cycle must give the buffer back.
    for (int cycle = 0; cycle < 8; ++cycle) {
        std::optional<HostMemory> memory;
        memory.emplace(BACKING_SIZE, VIRTUAL_SIZE);
        REQUIRE(memory->BackingBasePointer() != nullptr);
        memory->BackingBasePointer()[BACKING_SIZE - 1] = static_cast<u8>(cycle);
        CHECK(memory->BackingBasePointer()[BACKING_SIZE - 1] == static_cast<u8>(cycle));
        memory.reset();
    }
}