// SPDX-License-Identifier: GPL-3.0-or-later

#include <map>
#include <set>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"

namespace {
using namespace Shader;

namespace Op {
constexpr u32 Constant = 43, Variable = 59, Store = 62, LoopMerge = 246, Label = 248;
}
constexpr u32 PrivateStorage = 6;

struct Inst {
    u32 opcode;
    std::vector<u32> words;
    u32 block; ///< Label of the block it is in (0 outside functions)
};

std::vector<Inst> Parse(const std::vector<u32>& code) {
    std::vector<Inst> insts;
    u32 block = 0;
    for (size_t offset = 5; offset < code.size();) {
        const u32 size = code[offset] >> 16;
        REQUIRE(size != 0);
        REQUIRE(offset + size <= code.size());
        const u32 opcode = code[offset] & 0xffffU;
        std::vector<u32> words{code.begin() + offset + 1, code.begin() + offset + size};
        if (opcode == Op::Label) {
            block = words[0];
        }
        insts.push_back({opcode, std::move(words), block});
        offset += size;
    }
    return insts;
}

/// do { do { } while (true); } while (true), laid out as the structurizer lays out two nested
/// loops: each loop has a header block of its own and is left at its merge block.
struct NestedLoops {
    ObjectPool<IR::Inst> pool;
    IR::Block entry{pool}, outer_header{pool}, outer_body{pool}, inner_header{pool},
        inner_body{pool}, inner_continue{pool}, inner_merge{pool}, outer_continue{pool},
        exit{pool};
    IR::Program program;

    NestedLoops() {
        program.stage = Stage::Compute;
        program.workgroup_size = {1, 1, 1};
        program.blocks = {&entry,          &outer_header,   &outer_body,
                          &inner_header,   &inner_body,     &inner_continue,
                          &inner_merge,    &outer_continue, &exit};
        program.post_order_blocks = {program.blocks.rbegin(), program.blocks.rend()};
        for (u32 index = 0; index < program.blocks.size(); ++index) {
            program.blocks[index]->SetOrder(index);
        }
        entry.AddBranch(&outer_header);
        outer_header.AddBranch(&outer_body);
        outer_body.AddBranch(&inner_header);
        inner_header.AddBranch(&inner_body);
        inner_body.AddBranch(&inner_continue);
        inner_continue.AddBranch(&inner_header);
        inner_continue.AddBranch(&inner_merge);
        inner_merge.AddBranch(&outer_continue);
        outer_continue.AddBranch(&outer_header);
        outer_continue.AddBranch(&exit);
        IR::IREmitter ir{entry};
        using Node = IR::AbstractSyntaxNode;
        program.syntax_list = {
            {{.block = &entry}, Node::Type::Block},
            {{.block = &outer_header}, Node::Type::Block},
            {{.loop{&outer_body, &outer_continue, &exit}}, Node::Type::Loop},
            {{.block = &outer_body}, Node::Type::Block},
            {{.block = &inner_header}, Node::Type::Block},
            {{.loop{&inner_body, &inner_continue, &inner_merge}}, Node::Type::Loop},
            {{.block = &inner_body}, Node::Type::Block},
            {{.block = &inner_continue}, Node::Type::Block},
            {{.repeat{ir.Imm1(true), &inner_header, &inner_merge}}, Node::Type::Repeat},
            {{.block = &inner_merge}, Node::Type::Block},
            {{.block = &outer_continue}, Node::Type::Block},
            {{.repeat{ir.Imm1(true), &outer_header, &exit}}, Node::Type::Repeat},
            {{.block = &exit}, Node::Type::Block},
            {{}, Node::Type::Return},
        };
    }
};
} // Anonymous namespace

TEST_CASE("A nested loop's safety counter starts again each time the loop is entered",
          "[shader][loop_safety]") {
    NestedLoops fixture;
    Profile profile;
    profile.supported_spirv = 0x00010400;
    const auto insts = Parse(Backend::SPIRV::EmitSPIRV(profile, fixture.program));

    std::set<u32> limits;
    for (const Inst& inst : insts) {
        if (inst.opcode == Op::Constant && inst.words.size() == 3 && inst.words[2] == 0x2000) {
            limits.insert(inst.words[1]);
        }
    }
    std::set<u32> counters;
    for (const Inst& inst : insts) {
        if (inst.opcode == Op::Variable && inst.words.size() == 4 &&
            inst.words[2] == PrivateStorage && limits.contains(inst.words[3])) {
            counters.insert(inst.words[1]);
        }
    }
    REQUIRE(counters.size() == 2);

    // Each loop's back-edge decrements its counter, the inner loop's first.
    std::vector<u32> decremented;
    std::map<u32, std::set<u32>> resets; ///< Counter -> blocks that set it to the limit again
    std::vector<u32> merges;
    for (const Inst& inst : insts) {
        if (inst.opcode == Op::LoopMerge) {
            merges.push_back(inst.words[0]);
        }
        if (inst.opcode != Op::Store || !counters.contains(inst.words[0])) {
            continue;
        }
        if (limits.contains(inst.words[1])) {
            resets[inst.words[0]].insert(inst.block);
        } else {
            decremented.push_back(inst.words[0]);
        }
    }
    REQUIRE(decremented.size() == 2);
    REQUIRE(merges.size() == 2);

    // Leaving a loop gives its next entry, on the next outer iteration, the whole count.
    CHECK(resets[decremented[0]] == std::set<u32>{merges[1]});
    CHECK(resets[decremented[1]] == std::set<u32>{merges[0]});
}
