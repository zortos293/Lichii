// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Demote reordering (HostTranslateInfo::needs_demote_reorder, AMD drivers) moves the demotes it
// finds at most one selection deep to the end of a fragment program, OR-ing their conditions.
// A demote inside a guest loop must not move: its condition is the value of one iteration, and
// a break before it can leave the loop on a path that never computed it. Nor must a demote with
// code beside it in its selection: only a demote alone in its selection moves.
// EDEN_DEMOTE_REORDER_TEST_OUTPUT=<dir> writes the modules for spirv-val.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/profile.h"

namespace {
using namespace Shader;

u32 Address(u32 index) {
    return 0x50 + index * 8 + ((index + 1) / 3) * 8;
}

/// Guest program with named labels; Branch() takes the label it jumps to.
struct Assembler {
    struct Insn {
        u64 word;
        std::string target;
    };
    std::vector<Insn> insns;
    std::map<std::string, u32> labels;

    void Label(const std::string& name) {
        labels[name] = static_cast<u32>(insns.size());
    }
    void Emit(u64 word) {
        insns.push_back({word, {}});
    }
    /// R[dest] = c0[offset], 32 or 64 bits
    void LoadConst(u32 dest, u32 offset, bool wide) {
        Emit(0xef90000000070000ULL | dest | (u64{255} << 8) | (u64{offset} << 20) |
             (u64{wide ? 5U : 4U} << 48));
    }
    /// R[dest] = R[src] + imm
    void AddImm(u32 dest, u32 src, u32 imm) {
        Emit(0x1c00000000070000ULL | dest | (u64{src} << 8) | (u64{imm} << 20));
    }
    /// [R2:R3 + offset] = R[data], 32 bits
    void Store(u32 data, u32 offset) {
        Emit(0xeed8000000070000ULL | data | (u64{2} << 8) | (u64{1} << 45) | (u64{4} << 48) |
             (u64{offset} << 20));
    }
    /// P[pred] = R[src] (compare_op 2 ==, 4 >) imm
    void SetPred(u32 pred, u32 src, u32 compare_op, u32 imm, bool is_signed = false) {
        Emit(0x3660000000070007ULL | (u64{compare_op} << 49) | (u64{is_signed} << 48) |
             (u64{7} << 39) | (u64{imm} << 20) | (u64{src} << 8) | (u64{pred} << 3));
    }
    /// @P[pred] BRA target
    void Branch(u32 pred, const std::string& target) {
        insns.push_back({0xe24000000000000fULL | (u64{pred} << 16), target});
    }
    /// R[dest] = R[src] + imm when P[pred]
    void AddImmIf(u32 pred, u32 dest, u32 src, u32 imm) {
        Emit((0x1c00000000070000ULL & ~(u64{0xf} << 16)) | (u64{pred} << 16) | dest |
             (u64{src} << 8) | (u64{imm} << 20));
    }
    /// @P[pred] KIL
    void Kill(u32 pred) {
        Emit(0xe33000000000000fULL | (u64{pred} << 16));
    }
    void Exit() {
        Emit(0xe30000000007000fULL);
    }
    /// @P[pred] EXIT
    void ExitIf(u32 pred) {
        Emit(0xe30000000000000fULL | (u64{pred} << 16));
    }
    std::vector<u64> Code() const {
        std::vector<u64> code;
        for (u32 i = 0; i < insns.size(); ++i) {
            u64 word = insns[i].word;
            if (!insns[i].target.empty()) {
                const s32 offset = static_cast<s32>(Address(labels.at(insns[i].target))) -
                                   static_cast<s32>(Address(i) + 8);
                word |= (static_cast<u64>(static_cast<u32>(offset)) & 0xffffff) << 20;
            }
            code.push_back(word);
        }
        return code;
    }
};

/// A fragment program with a guest loop of three iterations: R0 = input + iteration; with
/// `with_break`, the loop is left when R0 == 7, before the predicated KIL on R0 == 5.
std::vector<u64> LoopWithKill(bool with_break) {
    Assembler a;
    a.LoadConst(0, 0, false);
    a.LoadConst(2, 0x310, true);
    a.AddImm(20, 255, 3);
    a.Label("head");
    a.AddImm(0, 0, 1);
    if (with_break) {
        a.SetPred(0, 0, 2, 7);
        a.Branch(0, "after");
    }
    a.SetPred(1, 0, 2, 5);
    a.Kill(1);
    a.AddImm(20, 20, 0xffffffff);
    a.SetPred(2, 20, 4, 0, true);
    a.Branch(2, "head");
    a.Label("after");
    a.Store(0, 0);
    a.Exit();
    return a.Code();
}

/// A fragment program without a loop: KIL on R0 == 5, then a store.
std::vector<u64> KillWithoutLoop() {
    Assembler a;
    a.LoadConst(0, 0, false);
    a.LoadConst(2, 0x310, true);
    a.SetPred(1, 0, 2, 5);
    a.Kill(1);
    a.Store(0, 0);
    a.Exit();
    return a.Code();
}

/// A guest loop with a KIL on R0 == 5 in it, then after the loop (R0 = input + 3) a KIL on
/// R0 == 9: the second one is outside the loop and still moves to the end; input 6 discards only
/// through it.
std::vector<u64> LoopThenKill() {
    Assembler a;
    a.LoadConst(0, 0, false);
    a.LoadConst(2, 0x310, true);
    a.AddImm(20, 255, 3);
    a.Label("head");
    a.AddImm(0, 0, 1);
    a.SetPred(1, 0, 2, 5);
    a.Kill(1);
    a.AddImm(20, 20, 0xffffffff);
    a.SetPred(2, 20, 4, 0, true);
    a.Branch(2, "head");
    a.SetPred(1, 0, 2, 9);
    a.Kill(1);
    a.Store(0, 0);
    a.Exit();
    return a.Code();
}

/// Two KILs: one on R0 == 7, then, on every other input, one that is not the first block of a
/// selection: it follows a predicated instruction (`predicated`) or a loop (otherwise) inside
/// the branch around it. Every input discards, input 7 through the first KIL.
std::vector<u64> TwoKills(bool predicated) {
    Assembler a;
    a.LoadConst(0, 0, false);
    a.LoadConst(2, 0x310, true);
    a.SetPred(1, 0, 2, 7);
    a.Kill(1);
    a.Branch(1, "skip");
    if (predicated) {
        a.SetPred(2, 0, 2, 2);
        a.AddImmIf(2, 0, 0, 1);
    } else {
        a.AddImm(20, 255, 2);
        a.Label("head");
        a.AddImm(20, 20, 0xffffffff);
        a.SetPred(2, 20, 4, 0, true);
        a.Branch(2, "head");
    }
    a.Kill(7);
    a.Label("skip");
    a.Store(0, 0);
    a.Exit();
    return a.Code();
}

/// A program whose only EXIT, on R0 == 3, comes before a KIL on R0 == 5 (after a guest loop with
/// a KIL in it, with `loop`): the other inputs end in a branch to itself, so no EXIT follows the
/// KIL.
std::vector<u64> ExitBeforeKill(bool loop) {
    Assembler a;
    a.LoadConst(0, 0, false);
    a.LoadConst(2, 0x310, true);
    a.SetPred(0, 0, 2, 3);
    a.ExitIf(0);
    if (loop) {
        a.AddImm(20, 255, 2);
        a.Label("head");
        a.Kill(7);
        a.AddImm(20, 20, 0xffffffff);
        a.SetPred(2, 20, 4, 0, true);
        a.Branch(2, "head");
    }
    a.SetPred(1, 0, 2, 5);
    a.Kill(1);
    a.Store(0, 0);
    a.Label("tail");
    a.Branch(7, "tail");
    return a.Code();
}

/// A KIL on R0 == 5, a store, then a branch to itself: the program has no EXIT at all.
std::vector<u64> KillWithoutExit() {
    Assembler a;
    a.LoadConst(0, 0, false);
    a.LoadConst(2, 0x310, true);
    a.SetPred(1, 0, 2, 5);
    a.Kill(1);
    a.Store(0, 0);
    a.Label("tail");
    a.Branch(7, "tail");
    return a.Code();
}

/// A KIL inside the branch that inputs other than 3 take, with code beside it in that branch:
/// after the KIL (`after`), before it in its block (`before`), or both. Each piece adds to R0 and
/// stores it; the store after the branch runs on every input.
std::vector<u64> KillWithCode(bool before, bool after) {
    Assembler a;
    a.LoadConst(0, 0, false);
    a.LoadConst(2, 0x310, true);
    a.SetPred(0, 0, 2, 3);
    a.Branch(0, "skip");
    if (before) {
        a.AddImm(0, 0, 7);
        a.Store(0, 0x20);
    }
    a.Kill(7);
    if (after) {
        a.AddImm(0, 0, 5);
        a.Store(0, 0x10);
    }
    a.Label("skip");
    a.AddImm(0, 0, 1);
    a.Store(0, 0xff0);
    a.Exit();
    return a.Code();
}

/// The stores (offset, value) the guest program above makes for `input`, in order.
std::vector<std::pair<u32, u32>> KillWithCodeStores(bool before, bool after, u32 input) {
    std::vector<std::pair<u32, u32>> stores;
    u32 r0 = input;
    if (r0 != 3) {
        if (before) {
            r0 += 7;
            stores.emplace_back(0x20, r0);
        }
        if (after) {
            r0 += 5;
            stores.emplace_back(0x10, r0);
        }
    }
    stores.emplace_back(0xff0, r0 + 1);
    return stores;
}

/// Stores as text, so a failed check shows them.
std::string Show(const std::vector<std::pair<u32, u32>>& stores) {
    std::string text;
    for (const auto& [offset, value] : stores) {
        text += fmt::format(" [{:#x}]={}", offset, value);
    }
    return text;
}

/// A KIL on R0 == 3 alone in its selection, then the branch of KillWithCode with code after its
/// KIL: the first demote moves to the end, the second stays where it is. Every input discards,
/// input 3 only through the demote that moves.
std::vector<u64> KillThenKillWithCode() {
    Assembler a;
    a.LoadConst(0, 0, false);
    a.LoadConst(2, 0x310, true);
    a.SetPred(1, 0, 2, 3);
    a.Kill(1);
    a.SetPred(0, 0, 2, 3);
    a.Branch(0, "skip");
    a.Kill(7);
    a.AddImm(0, 0, 5);
    a.Store(0, 0x10);
    a.Label("skip");
    a.AddImm(0, 0, 1);
    a.Store(0, 0xff0);
    a.Exit();
    return a.Code();
}

/// Whether LoopWithKill(with_break) discards for `input`.
bool ReferenceDiscards(bool with_break, u32 input) {
    u32 r0 = input;
    for (u32 iteration = 0; iteration < 3; ++iteration) {
        ++r0;
        if (with_break && r0 == 7) {
            return false;
        }
        if (r0 == 5) {
            return true;
        }
    }
    return false;
}

class FragmentEnvironment final : public Environment {
public:
    explicit FragmentEnvironment(std::span<const u64> code_) : code{code_} {
        stage = Stage::Fragment;
    }
    u64 ReadInstruction(u32 address) override {
        for (u32 index = 0; index < code.size(); ++index) {
            if (address == Address(index)) {
                return code[index];
            }
        }
        FAIL("Read outside shader fixture at " << address);
        return 0;
    }
    u32 ReadCbufValue(u32, u32) override {
        return 0;
    }
    TextureType ReadTextureType(u32) override {
        return TextureType::Color2D;
    }
    TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return false;
    }
    u32 ReadViewportTransformState() override {
        return 1;
    }
    u32 TextureBoundBuffer() const override {
        return 0;
    }
    u32 LocalMemorySize() const override {
        return 0;
    }
    u32 SharedMemorySize() const override {
        return 0;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {1, 1, 1};
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    bool HasNativeInstructions() const override {
        return false;
    }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    std::span<const u64> code;
};

IR::Program Translate(ObjectPool<IR::Inst>& inst_pool, ObjectPool<IR::Block>& block_pool,
                      std::span<const u64> code, bool demote_reorder) {
    FragmentEnvironment env{code};
    ObjectPool<Maxwell::Flow::Block> flow_pool;
    Maxwell::Flow::CFG cfg{env, flow_pool, Maxwell::Location{0x50}};
    HostTranslateInfo host{};
    host.min_ssbo_alignment = 16;
    host.support_int64 = true;
    host.needs_demote_reorder = demote_reorder;
    return Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host);
}

