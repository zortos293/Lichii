// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The x86-64 decoder code images use to find relocations (backend/x64/x64_decode.h), against
// what Xbyak encodes: every instruction's length, and the position and target of its
// position-dependent field.

#include "dynarmic/backend/x64/x64_decode.h"

#include <cstring>
#include <functional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "dynarmic/backend/x64/xbyak.h"
#include "dynarmic/common/hash.h"

using namespace Dynarmic::Backend::X64;
using namespace Xbyak::util;

namespace {

struct Expected {
    size_t begin;
    size_t end;
    X64FieldKind field;
    const void* target;  // Rel*/RipDisp32: where it points; Imm64: the value
};

class Gen final : public Xbyak::CodeGenerator {
public:
    Gen()
            : Xbyak::CodeGenerator(4096) {}

    std::vector<Expected> expected;

    void Add(const std::function<void()>& emit, X64FieldKind field = X64FieldKind::None, const void* target = nullptr) {
        const size_t begin = getSize();
        emit();
        expected.push_back({begin, getSize(), field, target});
    }
};

}  // namespace

TEST_CASE("x64 decoder: lengths and fields of emitted code", "[x64][code_image]") {
    Gen g;
    // A target outside the instructions but within rel32 reach: the end of the buffer.
    const void* const far_target = g.getCode() + 4000;
    using K = X64FieldKind;

    // Plain integer code.
    g.Add([&] { g.mov(rax, 0x1122334455667788); }, K::Imm64, reinterpret_cast<const void*>(0x1122334455667788));
    g.Add([&] { g.mov(r11, 0x8000000000000000); }, K::Imm64, reinterpret_cast<const void*>(0x8000000000000000));
    g.Add([&] { g.mov(eax, 0x12345678); });
    g.Add([&] { g.mov(rcx, 5); });
    g.Add([&] { g.mov(word[r13 + rax], 0x1234); });
    g.Add([&] { g.mov(dword[r15 + 0x108], 0x12345678); });
    g.Add([&] { g.mov(byte[rsp + rax * 8 + 0x100], 7); });
    g.Add([&] { g.add(rax, 0x12345678); });
    g.Add([&] { g.add(ax, 0x1234); });
    g.Add([&] { g.add(eax, 1); });
    g.Add([&] { g.and_(r12d, 0xFFF0); });
    g.Add([&] { g.cmp(dword[r15 + 0x40], 0); });
    g.Add([&] { g.test(rax, 0x100); });
    g.Add([&] { g.test(byte[rcx], 1); });
    g.Add([&] { g.test(word[rcx], 0x8000); });
    g.Add([&] { g.not_(qword[rcx + 8]); });
    g.Add([&] { g.neg(r9); });
    g.Add([&] { g.imul(eax, ecx, 0x1000); });
    g.Add([&] { g.imul(eax, ecx, 3); });
    g.Add([&] { g.lea(rbp, ptr[rsp + 0x28]); });
    g.Add([&] { g.lea(rax, ptr[rax + rcx * 4 + 0x12345]); });
    g.Add([&] { g.lea(rax, ptr[rcx * 8]); });  // SIB without base: disp32
    g.Add([&] { g.movzx(eax, byte[rdx]); });
    g.Add([&] { g.movsxd(rax, ecx); });
    g.Add([&] { g.cmovb(rsp, qword[r15 + 0x200]); });
    g.Add([&] { g.setne(al); });
    g.Add([&] { g.seto(byte[rsp]); });
    g.Add([&] { g.bt(eax, 3); });
    g.Add([&] { g.bts(qword[rcx], rax); });
    g.Add([&] { g.shld(rax, rcx, 7); });
    g.Add([&] { g.shrd(rax, rcx, cl); });
    g.Add([&] { g.shl(rax, 32); });
    g.Add([&] { g.sar(ecx, 1); });
    g.Add([&] { g.ror(edx, cl); });
    g.Add([&] { g.xchg(qword[rcx], rax); });
    g.Add([&] { g.lock(); g.or_(dword[rsp], 0); });
    g.Add([&] { g.lock(); g.cmpxchg(qword[r13 + rcx], rdx); });
    g.Add([&] { g.lock(); g.xadd(dword[rcx], eax); });
    g.Add([&] { g.popcnt(rax, rcx); });
    g.Add([&] { g.lzcnt(eax, ecx); });
    g.Add([&] { g.tzcnt(rax, qword[rcx]); });
    g.Add([&] { g.bsr(eax, ecx); });
    g.Add([&] { g.bswap(rax); });
    g.Add([&] { g.crc32(r12, r8); });
    g.Add([&] { g.crc32(eax, byte[rcx]); });
    g.Add([&] { g.pdep(eax, eax, ecx); });
    g.Add([&] { g.pext(rax, rcx, qword[rdx]); });
    g.Add([&] { g.shlx(rax, rcx, rdx); });
    g.Add([&] { g.rorx(eax, ecx, 5); });
    g.Add([&] { g.andn(rax, rcx, rdx); });
    g.Add([&] { g.mul(qword[rcx]); });
    g.Add([&] { g.div(ecx); });
    g.Add([&] { g.cqo(); });
    g.Add([&] { g.cdq(); });
    g.Add([&] { g.cmc(); });
    g.Add([&] { g.lahf(); });
    g.Add([&] { g.sahf(); });
    g.Add([&] { g.pushf(); });
    g.Add([&] { g.popf(); });
    g.Add([&] { g.push(rbx); });
    g.Add([&] { g.pop(r12); });
    g.Add([&] { g.push(0x12345678); });
    g.Add([&] { g.ret(); });
    g.Add([&] { g.ret(16); });
    g.Add([&] { g.db(0xCC); });
    g.Add([&] { g.ud2(); });
    g.Add([&] { g.rdtsc(); });
    g.Add([&] { g.rdtscp(); });
    g.Add([&] { g.cpuid(); });
    g.Add([&] { g.lfence(); });
    g.Add([&] { g.mfence(); });
    g.Add([&] { g.pause(); });
    g.Add([&] { g.ldmxcsr(dword[r15 + 0x30]); });
    g.Add([&] { g.stmxcsr(dword[rsp + 4]); });
    g.Add([&] { g.call(rax); });
    g.Add([&] { g.jmp(qword[rcx + 8]); });
    for (int n = 1; n <= 15; ++n) {
        g.Add([&] { g.nop(size_t(n)); });
    }

    // SSE, AVX, AVX-512.
    g.Add([&] { g.movaps(xmm0, xmm1); });
    g.Add([&] { g.movq(xmm1, rax); });
    g.Add([&] { g.movq(rax, xmm1); });
    g.Add([&] { g.movd(xmm2, ecx); });
    g.Add([&] { g.pshufd(xmm0, xmm1, 0x1B); });
    g.Add([&] { g.psrlq(xmm0, 3); });
    g.Add([&] { g.pslldq(xmm0, 8); });
    g.Add([&] { g.pinsrq(xmm0, rax, 1); });
    g.Add([&] { g.pextrd(eax, xmm3, 2); });
    g.Add([&] { g.pextrw(eax, xmm3, 2); });
    g.Add([&] { g.pinsrw(xmm3, eax, 2); });
    g.Add([&] { g.shufps(xmm0, xmm1, 0x44); });
    g.Add([&] { g.cmpps(xmm0, xmm1, 3); });
    g.Add([&] { g.roundsd(xmm0, xmm1, 4); });
    g.Add([&] { g.pshufb(xmm0, xmm1); });
    g.Add([&] { g.ptest(xmm0, xmm1); });
    g.Add([&] { g.cvttsd2si(rax, xmm0); });
    g.Add([&] { g.ucomisd(xmm0, qword[rcx]); });
    g.Add([&] { g.aesenc(xmm0, xmm1); });
    g.Add([&] { g.aeskeygenassist(xmm0, xmm1, 1); });
    g.Add([&] { g.sha256rnds2(xmm1, xmm2); });
    g.Add([&] { g.pclmulqdq(xmm0, xmm1, 0x11); });
    g.Add([&] { g.vaddps(ymm0, ymm1, ymm2); });
    g.Add([&] { g.vxorps(xmm0, xmm0, xmm0); });
    g.Add([&] { g.vpshufd(xmm0, xmm1, 0x4E); });
    g.Add([&] { g.vpsrlq(xmm0, xmm1, 7); });
    g.Add([&] { g.vcmpps(xmm0, xmm1, xmm2, 1); });
    g.Add([&] { g.vfmadd231ps(xmm0, xmm1, xmm2); });
    g.Add([&] { g.vfmadd213sd(xmm0, xmm1, qword[rcx + 0x40]); });
    g.Add([&] { g.vpblendvb(xmm0, xmm1, xmm2, xmm3); });
    g.Add([&] { g.vzeroupper(); });
    g.Add([&] { g.vpaddd(zmm0, zmm1, zmm2); });
    g.Add([&] { g.vpaddd(zmm0, zmm1, ptr[rcx + 0x40]); });
    g.Add([&] { g.vpternlogd(xmm0, xmm1, xmm2, 0x96); });
    g.Add([&] { g.vcmpps(k1, xmm1, xmm2, 4); });
    g.Add([&] { g.vpsrlq(xmm16, xmm17, 3); });
    g.Add([&] { g.vfixupimmpd(xmm0, xmm1, xmm2, 0); });
    g.Add([&] { g.vpermt2b(xmm0, xmm1, xmm2); });
    g.Add([&] { g.kmovw(k1, eax); });
    g.Add([&] { g.vmovdqu8(xmm0 | k1 | T_z, xmm1); });
    g.Add([&] { g.vpbroadcastq(xmm0, rax); });

    // Position-dependent fields.
    g.Add([&] { g.movaps(xmm0, ptr[rip + far_target]); }, K::RipDisp32, far_target);
    g.Add([&] { g.cmp(dword[rip + far_target], 0); }, K::RipDisp32, far_target);
    g.Add([&] { g.cmp(qword[rip + far_target], 0x12345); }, K::RipDisp32, far_target);
    g.Add([&] { g.mov(word[rip + far_target], 0x1234); }, K::RipDisp32, far_target);
    g.Add([&] { g.vpaddd(xmm0, xmm1, ptr[rip + far_target]); }, K::RipDisp32, far_target);
    g.Add([&] { g.vpaddd(zmm0, zmm1, ptr[rip + far_target]); }, K::RipDisp32, far_target);
    g.Add([&] { g.vpternlogd(xmm0, xmm1, ptr[rip + far_target], 0x96); }, K::RipDisp32, far_target);
    g.Add([&] { g.jmp(qword[rip + far_target]); }, K::RipDisp32, far_target);
    g.Add([&] { g.lea(rbp, ptr[rip + far_target]); }, K::RipDisp32, far_target);
    g.Add([&] { g.roundsd(xmm0, ptr[rip + far_target], 4); }, K::RipDisp32, far_target);
    g.Add([&] { g.call(far_target); }, K::Rel32, far_target);
    g.Add([&] { g.jmp(far_target, Xbyak::CodeGenerator::T_NEAR); }, K::Rel32, far_target);
    g.Add([&] { g.jne(far_target); }, K::Rel32, far_target);
    g.Add([&] { g.ja(far_target); }, K::Rel32, far_target);
    Xbyak::Label back;
    g.L(back);
    const void* const here = g.getCurr();
    g.Add([&] { g.jmp(here, Xbyak::CodeGenerator::T_SHORT); }, K::Rel8, here);
    g.Add([&] { g.jz(back, Xbyak::CodeGenerator::T_SHORT); }, K::Rel8, here);
    g.Add([&] { g.jnz(back, Xbyak::CodeGenerator::T_NEAR); }, K::Rel32, here);

    const u8* const code = g.getCode();
    size_t pos = 0;
    for (const Expected& e : g.expected) {
        INFO("instruction at offset " << e.begin);
        REQUIRE(pos == e.begin);
        const auto insn = DecodeX64Instruction(code + pos, g.getSize() - pos);
        REQUIRE(insn.has_value());
        REQUIRE(pos + insn->length == e.end);
        REQUIRE(insn->field == e.field);
        const u8* const field = code + pos + insn->field_offset;
        const u8* const end = code + e.end;
        switch (e.field) {
        case K::None:
            break;
        case K::Rel8:
            REQUIRE(end + s8(*field) == e.target);
            break;
        case K::Rel32:
        case K::RipDisp32: {
            s32 disp;
            std::memcpy(&disp, field, sizeof(disp));
            REQUIRE(end + disp == e.target);
            break;
        }
        case K::Imm64: {
            u64 value;
            std::memcpy(&value, field, sizeof(value));
            REQUIRE(value == reinterpret_cast<u64>(e.target));
            break;
        }
        }
        pos = e.end;
    }
    REQUIRE(pos == g.getSize());
}

