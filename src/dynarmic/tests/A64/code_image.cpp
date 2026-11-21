// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// UserConfig::code_image: a program run with an image records its blocks; a second Jit (a new
// code cache at other addresses) given the serialised image installs them instead of compiling
// and must end in the same state. Covers what a block can hold: fastmem accesses with their
// fault records, far code, constants, host calls and returns, indirect branches,
// compact exits with their stubs. Changed guest code is never run from the image, and images
// for another key or damaged ones are rejected.

#include "dynarmic/interface/A64/code_image.h"

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <oaknut/oaknut.hpp>

#include "common/common_types.h"
#include "dynarmic/interface/A64/a64.h"

using namespace Dynarmic;
using namespace oaknut::util;

namespace {

class Env final : public A64::UserCallbacks {
public:
    alignas(4096) std::array<u8, 0x10000> mem{};
    A64::Jit* jit = nullptr;
    u64 svcs = 0;

    template<typename T>
    T Rd(u64 a) const {
        T v;
        std::memcpy(&v, mem.data() + a, sizeof v);
        return v;
    }
    template<typename T>
    void Wr(u64 a, T v) { std::memcpy(mem.data() + a, &v, sizeof v); }

    std::optional<u32> MemoryReadCode(u64 a) override {
        if (a + 4 > mem.size()) {
            return std::nullopt;
        }
        return Rd<u32>(a);
    }
    u64 MemoryRead(u64 a, size_t s) override {
        return s == 8 ? Rd<u64>(a) : s == 4 ? Rd<u32>(a)
                               : s == 2     ? Rd<u16>(a)
                                            : Rd<u8>(a);
    }
    A64::Vector MemoryRead128(u64 a) override { return Rd<A64::Vector>(a); }
    void MemoryWrite(u64 a, u64 v, size_t s) override {
        if (s == 8)
            Wr<u64>(a, v);
        else if (s == 4)
            Wr<u32>(a, u32(v));
        else if (s == 2)
            Wr<u16>(a, u16(v));
        else
            Wr<u8>(a, u8(v));
    }
    void MemoryWrite128(u64 a, A64::Vector v) override { Wr(a, v); }
    bool MemoryWriteExclusive8(u64 a, u8 v, u8) override { return Wr(a, v), true; }
    bool MemoryWriteExclusive16(u64 a, u16 v, u16) override { return Wr(a, v), true; }
    bool MemoryWriteExclusive32(u64 a, u32 v, u32) override { return Wr(a, v), true; }
    bool MemoryWriteExclusive64(u64 a, u64 v, u64) override { return Wr(a, v), true; }
    bool MemoryWriteExclusive128(u64 a, A64::Vector v, A64::Vector) override { return Wr(a, v), true; }
    void CallSVC(u32 swi) override {
        svcs += swi + 1;
        if (swi == 0)
            jit->HaltExecution();
    }
    void ExceptionRaised(u64, A64::Exception) override { jit->HaltExecution(); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1ull << 40; }
    u64 GetCNTPCT() override { return 0; }
};

constexpr u64 data_base = 0x8000;
constexpr u32 add_x0_x0_1 = 0x91000400;
constexpr u32 add_x0_x0_5 = 0x91001400;

/// The program; returns the index (in words) of the callee's `add x0, x0, #1`.
size_t WriteProgram(Env& env) {
    std::vector<u32> prog;
    oaknut::VectorCodeGenerator code{prog, nullptr};
    oaknut::Label loop, callee, cases, join, vcall;
    code.MOV(X0, XZR);
    code.MOV(X1, XZR);
    code.MOVZ(X19, 300);
    code.MOVZ(X20, data_base);
    code.ADR(X9, vcall);
    code.ADR(X21, cases);
    code.MOV(X10, 0x3FF8'0000'0000'0000ull);  // 1.5
    code.FMOV(D0, X10);
    code.FMOV(D1, XZR);
    code.l(loop);
    code.BL(callee);       // host call/return
    code.BLR(X9);          // indirect branch
    code.AND(X2, X19, 3);  // jump table: a megamorphic site
    code.ADD(X3, X21, X2, LSL, 4);
    code.BR(X3);
    code.l(cases);
    for (int i = 0; i < 4; ++i) {
        code.ADD(X1, X1, 1 << (3 * i));
        code.B(join);
        code.NOP();
        code.NOP();
    }
    code.l(join);
    code.LDR(X4, X20, 8);  // fastmem loads and stores, folded offsets
    code.ADD(X4, X4, X19);
    code.STR(X4, X20, 8);
    code.LDR(W5, X20, X2, oaknut::IndexExt::LSL, 2);
    code.ADD(W5, W5, 3);
    code.STR(W5, X20, X2, oaknut::IndexExt::LSL, 2);
    code.FADD(D1, D1, D0);  // FP: constants from the pool
    code.FSQRT(D2, D1);
    code.FCVTZS(X6, D2);
    code.ADD(X7, X7, X6);
    code.SVC(1);
    code.SUBS(X19, X19, 1);
    code.B(NE, loop);
    code.STR(D1, X20, 32);
    code.SVC(0);
    code.l(callee);
    const size_t patch_index = prog.size();
    code.ADD(X0, X0, 1);
    code.RET();
    code.l(vcall);
    code.ADD(X8, X8, 2);
    code.RET();
    std::memcpy(env.mem.data(), prog.data(), prog.size() * 4);
    return patch_index;
}

A64::UserConfig MakeConfig(Env& env, std::shared_ptr<A64::CodeImage> image, bool verify) {
    A64::UserConfig config{};
    config.callbacks = &env;
    config.fastmem_pointer = reinterpret_cast<uintptr_t>(env.mem.data());
    config.fastmem_address_space_bits = 64;
    config.silently_mirror_fastmem = false;
    config.enable_cycle_counting = false;
    config.omit_block_frame = true;
    config.halt_check_on_back_edges = true;
    config.host_call_return = true;
    config.compact_block_exits = true;
    config.far_code = true;
    config.code_cache_size = 8 * 1024 * 1024;
    config.reuse_host_flags = true;
    config.fold_address_offsets = true;
    config.elide_redundant_zext = true;
    config.spill_to_free_gpr = true;
    config.fast_dispatch_set_associative = true;
    config.fast_dispatch_table_bits = 12;
    config.core_independent_code = true;
    config.code_image = std::move(image);
    config.code_image_verify = verify;
    return config;
}

struct Outcome {
    std::array<u64, 31> regs{};
    std::array<u8, 64> data{};
    u64 svcs = 0;
    bool operator==(const Outcome&) const = default;
};

Outcome Run(Env& env, std::shared_ptr<A64::CodeImage> image, bool verify = false) {
    A64::Jit jit{MakeConfig(env, std::move(image), verify)};
    env.jit = &jit;
    env.svcs = 0;
    std::fill(env.mem.begin() + data_base, env.mem.begin() + data_base + 64, u8(0));
    jit.SetPC(0);
    jit.SetSP(0x4000);
    jit.Run();
    Outcome o;
    for (size_t i = 0; i < 31; ++i) {
        o.regs[i] = jit.GetRegister(i);
    }
    std::memcpy(o.data.data(), env.mem.data() + data_base, o.data.size());
    o.svcs = env.svcs;
    return o;
}

std::vector<u8> Key(u8 tag) {
    return {'t', 'e', 's', 't', tag};
}

}  // namespace

TEST_CASE("A64: code_image round trip", "[a64][code_image]") {
    Env env;
    WriteProgram(env);
    const Outcome reference = Run(env, nullptr);
    REQUIRE(reference.regs[0] == 300);
    REQUIRE(reference.regs[8] == 600);
    REQUIRE(reference.svcs == 601);

    auto first = std::make_shared<A64::CodeImage>(Key(1));
    REQUIRE(Run(env, first) == reference);
    const auto recorded = first->GetStatistics();
    REQUIRE(recorded.recorded > 10);
    REQUIRE(recorded.not_recordable == 0);
    REQUIRE(recorded.loaded == 0);
    REQUIRE(first->Dirty());
    const std::vector<u8> bytes = first->Serialize();
    REQUIRE_FALSE(first->Dirty());

    // A fresh Jit, code cache and image: every block comes from the image.
    auto second = std::make_shared<A64::CodeImage>(Key(1));
    REQUIRE(second->Deserialize(bytes));
    REQUIRE(second->GetStatistics().blocks == recorded.blocks);
    REQUIRE(Run(env, second) == reference);
    const auto loaded = second->GetStatistics();
    REQUIRE(loaded.loaded == recorded.recorded);
    REQUIRE(loaded.recorded == 0);
    REQUIRE(loaded.guest_changed == 0);
    REQUIRE(loaded.not_installable == 0);
    REQUIRE_FALSE(second->Dirty());

    // Serialising what was loaded gives the same image.
    REQUIRE(second->Serialize() == bytes);
}

TEST_CASE("A64: code_image verify mode", "[a64][code_image]") {
    Env env;
    WriteProgram(env);
    const Outcome reference = Run(env, nullptr);
    auto first = std::make_shared<A64::CodeImage>(Key(2));
    Run(env, first);
    auto second = std::make_shared<A64::CodeImage>(Key(2));
    REQUIRE(second->Deserialize(first->Serialize()));
    // Each block is installed (and read back) and compiled: the records must agree.
    REQUIRE(Run(env, second, true) == reference);
    const auto s = second->GetStatistics();
    REQUIRE(s.verify_mismatches == 0);
    REQUIRE(s.loaded == 0);
    REQUIRE(s.not_installable == 0);
}

TEST_CASE("A64: code_image never runs changed guest code", "[a64][code_image]") {
    Env env;
    const size_t patch_index = WriteProgram(env);
    REQUIRE(env.Rd<u32>(patch_index * 4) == add_x0_x0_1);
    auto first = std::make_shared<A64::CodeImage>(Key(3));
    Run(env, first);
    const std::vector<u8> bytes = first->Serialize();

    env.Wr<u32>(patch_index * 4, add_x0_x0_5);
    const Outcome reference = Run(env, nullptr);
    REQUIRE(reference.regs[0] == 1500);
    auto second = std::make_shared<A64::CodeImage>(Key(3));
    REQUIRE(second->Deserialize(bytes));
    REQUIRE(Run(env, second) == reference);
    const auto s = second->GetStatistics();
    REQUIRE(s.guest_changed == 1);
    REQUIRE(s.loaded > 10);
    REQUIRE(s.recorded == 1);  // the changed callee replaces its stale record

    // The next run loads the new callee too.
    auto third = std::make_shared<A64::CodeImage>(Key(3));
    REQUIRE(third->Deserialize(second->Serialize()));
    REQUIRE(Run(env, third) == reference);
    REQUIRE(third->GetStatistics().guest_changed == 0);
    REQUIRE(third->GetStatistics().recorded == 0);
}

TEST_CASE("A64: code_image rejects other keys and damaged data", "[a64][code_image]") {
    Env env;
    WriteProgram(env);
    auto first = std::make_shared<A64::CodeImage>(Key(4));
    Run(env, first);
    const std::vector<u8> bytes = first->Serialize();

    A64::CodeImage other_key{Key(5)};
    REQUIRE_FALSE(other_key.Deserialize(bytes));
    REQUIRE(other_key.GetStatistics().blocks == 0);

    A64::CodeImage image{Key(4)};
    REQUIRE(image.Deserialize(bytes));
    const size_t blocks = image.GetStatistics().blocks;
    REQUIRE(blocks > 10);
    // Header (magic, version): nothing.
    for (const size_t at : {size_t(0), size_t(9)}) {
        auto damaged = bytes;
        damaged[at] ^= 0x40;
        REQUIRE_FALSE(image.Deserialize(damaged));
        REQUIRE(image.GetStatistics().blocks == 0);
    }
    // The one chunk of records, damaged or cut off: the header alone.
    for (const size_t at : {bytes.size() / 2, bytes.size() - 1}) {
        auto damaged = bytes;
        damaged[at] ^= 0x40;
        REQUIRE(image.Deserialize(damaged));
        REQUIRE(image.GetStatistics().blocks == 0);
    }
    REQUIRE(image.Deserialize(std::span{bytes}.first(bytes.size() - 1)));
    REQUIRE(image.GetStatistics().blocks == 0);
    REQUIRE_FALSE(image.Deserialize({}));
}

TEST_CASE("A64: code_image appends new records", "[a64][code_image]") {
    Env env;
    const size_t patch_index = WriteProgram(env);
    auto image = std::make_shared<A64::CodeImage>(Key(7));
    const Outcome first_run = Run(env, image);
    std::vector<u8> file = image->Serialize();
    const size_t whole = image->GetStatistics().blocks;
    REQUIRE(image->SerializeAdded().empty());

    // The callee changes: its record is replaced, and only that goes into the appended chunk.
    env.Wr<u32>(patch_index * 4, add_x0_x0_5);
    const Outcome changed_run = Run(env, image);
    REQUIRE(changed_run.regs[0] == 1500);
    const std::vector<u8> chunk = image->SerializeAdded();
    REQUIRE_FALSE(chunk.empty());
    REQUIRE(chunk.size() < file.size() / 4);
    file.insert(file.end(), chunk.begin(), chunk.end());

    size_t consumed = 0;
    // The appended file: the later record wins, so the changed program runs entirely from it.
    auto loaded = std::make_shared<A64::CodeImage>(Key(7));
    REQUIRE(loaded->Deserialize(file, &consumed));
    REQUIRE(consumed == file.size());
    REQUIRE(loaded->GetStatistics().blocks == whole);
    REQUIRE(Run(env, loaded) == changed_run);
    REQUIRE(loaded->GetStatistics().guest_changed == 0);
    REQUIRE(loaded->GetStatistics().recorded == 0);

    // A cut-off appended chunk (a write that did not finish) leaves the image before it.
    auto cut = std::make_shared<A64::CodeImage>(Key(7));
    REQUIRE(cut->Deserialize(std::span{file}.first(file.size() - 3), &consumed));
    REQUIRE(consumed == file.size() - chunk.size());
    REQUIRE(cut->GetStatistics().blocks == whole);
    env.Wr<u32>(patch_index * 4, add_x0_x0_1);
    REQUIRE(Run(env, cut) == first_run);
    REQUIRE(cut->GetStatistics().guest_changed == 0);
}

TEST_CASE("A64: code_image keeps configs apart", "[a64][code_image]") {
    Env env;
    WriteProgram(env);
    auto image = std::make_shared<A64::CodeImage>(Key(6));
    Run(env, image);
    const size_t blocks = image->GetStatistics().blocks;

    // Another code-generation config: nothing of the first is installed, its own blocks are added.
    A64::UserConfig config = MakeConfig(env, image, false);
    config.far_code = false;
    A64::Jit jit{config};
    env.jit = &jit;
    jit.SetPC(0);
    jit.SetSP(0x4000);
    jit.Run();
    const auto s = image->GetStatistics();
    REQUIRE(s.loaded == 0);
    REQUIRE(s.not_recordable == 0);
    REQUIRE(s.blocks == 2 * blocks);
}