std::vector<u32> Emit(IR::Program& program, const std::string& name) {
    Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.min_ssbo_alignment = 16;
    profile.support_int64 = true; // as the host info in Translate
    profile.support_demote_to_helper_invocation = true;
    std::vector<u32> spirv = Backend::SPIRV::EmitSPIRV(profile, program);
    if (const char* const directory = std::getenv("EDEN_DEMOTE_REORDER_TEST_OUTPUT")) {
        std::filesystem::create_directories(directory);
        const std::filesystem::path path{std::filesystem::path{directory} / (name + ".spv")};
        std::ofstream file{path, std::ios::binary};
        file.write(reinterpret_cast<const char*>(spirv.data()),
                   static_cast<std::streamsize>(spirv.size() * sizeof(u32)));
        file.close();
        INFO("writing " << path.string());
        REQUIRE(file.good());
    }
    return spirv;
}

/// Operands defined in a block that does not dominate the block using them (phi operands are
/// checked against the predecessor they come from). SPIR-V requires every definition to
/// dominate its uses.
std::vector<std::string> DominanceProblems(const IR::Program& program) {
    const auto& blocks{program.blocks};
    std::unordered_map<const IR::Block*, std::unordered_set<const IR::Block*>> dominators;
    for (const IR::Block* const block : blocks) {
        dominators[block] =
            block == blocks.front()
                ? std::unordered_set<const IR::Block*>{block}
                : std::unordered_set<const IR::Block*>{blocks.begin(), blocks.end()};
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (const IR::Block* const block : blocks) {
            if (block == blocks.front()) {
                continue;
            }
            std::optional<std::unordered_set<const IR::Block*>> meet;
            for (const IR::Block* const predecessor : block->ImmPredecessors()) {
                if (!dominators.contains(predecessor)) {
                    continue; // dropped as unreachable
                }
                const auto& set{dominators.at(predecessor)};
                if (!meet) {
                    meet = set;
                } else {
                    std::erase_if(*meet, [&](const IR::Block* b) { return !set.contains(b); });
                }
            }
            std::unordered_set<const IR::Block*> result{
                meet.value_or(std::unordered_set<const IR::Block*>{})};
            result.insert(block);
            if (result != dominators.at(block)) {
                dominators[block] = std::move(result);
                changed = true;
            }
        }
    }
    std::unordered_map<const IR::Inst*, const IR::Block*> block_of;
    for (const IR::Block* const block : blocks) {
        for (const IR::Inst& inst : *block) {
            block_of.emplace(&inst, block);
        }
    }
    std::vector<std::string> problems;
    for (const IR::Block* const block : blocks) {
        for (const IR::Inst& inst : *block) {
            for (size_t index = 0; index < inst.NumArgs(); ++index) {
                const IR::Value arg{inst.Arg(index).Resolve()};
                if (arg.IsImmediate() || arg.IsEmpty()) {
                    continue;
                }
                const auto definition{block_of.find(arg.InstRecursive())};
                const IR::Block* const use{
                    inst.GetOpcode() == IR::Opcode::Phi ? inst.PhiBlock(index) : block};
                if (definition == block_of.end() || !dominators.contains(use)) {
                    continue; // a block translation dropped as unreachable
                }
                if (!dominators.at(use).contains(definition->second)) {
                    problems.push_back(fmt::format("{} uses a value from a block that does not "
                                                   "dominate it",
                                                   inst.GetOpcode()));
                }
            }
        }
    }
    return problems;
}