TEST_CASE("x64 decoder: truncated and unknown bytes", "[x64][code_image]") {
    const u8 mov_imm64[] = {0x48, 0xB8, 1, 2, 3, 4, 5, 6, 7, 8};
    REQUIRE(DecodeX64Instruction(mov_imm64, sizeof(mov_imm64)).has_value());
    for (size_t size = 0; size < sizeof(mov_imm64); ++size) {
        REQUIRE_FALSE(DecodeX64Instruction(mov_imm64, size).has_value());
    }
    const u8 moffs[] = {0x48, 0xA1, 0, 0, 0, 0, 0, 0, 0, 0};  // mov rax, [moffs64]: not emitted
    REQUIRE_FALSE(DecodeX64Instruction(moffs, sizeof(moffs)).has_value());
    const u8 prefixes[16] = {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x90};
    REQUIRE_FALSE(DecodeX64Instruction(prefixes, sizeof(prefixes)).has_value());  // longer than 15 bytes
}

TEST_CASE("Hash128: independent of how the bytes are split", "[code_image]") {
    std::vector<u8> bytes(1000);
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = u8(i * 7 + 3);
    }
    const auto whole = Dynarmic::Common::Hash128::Of(bytes);
    for (const size_t split : {size_t(0), size_t(1), size_t(7), size_t(8), size_t(13), size_t(999)}) {
        Dynarmic::Common::Hash128 h;
        h.Update(std::span{bytes}.first(split));
        h.Update(std::span{bytes}.subspan(split));
        REQUIRE(h.Finish() == whole);
    }
    auto changed = bytes;
    changed[500] ^= 1;
    REQUIRE(Dynarmic::Common::Hash128::Of(changed) != whole);
    REQUIRE(Dynarmic::Common::Hash128::Of(std::span{bytes}.first(999)) != whole);
}
