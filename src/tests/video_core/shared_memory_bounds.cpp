// SPDX-License-Identifier: GPL-3.0-or-later

#include <deque>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
namespace IR = Shader::IR;

constexpr u32 SharedSize = 32768;
constexpr u32 Sink = SharedSize - 16;

struct OneBlock {
    Shader::ObjectPool<IR::Inst> pool{64};
    std::deque<IR::Block> blocks;
    IR::Program program;
    IR::Block* block;

    OneBlock() : block{&blocks.emplace_back(pool)} {
        program.blocks.push_back(block);
    }

    IR::Inst* Find(IR::Opcode opcode, size_t nth = 0) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == opcode && nth-- == 0) {
                return &inst;
            }
        }
        return nullptr;
    }
};

u32 ImmediateOffset(const IR::Inst* inst) {
    REQUIRE(inst != nullptr);
    REQUIRE(inst->Arg(0).IsImmediate());
    return inst->Arg(0).U32();
}
} // Anonymous namespace

TEST_CASE("SharedMemoryBoundsPass keeps accesses that fit", "[shader]") {
    OneBlock test;
    IR::IREmitter ir{*test.block};
    (void)ir.LoadShared(8, false, ir.Imm32(32767));
    ir.WriteShared(32, ir.Imm32(32764), ir.Imm32(1));
    ir.WriteShared(128, ir.Imm32(32752),
                   ir.CompositeConstruct(ir.Imm32(1), ir.Imm32(2), ir.Imm32(3), ir.Imm32(4)));

    Shader::Optimization::SharedMemoryBoundsPass(test.program, SharedSize);

    REQUIRE(ImmediateOffset(test.Find(IR::Opcode::LoadSharedU8)) == 32767);
    REQUIRE(ImmediateOffset(test.Find(IR::Opcode::WriteSharedU32)) == 32764);
    REQUIRE(ImmediateOffset(test.Find(IR::Opcode::WriteSharedU128)) == 32752);
}

TEST_CASE("SharedMemoryBoundsPass sends accesses past the end to the sink", "[shader]") {
    OneBlock test;
    IR::IREmitter ir{*test.block};
    (void)ir.LoadShared(16, false, ir.Imm32(32767));
    ir.WriteShared(32, ir.Imm32(32768), ir.Imm32(1));
    (void)ir.LoadShared(128, false, ir.Imm32(32760));
    (void)ir.SharedAtomicIAdd(ir.Imm32(40000), ir.Imm32(1));

    Shader::Optimization::SharedMemoryBoundsPass(test.program, SharedSize);

    REQUIRE(ImmediateOffset(test.Find(IR::Opcode::LoadSharedU16)) == Sink);
    REQUIRE(ImmediateOffset(test.Find(IR::Opcode::WriteSharedU32)) == Sink);
    REQUIRE(ImmediateOffset(test.Find(IR::Opcode::LoadSharedU128)) == Sink);
    REQUIRE(ImmediateOffset(test.Find(IR::Opcode::SharedAtomicIAdd32)) == Sink);
}

TEST_CASE("SharedMemoryBoundsPass selects a dynamic offset against the access size", "[shader]") {
    OneBlock test;
    IR::IREmitter ir{*test.block};
    const IR::U32 offset{ir.LocalInvocationIdX()};
    (void)ir.LoadShared(64, false, offset);

    Shader::Optimization::SharedMemoryBoundsPass(test.program, SharedSize);

    IR::Inst* const load{test.Find(IR::Opcode::LoadSharedU64)};
    REQUIRE(load != nullptr);
    IR::Inst* const select{load->Arg(0).InstRecursive()};
    REQUIRE(select->GetOpcode() == IR::Opcode::SelectU32);
    REQUIRE(select->Arg(1).InstRecursive() == offset.InstRecursive());
    REQUIRE(select->Arg(2).U32() == Sink);
    IR::Inst* const compare{select->Arg(0).InstRecursive()};
    REQUIRE(compare->GetOpcode() == IR::Opcode::ULessThanEqual);
    REQUIRE(compare->Arg(0).InstRecursive() == offset.InstRecursive());
    REQUIRE(compare->Arg(1).U32() == SharedSize - 8);
}

TEST_CASE("SharedMemoryBoundsPass leaves other instructions alone", "[shader]") {
    OneBlock test;
    IR::IREmitter ir{*test.block};
    (void)ir.IAdd(ir.Imm32(40000), ir.Imm32(1));

    Shader::Optimization::SharedMemoryBoundsPass(test.program, SharedSize);

    IR::Inst* const add{test.Find(IR::Opcode::IAdd32)};
    REQUIRE(add != nullptr);
    REQUIRE(add->Arg(0).U32() == 40000);
}