/// Runs a fragment program on the CPU following its syntax list and returns whether the
/// invocation ends demoted. c0[0] is the input and c0[0x310] the global pointer, 0x10000;
/// stores are not evaluated. Unknown instructions throw, and so does a condition on an undefined
/// value. Values stay from earlier loop iterations, so a read of a value computed in an earlier
/// iteration but not on the current path is not caught; one never computed at all throws.
/// With `stores`, storage buffer writes are recorded there as (offset, value).
bool Discards(const IR::Program& program, u32 input,
              std::vector<std::pair<u32, u32>>* stores = nullptr) {
    using IR::Opcode;
    using Type = IR::AbstractSyntaxNode::Type;
    struct Val {
        u64 word{};
        bool undef{};
    };
    const auto cbuf = [&](u64 offset) -> u32 {
        return offset == 0 ? input : offset == 0x310 ? 0x10000U : 0U;
    };
    std::unordered_map<const IR::Inst*, Val> values;
    const auto read = [&](const IR::Value& value) -> Val {
        const IR::Value resolved{value.Resolve()};
        if (resolved.IsEmpty()) {
            return {};
        }
        if (!resolved.IsImmediate()) {
            return values.at(resolved.InstRecursive());
        }
        switch (resolved.Type()) {
        case IR::Type::U1:
            return {resolved.U1()};
        case IR::Type::U32:
            return {resolved.U32()};
        case IR::Type::U64:
            return {resolved.U64()};
        default:
            throw std::runtime_error("immediate type");
        }
    };
    const auto condition = [&](const IR::Value& value) {
        const Val v{read(value)};
        if (v.undef) {
            throw std::runtime_error("branch on an undefined value");
        }
        return v.word != 0;
    };
    bool demoted{};
    std::unordered_map<const IR::Block*, size_t> node_of;
    const auto& list{program.syntax_list};
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].type == Type::Block) {
            node_of.emplace(list[i].data.block, i);
        }
    }
    const IR::Block* previous{};
    for (size_t at = 0, steps = 0; at < list.size(); ++steps) {
        if (steps > 10000) {
            throw std::runtime_error("no termination");
        }
        const IR::AbstractSyntaxNode& node{list[at]};
        switch (node.type) {
        case Type::Block: {
            std::vector<std::pair<const IR::Inst*, Val>> phis;
            for (const IR::Inst& inst : *node.data.block) {
                if (inst.GetOpcode() != Opcode::Phi) {
                    continue;
                }
                std::optional<Val> value;
                for (size_t index = 0; index < inst.NumArgs(); ++index) {
                    if (inst.PhiBlock(index) == previous) {
                        value = read(inst.Arg(index));
                    }
                }
                if (!value) {
                    throw std::runtime_error("phi without the predecessor");
                }
                phis.emplace_back(&inst, *value);
            }
            for (const auto& [inst, value] : phis) {
                values[inst] = value;
            }
            for (const IR::Inst& inst : *node.data.block) {
                const Opcode op{inst.GetOpcode()};
                if (op == Opcode::Phi) {
                    continue;
                }
                std::array<Val, 3> args{};
                bool undef{};
                for (size_t index = 0; index < std::min<size_t>(inst.NumArgs(), 3); ++index) {
                    args[index] = read(inst.Arg(index));
                    undef |= args[index].undef;
                }
                const auto arg = [&](size_t index) { return args[index].word; };
                Val result{};
                switch (op) {
                case Opcode::Identity:
                case Opcode::ConditionRef:
                    result = args[0];
                    break;
                case Opcode::WriteStorage32:
                    if (stores) {
                        stores->emplace_back(static_cast<u32>(arg(1)), static_cast<u32>(arg(2)));
                    }
                    break;
                case Opcode::Prologue:
                case Opcode::Epilogue:
                case Opcode::Void:
                case Opcode::WriteGlobal32:
                    break;
                case Opcode::UndefU1:
                case Opcode::UndefU32:
                    result.undef = true;
                    break;
                case Opcode::DemoteToHelperInvocation:
                    demoted = true;
                    break;
                case Opcode::GetCbufU32:
                    result.word = cbuf(arg(1));
                    break;
                case Opcode::GetCbufU32x2: // the two words packed, the first in the low half
                    result.word = cbuf(arg(1)) | (u64{cbuf(arg(1) + 4)} << 32);
                    break;
                case Opcode::CompositeExtractU32x2:
                    result.word = (arg(0) >> (32 * arg(1))) & 0xffffffff;
                    break;
                case Opcode::BitwiseAnd32:
                    result.word = arg(0) & arg(1);
                    break;
                case Opcode::ISub32:
                    result.word = static_cast<u32>(arg(0) - arg(1));
                    break;
                case Opcode::IAdd32:
                    result.word = static_cast<u32>(arg(0) + arg(1));
                    break;
                case Opcode::IEqual:
                    result.word = static_cast<u32>(arg(0)) == static_cast<u32>(arg(1));
                    break;
                case Opcode::SGreaterThan:
                    result.word = static_cast<s32>(arg(0)) > static_cast<s32>(arg(1));
                    break;
                case Opcode::LogicalNot:
                    result.word = !arg(0);
                    break;
                case Opcode::LogicalOr:
                    result.word = arg(0) || arg(1);
                    break;
                case Opcode::LogicalAnd:
                    result.word = arg(0) && arg(1);
                    break;
                default:
                    throw std::runtime_error(fmt::format("opcode {}", op));
                }
                result.undef |= undef;
                values[&inst] = result;
            }
            previous = node.data.block;
            ++at;
            break;
        }
        case Type::If:
            at = condition(node.data.if_node.cond) ? at + 1 : node_of.at(node.data.if_node.merge);
            break;
        case Type::EndIf:
            at = node_of.at(node.data.end_if.merge);
            break;
        case Type::Loop:
            ++at;
            break;
        case Type::Break:
            at = condition(node.data.break_node.cond) ? node_of.at(node.data.break_node.merge)
                                                      : node_of.at(node.data.break_node.skip);
            break;
        case Type::Repeat:
            at = condition(node.data.repeat.cond) ? node_of.at(node.data.repeat.loop_header)
                                                  : node_of.at(node.data.repeat.merge);
            break;
        case Type::Return:
            return demoted;
        default:
            throw std::runtime_error("syntax node");
        }
    }
    return demoted;
}
} // namespace

