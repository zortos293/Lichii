// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/memory/dmnt_cheat_vm.h"

namespace {

using Core::Memory::CheatEntry;
using Core::Memory::CheatProcessMetadata;
using Core::Memory::DmntCheatVm;

constexpr u64 MainBase = 0x8000000;

struct Write {
    VAddr address;
    u64 value;
};

/// Memory that reads as zero and records each write.
class Callbacks final : public DmntCheatVm::Callbacks {
public:
    explicit Callbacks(std::vector<Write>& writes_) : writes{writes_} {}
    void MemoryReadUnsafe(VAddr, void* data, u64 size) override {
        std::memset(data, 0, static_cast<std::size_t>(size));
    }
    void MemoryWriteUnsafe(VAddr address, const void* data, u64 size) override {
        u64 value{};
        std::memcpy(&value, data, static_cast<std::size_t>(size));
        writes.push_back({address, value});
    }
    u64 HidKeysDown() override {
        return 0;
    }
    void PauseProcess() override {}
    void ResumeProcess() override {}
    void DebugLog(u8, u64) override {}
    void CommandLog(std::string_view) override {}

private:
    std::vector<Write>& writes;
};

CheatEntry Cheat(const std::vector<u32>& words) {
    CheatEntry cheat{};
    cheat.enabled = true;
    cheat.definition.num_opcodes = static_cast<u32>(words.size());
    std::copy(words.begin(), words.end(), cheat.definition.opcodes.begin());
    return cheat;
}

/// Runs the cheats, each of its words, `runs` times and returns what they wrote.
std::vector<Write> Run(const std::vector<std::vector<u32>>& cheats, int runs = 1) {
    std::vector<Write> writes;
    DmntCheatVm vm{std::make_unique<Callbacks>(writes)};
    std::vector<CheatEntry> entries;
    for (const auto& words : cheats) {
        entries.push_back(Cheat(words));
    }
    REQUIRE(vm.LoadProgram(entries));
    CheatProcessMetadata metadata{};
    metadata.main_nso_extents = {MainBase, 0x1000000};
    for (int run = 0; run < runs; ++run) {
        vm.Execute(metadata);
    }
    return writes;
}

std::vector<Write> Run(const std::vector<u32>& words) {
    return Run(std::vector<std::vector<u32>>{words});
}

// 400R0000 VVVVVVVV VVVVVVVV: register R = V.
std::vector<u32> Load(u32 reg, u64 value) {
    return {0x40000000 | reg << 16, static_cast<u32>(value >> 32), static_cast<u32>(value)};
}

// A8R00400 00001000: the 64-bit register R to main + 0x1000.
std::vector<u32> Store(u32 reg) {
    return {0xA8000400 | reg << 20, 0x00001000};
}

std::vector<u32> Join(std::initializer_list<std::vector<u32>> parts) {
    std::vector<u32> out;
    for (const auto& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

// 0T00 0000 AAAAAAAA V..: a static store of width T to main + A (0x1000 by default).
std::vector<u32> StoreStatic(u32 width, u32 value, u32 address = 0x1000) {
    return {0x00000000 | width << 24, address, value};
}

/// What the console's VM gives: the 64-bit shift by the count modulo 64, then the width.
u64 Shifted(u64 value, u64 count, bool left, u32 width) {
    const u64 shifted = left ? value << (count & 63) : value >> (count & 63);
    return width == 8 ? shifted : shifted & ((u64{1} << (width * 8)) - 1);
}

/// The value stored by a program ending in Store(0).
u64 Stored(const std::vector<u32>& words) {
    const auto writes = Run(words);
    REQUIRE(writes.size() == 1);
    REQUIRE(writes[0].address == MainBase + 0x1000);
    return writes[0].value;
}

} // Anonymous namespace

TEST_CASE("Cheat VM static shifts take their count modulo 64", "[core][cheats]") {
    // 7T0RC000 VVVVVVVV, 64-bit, register 0, C = 3 left or 4 right, by V.
    const auto shift = [](u32 type, u32 count) {
        return std::vector<u32>{0x78000000 | type << 12, count};
    };
    for (const u32 count : {0u, 1u, 63u, 64u, 65u, 127u, 0xFFFFFFFFu}) {
        CAPTURE(count);
        CHECK(Stored(Join({Load(0, 1), shift(3, count), Store(0)})) == u64{1} << (count & 63));
        CHECK(Stored(Join({Load(0, u64{1} << 63), shift(4, count), Store(0)})) ==
              (u64{1} << 63) >> (count & 63));
    }
}

TEST_CASE("Cheat VM register shifts take their count modulo 64", "[core][cheats]") {
    // 9TCRS0s0: 64-bit, C = 3 left or 4 right, register 0 = register 1 shifted by register 2.
    const auto shift = [](u32 type) { return std::vector<u32>{0x98001020 | type << 20}; };
    for (const u64 count : {u64{0}, u64{63}, u64{64}, u64{65}, u64{170}, ~u64{0}}) {
        CAPTURE(count);
        CHECK(Stored(Join({Load(1, 1), Load(2, count), shift(3), Store(0)})) ==
              u64{1} << (count & 63));
        CHECK(Stored(Join({Load(1, u64{1} << 63), Load(2, count), shift(4), Store(0)})) ==
              (u64{1} << 63) >> (count & 63));
    }
}

TEST_CASE("Cheat VM shifts the whole register, then applies the operation width",
          "[core][cheats]") {
    // A register with bits above every width, and counts at and past each width: masking the
    // count by the width, or truncating before the shift, would give other values.
    constexpr u64 Value = 0x8123456789ABCDEFULL;
    for (const u32 width : {1u, 2u, 4u}) {
        // 0x141 and 0x10041 for coverage: under the modulo, which only reads the low six bits,
        // an immediate of width 1 or 2 gives the same count whether it was truncated or not.
        for (const u32 count : {width * 8, width * 8 + 8, 63u, 64u, 72u, 0x141u, 0x10041u}) {
            for (const bool left : {true, false}) {
                CAPTURE(width, count, left);
                const u32 type = left ? 3 : 4;
                // 7T0RC000 VVVVVVVV: register 0 by the static count.
                CHECK(Stored(Join({Load(0, Value), {0x70000000 | width << 24 | type << 12, count},
                                   Store(0)})) == Shifted(Value, count, left, width));
                // 9TCRSIs0: register 0 = register 1 by register 2.
                CHECK(Stored(Join({Load(1, Value), Load(2, count),
                                   {0x90001020 | width << 24 | type << 20}, Store(0)})) ==
                      Shifted(Value, count, left, width));
                // 9TCRSI00 VVVVVVVV: register 0 = register 1 by an immediate of the width.
                CHECK(Stored(Join({Load(1, Value), {0x90001100 | width << 24 | type << 20, count},
                                   Store(0)})) ==
                      Shifted(Value, width == 4 ? count : count & ((1u << (width * 8)) - 1), left,
                              width));
            }
        }
    }
    // The 64-bit immediate form: two words.
    CHECK(Stored(Join({Load(1, Value), {0x98301100, 0, 65}, Store(0)})) ==
          Shifted(Value, 65, true, 8));
}

TEST_CASE("Cheat VM ends a cheat that skips outside any conditional block", "[core][cheats]") {
    // An else (21000000) with no conditional open used to reach an assertion that ended the
    // emulator. The cheat it is in ends there instead: the store after it does not run, one
    // before it still does, and so do the cheats after it.
    CHECK(Run(Join({{0x21000000}, Load(0, 7), Store(0)})).empty());
    CHECK(Stored(Join({Load(0, 7), Store(0), {0x21000000}, Load(0, 9), Store(0)})) == 7);
    // An else as the cheat's last word ends nothing that would have run.
    CHECK(Stored(Join({Load(0, 6), Store(0), {0x21000000}})) == 6);
    // Two cheats with one each: both end there, the cheat after them runs; and so on each run.
    const auto twice = Run(std::vector<std::vector<u32>>{Join({{0x21000000}, Load(0, 1), Store(0)}),
                                                         Join({{0x21000000}, Load(0, 2), Store(0)}),
                                                         Join({Load(0, 3), Store(0)})},
                           2);
    REQUIRE(twice.size() == 2);
    CHECK(twice[0].value == 3);
    CHECK(twice[1].value == 3);
    const auto writes =
        Run(std::vector<std::vector<u32>>{Join({{0x21000000}, Load(0, 7), Store(0)}),
                                          Join({Load(0, 8), Store(0)})});
    REQUIRE(writes.size() == 1);
    CHECK(writes[0].value == 8);
}

TEST_CASE("Cheat VM's static stores leave out those after an else outside any block",
          "[core][cheats]") {
    // MainStaticStores lists the stores Execute makes: none after the cheat has ended.
    const auto stores = DmntCheatVm::MainStaticStores(
        {Cheat(Join({StoreStatic(4, 1, 0x1000), {0x21000000}, StoreStatic(4, 2, 0x2000)})),
         Cheat(StoreStatic(4, 3, 0x3000))});
    REQUIRE(stores.size() == 2);
    CHECK(stores[0].cheat == 0);
    CHECK(stores[0].begin == 0x1000);
    CHECK(stores[1].cheat == 1);
    CHECK(stores[1].begin == 0x3000);
}

TEST_CASE("Cheat VM's static stores close the loops of a cheat that ends at a stray else",
          "[core][cheats]") {
    // The first cheat opens a loop (30R00000, register 1) and ends at the else before its loop
    // end: the next cheat's store runs once a run, outside any loop, and is listed as such.
    const std::vector<u32> abandoned{0x30100000, 2, 0x21000000, 0x31100000};
    const auto stores =
        DmntCheatVm::MainStaticStores({Cheat(abandoned), Cheat(StoreStatic(4, 3))});
    REQUIRE(stores.size() == 1);
    CHECK(stores[0].cheat == 1);
    CHECK(stores[0].begin == 0x1000);
    // Execute agrees: the store happens.
    const auto writes = Run(std::vector<std::vector<u32>>{abandoned, StoreStatic(4, 3)});
    REQUIRE(writes.size() == 1);
    CHECK(writes[0].value == 3);
}

TEST_CASE("Cheat VM's static stores keep a loop closed by the cheat that then ends",
          "[core][cheats]") {
    // Cheat 0 opens a loop on register 1, cheat 1 closes it and then ends at a stray else: the
    // loop is closed, so cheat 2's store runs once a run, outside any loop, and is listed.
    const std::vector<u32> opens{0x30100000, 2};
    const std::vector<u32> closes{0x31100000, 0x21000000};
    const auto stores = DmntCheatVm::MainStaticStores(
        {Cheat(opens), Cheat(closes), Cheat(StoreStatic(4, 3))});
    REQUIRE(stores.size() == 1);
    CHECK(stores[0].cheat == 2);
    const auto writes = Run(std::vector<std::vector<u32>>{opens, closes, StoreStatic(4, 3)});
    REQUIRE(writes.size() == 1);
    CHECK(writes[0].value == 3);
}

TEST_CASE("Cheat VM ends a cheat whose else is met outside its block on a later trip",
          "[core][cheats]") {
    // A keypress block holds a loop start and an else; going round the loop, Execute meets the
    // else outside the block and ends the cheat, so the store after the loop never runs.
    const std::vector<u32> words{0x80000000, 0x30100000, 2, 0x21000000, 0x20000000, 0x31100000,
                                 0x04000000, 0x1000,     1};
    CHECK(Run(words).empty());
    // A stated limit, not a requirement: the static walk follows the words in order and does
    // not see that, so it still lists the store, which errs on the side of warning.
    CHECK(DmntCheatVm::MainStaticStores({Cheat(words)}).size() == 1);
}

TEST_CASE("Cheat VM's static stores keep a store after a loop holding if, else and endif",
          "[core][cheats]") {
    // A valid cheat: a loop around a keypress block with an else, then a static store outside
    // any block or loop, which runs once a run and is listed.
    const std::vector<u32> words{0x30100000, 2,          0x80000000, 0x21000000, 0x20000000,
                                 0x31100000, 0x04000000, 0x1000,     1};
    const auto stores = DmntCheatVm::MainStaticStores({Cheat(words)});
    REQUIRE(stores.size() == 1);
    CHECK(stores[0].begin == 0x1000);
    const auto writes = Run(words);
    REQUIRE(writes.size() == 1);
    CHECK(writes[0].value == 1);
}

TEST_CASE("Cheat VM runs a store after an else that an if left open in a loop encloses",
          "[core][cheats]") {
    // A loop's body opens a keypress block without closing it, so each trip round the loop goes a
    // block deeper. After the loop, an endif and an else follow: Execute meets the else inside a
    // block, skips to the next endif, and runs the store after it.
    const std::vector<u32> words{0x30100000, 2,          0x80000000, 0x31100000, 0x20000000,
                                 0x21000000, 0x20000000, 0x04000000, 0x1000,     1};
    const auto writes = Run(words);
    REQUIRE(writes.size() == 1);
    CHECK(writes[0].value == 1);
    // A stated limit, not a requirement: the static walk, at depth 0 there, abandons the cheat
    // and leaves the store out. No well-formed cheat leaves an if open inside a loop.
    CHECK(DmntCheatVm::MainStaticStores({Cheat(words)}).empty());
}
