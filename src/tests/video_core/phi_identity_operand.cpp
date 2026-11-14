// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/object_pool.h"

namespace IR = Shader::IR;

// A trivial phi the SSA pass replaced with a constant becomes `Identity #false`. A phi completed
// later (SealBlock) used to keep a reference to that identity without counting it as a use: dead
// code elimination removed the identity and the phi was left with an operand that has no
// definition (SPIR-V "Id is 0" in seven of The Duskbloods' compute programs).
TEST_CASE("A phi operand that is an identity of an immediate survives dead code elimination",
          "[video_core][shader]") {
    Shader::ObjectPool<IR::Inst> pool{64};
    IR::Block entry{pool};
    IR::Block left{pool};
    IR::Block right{pool};
    IR::Block merge{pool};
    entry.AddBranch(&left);
    entry.AddBranch(&right);
    left.AddBranch(&merge);
    right.AddBranch(&merge);

    left.AppendNewInst(IR::Opcode::Identity, {IR::Value{false}});
    const IR::Value identity{&left.back()};
    right.AppendNewInst(IR::Opcode::GetPred, {IR::Value{IR::Pred::P0}});
    const IR::Value predicate{&right.back()};

    merge.AppendNewInst(IR::Opcode::Phi, {});
    IR::Inst& phi{merge.back()};
    phi.SetFlags(IR::Type::U1);
    phi.AddPhiOperand(&left, identity);
    phi.AddPhiOperand(&right, predicate);
    // Keep the phi alive: it selects the fragment depth.
    merge.AppendNewInst(IR::Opcode::SelectF32, {IR::Value{&phi}, IR::Value{1.0f}, IR::Value{0.0f}});
    merge.AppendNewInst(IR::Opcode::SetFragDepth, {IR::Value{&merge.back()}});

    IR::Program program;
    program.blocks = {&entry, &left, &right, &merge};
    program.post_order_blocks = {&merge, &right, &left, &entry};
    Shader::Optimization::DeadCodeEliminationPass(program);

    REQUIRE(phi.NumArgs() == 2);
    // The identity's operand is stored as the immediate itself.
    REQUIRE(phi.Arg(0).IsImmediate());
    CHECK(phi.Arg(0).Resolve().U1() == false);
    CHECK_FALSE(phi.Arg(0).IsIdentity());
    // The instruction operand is still counted and defined.
    REQUIRE_FALSE(phi.Arg(1).IsImmediate());
    CHECK(phi.Arg(1).Inst()->GetOpcode() == IR::Opcode::GetPred);
    CHECK(phi.Arg(1).Inst()->UseCount() == 1);
}