TEST_CASE("Demote reordering leaves a demote in a loop with a break before it",
          "[shader][demote_reorder]") {
    for (const bool reorder : {false, true}) {
        INFO("reorder " << reorder);
        ObjectPool<IR::Inst> inst_pool;
        ObjectPool<IR::Block> block_pool;
        const auto code = LoopWithKill(true);
        IR::Program program = Translate(inst_pool, block_pool, code, reorder);
        CHECK(DominanceProblems(program).empty());
        for (u32 input = 0; input < 12; ++input) {
            INFO("input " << input);
            CHECK(Discards(program, input) == ReferenceDiscards(true, input));
        }
        Emit(program, reorder ? "break_reorder" : "break");
    }
}

TEST_CASE("Demote reordering leaves a demote that an earlier iteration may reach",
          "[shader][demote_reorder]") {
    for (const bool reorder : {false, true}) {
        INFO("reorder " << reorder);
        ObjectPool<IR::Inst> inst_pool;
        ObjectPool<IR::Block> block_pool;
        const auto code = LoopWithKill(false);
        IR::Program program = Translate(inst_pool, block_pool, code, reorder);
        CHECK(DominanceProblems(program).empty());
        for (u32 input = 0; input < 12; ++input) {
            INFO("input " << input);
            CHECK(Discards(program, input) == ReferenceDiscards(false, input));
        }
        Emit(program, reorder ? "loop_reorder" : "loop");
    }
}

