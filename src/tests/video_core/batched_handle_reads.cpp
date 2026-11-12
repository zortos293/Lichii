// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cstring>
#include <catch2/catch_test_macros.hpp>
#include "video_core/batched_handle_reads.h"

namespace {
using namespace VideoCommon::BatchedHandles;
struct Memory {
    std::array<U32, 4096> words{};
    mutable U64 copies{}, reads{};
    bool native{true}, mapped{true}, special{};
    bool CanBatchHandleWords() const { return native; }
    bool TryReadHandleWords(U64 address, std::span<U32> output) const {
        ++copies;
        if (!mapped || special || output.size() > 256 || (address & 4095) + output.size_bytes() > 4096 ||
            address + output.size_bytes() > words.size() * sizeof(U32)) return false;
        std::memcpy(output.data(), reinterpret_cast<const unsigned char*>(words.data()) + address, output.size_bytes());
        return true;
    }
    template <class T> T Read(U64 address) const {
        ++reads;
        return static_cast<T>(words[address / sizeof(U32)]);
    }
};
struct Descriptor {
    U32 count{128}, handle_program{~U32{0}}, cbuf_index{}, cbuf_offset{4096}, size_shift{3};
    bool has_secondary{};
};
struct Cbuf { U64 address{}; bool enabled{true}; };
using Cbufs = std::array<Cbuf, 1>;
}

TEST_CASE("Handle batches gather padded layouts and split at page boundaries", "[video_core]") {
    FailedValidation().store(false);
    for (U32 shift : {2U, 3U, 4U}) {
        Memory memory;
        Descriptor desc;
        desc.size_shift = shift;
        desc.cbuf_offset = 4092;
        for (U32 i = 0; i < desc.count; ++i) memory.words[(desc.cbuf_offset + (i << shift)) / 4] = i;
        Cursor batch{memory, desc, Cbufs{}, true, false};
        for (U32 i = 0; i < desc.count; ++i) REQUIRE(batch.Read(i) == i);
        REQUIRE(memory.copies == 3);
        REQUIRE(memory.reads == 0);
    }
}

TEST_CASE("Handle batches are opt-in and retain scalar fallback eligibility", "[video_core]") {
    FailedValidation().store(false);
    Memory memory;
    Descriptor desc;
    Cursor disabled{memory, desc, Cbufs{}, false, false};
    REQUIRE_FALSE(disabled.Read(0).has_value());
    auto small = desc; small.count = 1;
    auto program = desc; program.handle_program = 0;
    auto secondary = desc; secondary.has_secondary = true;
    auto stride = desc; stride.size_shift = 8;
    auto offset = desc; offset.cbuf_offset = ~U32{0} - 3;
    auto alignment = desc; alignment.cbuf_offset = 4097;
    for (auto candidate : {small, program, secondary, stride, offset, alignment}) {
        Cursor batch{memory, candidate, Cbufs{}, true, false};
        REQUIRE_FALSE(batch.Read(0).has_value());
    }
    auto cbufs = Cbufs{};
    cbufs[0].address = ~U64{0} - 1024;
    Cursor wrapped{memory, desc, cbufs, true, false};
    REQUIRE_FALSE(wrapped.Read(0).has_value());
    memory.native = false;
    Cursor non_native{memory, desc, Cbufs{}, true, false};
    REQUIRE_FALSE(non_native.Read(0).has_value());
    REQUIRE(memory.copies == 0);
}

TEST_CASE("Failed batch reads do not hide newly mapped memory or special words", "[video_core]") {
    FailedValidation().store(false);
    Memory memory;
    Descriptor desc;
    memory.mapped = false;
    Cursor batch{memory, desc, Cbufs{}, true, false};
    REQUIRE_FALSE(batch.Read(0).has_value());
    memory.mapped = true;
    memory.special = true;
    REQUIRE_FALSE(batch.Read(0).has_value());
    memory.special = false;
    memory.words[1024] = 123;
    REQUIRE(batch.Read(0) == 123);
    memory.words[1024] = 456;
    Cursor next_array{memory, desc, Cbufs{}, true, false};
    REQUIRE(next_array.Read(0) == 456);
}

TEST_CASE("Handle validation uses the fresh word and discards the remaining snapshot", "[video_core]") {
    FailedValidation().store(false);
    Memory memory;
    Descriptor desc;
    Cursor batch{memory, desc, Cbufs{}, true, true};
    REQUIRE(batch.Read(0) == 0);
    memory.words[1026] = 789;
    REQUIRE(batch.Read(1) == 789);
    REQUIRE(batch.ValidationFailed());
    REQUIRE(FailedValidation().load());
    REQUIRE_FALSE(batch.Read(2).has_value());
    Cursor next_array{memory, desc, Cbufs{}, true, true};
    REQUIRE_FALSE(next_array.Read(0).has_value());
    FailedValidation().store(false);
}
