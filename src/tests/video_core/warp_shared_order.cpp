// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <deque>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
namespace IR = Shader::IR;

struct TestProgram {
    Shader::ObjectPool<IR::Inst> pool{64};
    std::deque<IR::Block> blocks;
    IR::Program program;

    IR::Block& Block() {
        IR::Block& block{blocks.emplace_back(pool)};
        block.SetOrder(static_cast<u32>(blocks.size() - 1));
        return block;
    }

    void Run() {
        for (IR::Block& block : blocks) {
            program.blocks.push_back(&block);
        }
        Shader::Optimization::WarpSharedOrderPass(program);
    }

    // Opcodes of a block in order.
    std::vector<IR::Opcode> Opcodes(const IR::Block& block) const {
        std::vector<IR::Opcode> opcodes;
        for (const IR::Inst& inst : block) {
            opcodes.push_back(inst.GetOpcode());
        }
        return opcodes;
    }

    size_t Barriers() const {
        size_t count{};
        for (const IR::Block& block : blocks) {
            count += std::ranges::count(block.Instructions(), IR::Opcode::WarpSharedMemoryBarrier,
                                        &IR::Inst::GetOpcode);
        }
        return count;
    }
};
} // Anonymous namespace

TEST_CASE("A shared load after a store with no barrier between is ordered for the warp",
          "[shader][warp_shared_order]") {
    TestProgram test;
    IR::Block& block{test.Block()};
    IR::IREmitter ir{block};
    ir.WriteShared(32, ir.Imm32(4), ir.LaneId());
    static_cast<void>(ir.LoadShared(32, false, ir.Imm32(8)));
    test.Run();
    CHECK(test.Opcodes(block) ==
          std::vector{IR::Opcode::LaneId, IR::Opcode::WriteSharedU32,
                      IR::Opcode::WarpSharedMemoryBarrier, IR::Opcode::LoadSharedU32});
    CHECK(test.program.info.orders_warp_shared_memory);
}

TEST_CASE("A guest barrier between a shared store and load needs no warp barrier",
          "[shader][warp_shared_order]") {
    TestProgram test;
    IR::Block& block{test.Block()};
    IR::IREmitter ir{block};
    ir.WriteShared(32, ir.Imm32(4), ir.LaneId());
    ir.Barrier();
    static_cast<void>(ir.LoadShared(32, false, ir.Imm32(8)));
    static_cast<void>(ir.LoadShared(32, false, ir.Imm32(12)));
    test.Run();
    CHECK(test.Barriers() == 0);
    CHECK_FALSE(test.program.info.orders_warp_shared_memory);
}

TEST_CASE("Consecutive stores get one warp barrier before the load that follows them",
          "[shader][warp_shared_order]") {
    TestProgram test;
    IR::Block& block{test.Block()};
    IR::IREmitter ir{block};
    ir.WriteShared(32, ir.Imm32(0), ir.LaneId());
    ir.WriteShared(32, ir.Imm32(4), ir.LaneId());
    static_cast<void>(ir.LoadShared(32, false, ir.Imm32(8)));
    static_cast<void>(ir.LoadShared(32, false, ir.Imm32(12)));
    test.Run();
    CHECK(test.Barriers() == 1);
}

// do { x = shared[i - 1]; shared[i] += x; } while (...): the load of the next iteration reads
// what a neighbour stored in the previous one, as the Orbitals sort's warp scan does.
TEST_CASE("A store reaching a load over a loop back edge is ordered for the warp",
          "[shader][warp_shared_order]") {
    TestProgram test;
    IR::Block& entry{test.Block()};
    IR::Block& body{test.Block()};
    IR::Block& exit{test.Block()};
    entry.AddBranch(&body);
    body.AddBranch(&body);
    body.AddBranch(&exit);
    IR::IREmitter ir{body};
    const IR::U32 value{ir.LoadShared(32, false, ir.Imm32(0))};
    ir.WriteShared(32, ir.Imm32(4), value);
    test.Run();
    CHECK(test.Opcodes(body).front() == IR::Opcode::WarpSharedMemoryBarrier);
    CHECK(test.Barriers() == 1);
    CHECK(test.program.info.orders_warp_shared_memory);
}

TEST_CASE("Shared loads with no store before them stay unordered", "[shader][warp_shared_order]") {
    TestProgram test;
    IR::Block& block{test.Block()};
    IR::IREmitter ir{block};
    static_cast<void>(ir.LoadShared(32, false, ir.Imm32(0)));
    ir.WriteShared(32, ir.Imm32(4), ir.LaneId());
    test.Run();
    CHECK(test.Barriers() == 0);
    CHECK_FALSE(test.program.info.orders_warp_shared_memory);
}

TEST_CASE("A shared atomic after a store is ordered for the warp", "[shader][warp_shared_order]") {
    TestProgram test;
    IR::Block& block{test.Block()};
    IR::IREmitter ir{block};
    ir.WriteShared(32, ir.Imm32(4), ir.LaneId());
    static_cast<void>(ir.SharedAtomicIAdd(ir.Imm32(8), ir.Imm32(1)));
    test.Run();
    CHECK(test.Opcodes(block) ==
          std::vector{IR::Opcode::LaneId, IR::Opcode::WriteSharedU32,
                      IR::Opcode::WarpSharedMemoryBarrier, IR::Opcode::SharedAtomicIAdd32});
    CHECK(test.program.info.orders_warp_shared_memory);
}

TEST_CASE("Consecutive shared atomics need no warp barrier, a load after them does",
          "[shader][warp_shared_order]") {
    TestProgram test;
    IR::Block& block{test.Block()};
    IR::IREmitter ir{block};
    static_cast<void>(ir.SharedAtomicIAdd(ir.Imm32(0), ir.Imm32(1)));
    static_cast<void>(ir.SharedAtomicIAdd(ir.Imm32(4), ir.Imm32(1)));
    static_cast<void>(ir.LoadShared(32, false, ir.Imm32(8)));
    test.Run();
    CHECK(test.Opcodes(block) ==
          std::vector{IR::Opcode::SharedAtomicIAdd32, IR::Opcode::SharedAtomicIAdd32,
                      IR::Opcode::WarpSharedMemoryBarrier, IR::Opcode::LoadSharedU32});
}