TEST_CASE("Demote reordering still moves a demote outside loops to the end",
          "[shader][demote_reorder]") {
    ObjectPool<IR::Inst> inst_pool;
    ObjectPool<IR::Block> block_pool;
    const auto code = KillWithoutLoop();
    IR::Program program = Translate(inst_pool, block_pool, code, true);
    CHECK(DominanceProblems(program).empty());
    // The demote now comes after the store, at the end of the program
    std::optional<size_t> demote_node, store_node;
    const auto& list{program.syntax_list};
    for (size_t index = 0; index < list.size(); ++index) {
        if (list[index].type != IR::AbstractSyntaxNode::Type::Block) {
            continue;
        }
        for (const IR::Inst& inst : *list[index].data.block) {
            if (inst.GetOpcode() == IR::Opcode::DemoteToHelperInvocation) {
                demote_node = index;
            } else if (inst.GetOpcode() == IR::Opcode::WriteStorage32 ||
                       inst.GetOpcode() == IR::Opcode::WriteGlobal32) {
                store_node = index;
            }
        }
    }
    REQUIRE(demote_node);
    REQUIRE(store_node);
    CHECK(*demote_node > *store_node);
    for (u32 input = 0; input < 8; ++input) {
        INFO("input " << input);
        CHECK(Discards(program, input) == (input == 5));
    }
}

TEST_CASE("Demote reordering moves a demote after a loop and leaves the one inside it",
          "[shader][demote_reorder]") {
    ObjectPool<IR::Inst> inst_pool;
    ObjectPool<IR::Block> block_pool;
    const auto code = LoopThenKill();
    IR::Program program = Translate(inst_pool, block_pool, code, true);
    CHECK(DominanceProblems(program).empty());
    // In syntax list order: the loop's demote, the store, then the moved demote
    std::vector<std::pair<size_t, bool>> demotes; // node, inside a loop
    std::optional<size_t> store_node;
    u32 loops{};
    const auto& list{program.syntax_list};
    for (size_t index = 0; index < list.size(); ++index) {
        loops += list[index].type == IR::AbstractSyntaxNode::Type::Loop;
        loops -= list[index].type == IR::AbstractSyntaxNode::Type::Repeat;
        if (list[index].type != IR::AbstractSyntaxNode::Type::Block) {
            continue;
        }
        for (const IR::Inst& inst : *list[index].data.block) {
            if (inst.GetOpcode() == IR::Opcode::DemoteToHelperInvocation) {
                demotes.emplace_back(index, loops > 0);
            } else if (inst.GetOpcode() == IR::Opcode::WriteStorage32 ||
                       inst.GetOpcode() == IR::Opcode::WriteGlobal32) {
                store_node = index;
            }
        }
    }
    REQUIRE(demotes.size() == 2);
    REQUIRE(store_node);
    CHECK(demotes[0].second);
    CHECK(!demotes[1].second);
    CHECK(demotes[1].first > *store_node);
    for (u32 input = 0; input < 8; ++input) {
        INFO("input " << input);
        u32 r0 = input;
        bool discards{};
        for (u32 iteration = 0; iteration < 3; ++iteration) {
            discards |= ++r0 == 5;
        }
        CHECK(Discards(program, input) == (discards || r0 == 9));
    }
}

TEST_CASE("Demote reordering keeps a demote it cannot move along with one it could",
          "[shader][demote_reorder]") {
    for (const bool predicated : {true, false}) {
        for (const bool reorder : {false, true}) {
            INFO("predicated " << predicated << " reorder " << reorder);
            ObjectPool<IR::Inst> inst_pool;
            ObjectPool<IR::Block> block_pool;
            const auto code = TwoKills(predicated);
            IR::Program program = Translate(inst_pool, block_pool, code, reorder);
            CHECK(DominanceProblems(program).empty());
            for (u32 input = 0; input < 10; ++input) {
                INFO("input " << input);
                CHECK(Discards(program, input));
            }
        }
    }
}

TEST_CASE("Demote reordering leaves a demote that comes after the only return",
          "[shader][demote_reorder]") {
    for (const bool loop : {false, true}) {
        for (const bool reorder : {false, true}) {
            INFO("loop " << loop << " reorder " << reorder);
            ObjectPool<IR::Inst> inst_pool;
            ObjectPool<IR::Block> block_pool;
            const auto code = ExitBeforeKill(loop);
            IR::Program program = Translate(inst_pool, block_pool, code, reorder);
            CHECK(DominanceProblems(program).empty());
            // The epilogue block comes before the demote outside the loop, which stays there
            std::optional<size_t> epilogue_node, outer_demote_node;
            u32 loops{};
            const auto& list{program.syntax_list};
            for (size_t index = 0; index < list.size(); ++index) {
                loops += list[index].type == IR::AbstractSyntaxNode::Type::Loop;
                loops -= list[index].type == IR::AbstractSyntaxNode::Type::Repeat;
                if (list[index].type != IR::AbstractSyntaxNode::Type::Block) {
                    continue;
                }
                for (const IR::Inst& inst : *list[index].data.block) {
                    if (inst.GetOpcode() == IR::Opcode::Epilogue) {
                        epilogue_node = index;
                    } else if (inst.GetOpcode() == IR::Opcode::DemoteToHelperInvocation &&
                               loops == 0) {
                        outer_demote_node = index;
                    }
                }
            }
            REQUIRE(epilogue_node);
            REQUIRE(outer_demote_node);
            CHECK(*outer_demote_node > *epilogue_node);
            // Input 3 leaves through the EXIT before any KIL
            CHECK_FALSE(Discards(program, 3));
            CHECK_NOTHROW(Emit(program, fmt::format("only_return{}{}", loop ? "_loop" : "",
                                                    reorder ? "_reorder" : "")));
        }
    }
}

TEST_CASE("Demote reordering leaves a demote in a program without a return",
          "[shader][demote_reorder]") {
    ObjectPool<IR::Inst> inst_pool;
    ObjectPool<IR::Block> block_pool;
    const auto code = KillWithoutExit();
    IR::Program program = Translate(inst_pool, block_pool, code, true);
    CHECK(DominanceProblems(program).empty());
    // The demote stays before the store
    std::optional<size_t> demote_node, store_node;
    const auto& list{program.syntax_list};
    for (size_t index = 0; index < list.size(); ++index) {
        if (list[index].type != IR::AbstractSyntaxNode::Type::Block) {
            continue;
        }
        for (const IR::Inst& inst : *list[index].data.block) {
            if (inst.GetOpcode() == IR::Opcode::DemoteToHelperInvocation) {
                demote_node = index;
            } else if (inst.GetOpcode() == IR::Opcode::WriteStorage32 ||
                       inst.GetOpcode() == IR::Opcode::WriteGlobal32) {
                store_node = index;
            }
        }
    }
    REQUIRE(demote_node);
    REQUIRE(store_node);
    CHECK(*demote_node < *store_node);
    CHECK_NOTHROW(Emit(program, "no_return_reorder"));
}

TEST_CASE("Demote reordering leaves a demote with code beside it in its selection",
          "[shader][demote_reorder]") {
    // Moving the demote's block would run the code after the KIL on inputs that skip the
    // branch, and moving it after the code that follows would read the values the code before
    // the KIL computes before computing them
    for (const auto& [before, after] : {std::pair{false, true}, {true, false}, {true, true}}) {
        for (const bool reorder : {false, true}) {
            INFO("before " << before << " after " << after << " reorder " << reorder);
            ObjectPool<IR::Inst> inst_pool;
            ObjectPool<IR::Block> block_pool;
            const auto code = KillWithCode(before, after);
            IR::Program program = Translate(inst_pool, block_pool, code, reorder);
            CHECK(DominanceProblems(program).empty());
            for (u32 input = 2; input < 6; ++input) {
                INFO("input " << input);
                std::vector<std::pair<u32, u32>> stores;
                bool discards{};
                REQUIRE_NOTHROW(discards = Discards(program, input, &stores));
                CHECK(discards == (input != 3));
                CHECK(Show(stores) == Show(KillWithCodeStores(before, after, input)));
            }
            CHECK_NOTHROW(Emit(program, fmt::format("code_beside{}{}{}", before ? "_before" : "",
                                                    after ? "_after" : "",
                                                    reorder ? "_reorder" : "")));
        }
    }
}

TEST_CASE("Demote reordering moves a demote alone in its selection next to one it leaves",
          "[shader][demote_reorder]") {
    ObjectPool<IR::Inst> inst_pool;
    ObjectPool<IR::Block> block_pool;
    const auto code = KillThenKillWithCode();
    IR::Program program = Translate(inst_pool, block_pool, code, true);
    CHECK(DominanceProblems(program).empty());
    // In syntax list order: the demote left with its code, the last store, then the moved one
    std::vector<size_t> demote_nodes;
    std::optional<size_t> last_store_node;
    const auto& list{program.syntax_list};
    for (size_t index = 0; index < list.size(); ++index) {
        if (list[index].type != IR::AbstractSyntaxNode::Type::Block) {
            continue;
        }
        for (const IR::Inst& inst : *list[index].data.block) {
            if (inst.GetOpcode() == IR::Opcode::DemoteToHelperInvocation) {
                demote_nodes.push_back(index);
            } else if (inst.GetOpcode() == IR::Opcode::WriteStorage32) {
                last_store_node = index;
            }
        }
    }
    REQUIRE(demote_nodes.size() == 2);
    REQUIRE(last_store_node);
    CHECK(demote_nodes[0] < *last_store_node);
    CHECK(demote_nodes[1] > *last_store_node);
    for (u32 input = 2; input < 7; ++input) {
        INFO("input " << input);
        std::vector<std::pair<u32, u32>> stores;
        bool discards{};
        REQUIRE_NOTHROW(discards = Discards(program, input, &stores));
        CHECK(discards);
        std::vector<std::pair<u32, u32>> expected;
        if (input != 3) {
            expected.emplace_back(0x10, input + 5);
        }
        expected.emplace_back(0xff0, input + (input != 3 ? 6 : 1));
        CHECK(Show(stores) == Show(expected));
    }
    CHECK_NOTHROW(Emit(program, "alone_and_beside_reorder"));
}
