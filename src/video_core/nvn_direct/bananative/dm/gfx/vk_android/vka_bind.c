// From Banana Native (github.com/hugorosario/BananaNative, Android port of github.com/seedds/nxnative by seedds), MIT: see LICENSE in src/video_core/nvn_direct/bananative.
// The binding walker (vka_bind.h): nvn_mtl.m encode_args' sources for each binding kind, written once for the Vulkan replay and its Mac test
#include "vka_bind.h"
#include "nvn_direct_front.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void vka_bind_log(const char *fmt, ...);   // Lichii: vka_draw.c (the replay's log)

_Static_assert(DMSH_C2_TEX + DMSH_C2_ROW * DMSH_C2_TEX_SLOTS == DMSH_C2_SEPTEX && DMSH_C2_SEPTEX + DMSH_C2_ROW * DMSH_C2_SEP_SLOTS == DMSH_C2_SAMP
               && DMSH_C2_SAMP + DMSH_C2_ROW * DMSH_C2_SEP_SLOTS <= DMSH_DRV_SIZE, "NVN driver handle tables");

uint64_t vka_tex_slot(const vka_bstate *s, int st, unsigned reg) {
    return reg >= 32 && reg - 32 < DMSH_C2_IMG_SLOTS && s->img[st][reg - 32] ? s->img[st][reg - 32] : reg < VKA_NTEX ? s->tex[st][reg] : 0;
}
uint64_t vka_c2_handle(const vka_bstate *s, int st, unsigned b) {
    if (b >= DMSH_C2_TEX && b < DMSH_C2_SEPTEX && !((b - DMSH_C2_TEX) % DMSH_C2_ROW)) return vka_tex_slot(s, st, (b - DMSH_C2_TEX) / DMSH_C2_ROW);
    if (b >= DMSH_C2_SEPTEX && b < DMSH_C2_SAMP && !((b - DMSH_C2_SEPTEX) % DMSH_C2_ROW)) return s->tex[st][DMSH_SEPTEX_SLOT0 + (b - DMSH_C2_SEPTEX) / DMSH_C2_ROW];
    if (b >= DMSH_C2_SAMP && b < DMSH_C2_SAMP + DMSH_C2_ROW * DMSH_C2_SEP_SLOTS && !((b - DMSH_C2_SAMP) % DMSH_C2_ROW)) return s->samp[st][(b - DMSH_C2_SAMP) / DMSH_C2_ROW];
    return 0;
}
static uint64_t ubo_word(const vka_bstate *s, int st, int reg, unsigned off, unsigned n, vka_read_fn rd, void *ctx) {
    uint64_t v = 0;
    if (reg < 0 || reg >= 16 || !s->ubo[st][reg].addr || !rd) return 0;
    if (s->ubo[st][reg].size && off + n > s->ubo[st][reg].size) return 0;
    if (!rd(ctx, s->ubo[st][reg].addr + off, &v, n)) return 0;
    return v;
}
static int ubo_tex_id(const vka_bstate *s, uint32_t h) { int t = (int)(h & 0xfffff); return t ? t + s->tex_base : 0; }
static int ubo_samp_id(const vka_bstate *s, uint32_t h) { int x = (int)(h >> 20); return x ? x + s->samp_base : 0; }

// The sampler half of a combined image sampler (skind, sreg, soff); element k of an array
static int samp_of(const vka_bstate *s, int st, const dmsh_vk_bind *b, unsigned k, vka_read_fn rd, void *ctx) {
    switch (b->skind) {
    case DMSH_VK_KIND_SAMP_TEX: return b->sreg >= 0 && b->sreg < VKA_NTEX ? (int)(vka_tex_slot(s, st, (unsigned)b->sreg) >> 20 & 0xfff) : 0;   // the handle the texture half reads
    case DMSH_VK_KIND_SAMP: return b->sreg >= 0 && b->sreg < 128 ? (int)(s->samp[st][b->sreg] >> 20 & 0xfff) : 0;
    case DMSH_VK_KIND_SAMP_ID: return b->sreg > 0 ? b->sreg : 0;   // a constant of the shader (nvn_mtl.m NVN_RB_SAMPI): taken as it is
    case DMSH_VK_KIND_SAMP_UBO: return ubo_samp_id(s, (uint32_t)ubo_word(s, st, b->sreg, b->soff, 4, rd, ctx));
    case DMSH_VK_KIND_SAMP_C2A: {   // the handle at c2 byte off + 8 k, ORed with the separate sampler word at soff (nvn_mtl.m NVN_RB_SAMPA)
        uint64_t sw = b->soff != 0xFFFF ? vka_c2_handle(s, st, b->soff) : 0;
        return (int)((vka_c2_handle(s, st, b->off + 8u * k) | sw) >> 20 & 0xfff); }
    default: return 0;
    }
}

// ---------- Lichii: handle programs (shader_recompiler/shader_info.h EvaluateHandleProgram, HandleOp) ----------
// A stack machine over 32-bit words whose constant buffer and storage buffer reads are the draw's: the replay's driver banks (vka_drv_fill), its uniform
// buffers, and the storage buffers the program's descriptors point at, as VideoCommon::EvaluateTextureHandle reads them for the GPU emulation
typedef struct { const vka_bstate *s; int st; vka_read_fn rd; void *ctx; uint32_t ssbo_align; const uint32_t *ssbo; uint32_t nssbo;
                 uint8_t drv[3][DMSH_DRV_SIZE]; uint8_t have[3]; } hp_ctx;
static uint32_t hp_cbuf(hp_ctx *x, uint32_t bank, uint32_t off) {
    uint32_t v = 0;
    int i = bank == 0 ? 0 : bank == 2 ? 1 : bank == DMSH_PUSH_BANK ? 2 : -1;
    if (i >= 0) {
        if (!x->have[i]) { vka_drv_fill(x->s, x->st, (int)bank, 0, x->drv[i]); x->have[i] = 1; }
        if (off + 4 <= vka_drv_size((int)bank)) memcpy(&v, x->drv[i] + off, 4);
        return v;
    }
    if (bank >= 3 && bank < 19) return (uint32_t)ubo_word(x->s, x->st, (int)bank - 3, off, 4, x->rd, x->ctx);
    return 0;   // c1: the shader's own constants, which no program seen reads
}
static uint64_t hp_mem64(hp_ctx *x, uint64_t guest) {   // a word at a game address (the game's pointers and c0's storage table hold those)
    uint64_t v = 0;
    return guest && x->rd(x->ctx, nvn_direct_host_of(guest), &v, 8) ? v : 0;
}
static int hp_trace;   // DM_VK_HPROG_TRACE: the reads of the evaluation being traced
static uint32_t hp_storage(hp_ctx *x, uint32_t binding, uint32_t off) {
    if (binding >= x->nssbo) return 0;
    const uint32_t *d = x->ssbo + 3 * binding;   // cbuf index, cbuf offset, indirect offset (VideoCommon::StorageBufferBase)
    uint64_t base = (uint64_t)hp_cbuf(x, d[0], d[1]) | (uint64_t)hp_cbuf(x, d[0], d[1] + 4) << 32, b0 = base;
    if (base && d[2] != ~0u) base = hp_mem64(x, base + d[2]);
    uint32_t v = 0; int got = 0;
    if (base) { base &= ~(uint64_t)((x->ssbo_align ? x->ssbo_align : 1) - 1); got = x->rd(x->ctx, nvn_direct_host_of(base + off), &v, 4); }
    if (hp_trace) vka_bind_log("hprog:   storage %u (c%u+%#x ind %#x) ptr %#llx base %#llx + %#x -> %s %#x", binding, d[0], d[1], d[2], (unsigned long long)b0, (unsigned long long)base, off, got ? "read" : "unread", v);
    return got ? v : 0;
}
static int hp_eval(hp_ctx *x, const uint32_t *code, uint32_t len, uint32_t element, uint32_t *out) {
    uint32_t stack[64], slots[256]; unsigned size = 0;
    memset(slots, 0, sizeof slots);
#define POP() (size ? stack[--size] : 0u)
#define PUSH(v) do { if (size < 64) stack[size++] = (v); } while (0)
    for (uint32_t pc = 0; pc < len;) {
        const uint32_t op = code[pc++];
#define OPERAND() (pc < len ? code[pc++] : 0u)
        switch (op) {
        case 0: PUSH(OPERAND()); continue;                                                    // Immediate
        case 1: { uint32_t i = OPERAND(), o = OPERAND(), v = hp_cbuf(x, i, o); if (hp_trace) vka_bind_log("hprog:   c%u+%#x -> %#x", i, o, v); PUSH(v); continue; }  // Cbuf
        case 2: { uint32_t i = OPERAND(), o = POP(); PUSH(hp_cbuf(x, i, o)); continue; }      // CbufDynamic
        case 3: { uint32_t b = OPERAND(), o = POP(); PUSH(hp_storage(x, b, o)); continue; }   // Storage
        case 13: { uint32_t a = POP(); PUSH(0u - a); continue; }                              // Neg
        case 14: { uint32_t a = POP(); PUSH(~a); continue; }                                  // Not
        case 32: { uint32_t a = POP(); PUSH(a == 0); continue; }                              // LNot
        case 33: { uint32_t f = POP(), t = POP(), c = POP(); PUSH(c ? t : f); continue; }     // Select
        case 34: { uint32_t s = OPERAND(); if (s < 256 && size) slots[s] = stack[size - 1]; continue; }   // Tee
        case 35: { uint32_t s = OPERAND(); PUSH(s < 256 ? slots[s] : 0u); continue; }        // Load
        case 36: PUSH(element); continue;                                                     // Index
        }
#undef OPERAND
        const uint32_t b = POP(), a = POP(); const int32_t sa = (int32_t)a, sb = (int32_t)b;
        switch (op) {
        case 4: PUSH(a + b); break; case 5: PUSH(a - b); break; case 6: PUSH(a * b); break;
        case 7: PUSH(a & b); break; case 8: PUSH(a | b); break; case 9: PUSH(a ^ b); break;
        case 10: PUSH(b >= 32 ? 0u : a << b); break; case 11: PUSH(b >= 32 ? 0u : a >> b); break;
        case 12: PUSH((uint32_t)(sa >> (b >= 32 ? 31 : b))); break;
        case 15: PUSH(a < b ? a : b); break; case 16: PUSH(a > b ? a : b); break;
        case 17: PUSH((uint32_t)(sa < sb ? sa : sb)); break; case 18: PUSH((uint32_t)(sa > sb ? sa : sb)); break;
        case 19: PUSH(sa < sb); break; case 20: PUSH(sa <= sb); break; case 21: PUSH(sa > sb); break; case 22: PUSH(sa >= sb); break;
        case 23: PUSH(a < b); break; case 24: PUSH(a <= b); break; case 25: PUSH(a > b); break; case 26: PUSH(a >= b); break;
        case 27: PUSH(a == b); break; case 28: PUSH(a != b); break;
        case 29: PUSH(a && b); break; case 30: PUSH(a || b); break; case 31: PUSH(!a != !b); break;
        default: return 0;
        }
    }
#undef POP
#undef PUSH
    if (size != 1) return 0;
    *out = stack[0];
    return 1;
}
// Program p of a shader's handle programs (nvn_direct.h layout), with its storage buffer descriptors; 0 when it has none
static int hp_find(const uint32_t *w, uint32_t n, unsigned p, const uint32_t **code, uint32_t *len, const uint32_t **ssbo, uint32_t *nssbo) {
    if (!w || n < 2) return 0;
    uint32_t np = w[0], at = 1;
    for (uint32_t i = 0; i < np; i++) {
        if (at >= n) return 0;
        uint32_t l = w[at++];
        if (at + l > n) return 0;
        if (i == p) { *code = w + at; *len = l; }
        at += l;
    }
    if (p >= np || at >= n) return 0;
    *nssbo = w[at++];
    if (at + 3 * (size_t)*nssbo > n) return 0;
    *ssbo = w + at;
    return 1;
}

static int refuse(char *why, size_t n, const char *fmt, ...) {
    if (why && n) { va_list ap; va_start(ap, fmt); vsnprintf(why, n, fmt, ap); va_end(ap); }
    return VKA_WALK_REFUSE;
}

int vka_walk(const vka_bstate *s, int st, const dmsh_vk_entry *e, const dmsh_vk_bind *bs, uint32_t ssbo_align, uint64_t dev_align, vka_read_fn rd, void *ctx,
             vka_res *out, int max, int *uses_pool, char *why, size_t whyn, const uint32_t *hprog, uint32_t hprog_words) {
    int n = 0;
    if (st < 0 || st >= VKA_NSTAGE) return refuse(why, whyn, "NVN stage %d", st);
    static _Thread_local hp_ctx hx;   // (the driver banks a program reads, filled once a walk)
    hx.s = s; hx.st = st; hx.rd = rd; hx.ctx = ctx; hx.ssbo_align = ssbo_align; memset(hx.have, 0, sizeof hx.have);
    for (unsigned i = 0; i < e->bind_count; i++) {
        const dmsh_vk_bind *b = &bs[i];
        if (b->kind == DMSH_VK_KIND_UNKNOWN) return refuse(why, whyn, "binding %u has no placed kind", b->binding);
        if (b->set == DMSH_VK_SET_POOL || b->kind == DMSH_VK_KIND_TEX_POOL || b->kind == DMSH_VK_KIND_SAMP_POOL) { if (uses_pool) *uses_pool = 1; continue; }
        unsigned cnt = b->count ? b->count : 1;
        for (unsigned k = 0; k < cnt; k++) {
            if (n >= max) return refuse(why, whyn, "more than %d descriptors", max);
            vka_res r = {VKA_R_NULL, b->desc, b->flags, b->dim, b->binding, (uint16_t)k, 0, 0, 0, 0, 0};
            int reg = b->reg;
            int noslot = (b->flags & DMSH_VK_BF_NOSLOT) || reg < 0;
            switch (b->kind) {
            case DMSH_VK_KIND_CBUF:
                if (!noslot && reg < 16 && s->ubo[st][reg].addr) { r.what = VKA_R_BUF; r.addr = s->ubo[st][reg].addr; r.size = s->ubo[st][reg].size; }
                break;
            case DMSH_VK_KIND_C1: r.what = VKA_R_C1; break;
            case DMSH_VK_KIND_SUPPORT: r.what = VKA_R_SUPPORT; break;
            case DMSH_VK_KIND_DRVCB: r.what = VKA_R_DRV; r.bank = (int16_t)reg; break;
            case VKA_KIND_NVN2_CS_C0: r.what = VKA_R_DRV; r.bank = VKA_BANK_NVN2_CS_C0; break;
            case VKA_KIND_LOOP_GUARD: r.what = VKA_R_LOOP_GUARD; break;
            case VKA_KIND_MESH_OUT: r.what = VKA_R_MESH_OUT; break;
            case DMSH_VK_KIND_SBUF: case DMSH_VK_KIND_UBO_PTR: {
                const vka_range *u = noslot || reg >= 16 ? NULL : b->kind == DMSH_VK_KIND_SBUF ? &s->ssbo[st][reg] : &s->ubo[st][reg];
                if (u && u->addr) { r.what = VKA_R_BUF; r.addr = u->addr; r.size = u->size; }
                break; }
            case DMSH_VK_KIND_SBUF_UBO: {
                uint64_t a = noslot ? 0 : ubo_word(s, st, reg, b->off, 8, rd, ctx);
                if (a) { r.what = VKA_R_BUF; r.addr = a; r.size = 0; }   // the size is not known: to the end of the chunk
                break; }
            case DMSH_VK_KIND_TEX: case DMSH_VK_KIND_TEX_UBO: case DMSH_VK_KIND_TEX_C2A: {
                int tid = b->kind == DMSH_VK_KIND_TEX_UBO ? (noslot ? 0 : ubo_tex_id(s, (uint32_t)ubo_word(s, st, reg, b->off, 4, rd, ctx)))
                        : b->kind == DMSH_VK_KIND_TEX_C2A ? (int)(vka_c2_handle(s, st, b->off + 8u * k) & 0xfffff)
                        : noslot ? 0 : (int)(vka_tex_slot(s, st, (unsigned)reg) & 0xfffff);
                int sid = b->skind ? samp_of(s, st, b, k, rd, ctx) : 0;
                if (tid) { r.what = VKA_R_TEX; r.tid = tid; r.sid = sid; }
                break; }
            case VKA_KIND_SBUF_IND: case VKA_KIND_SBUF_UBO_IND: {   // Lichii: the pointer's table entry holds the buffer's address (a game address)
                uint64_t p = b->kind == VKA_KIND_SBUF_IND ? (reg >= 0 && reg < 16 ? s->ssbo[st][reg].addr : 0) : ubo_word(s, st, reg, b->off, 8, rd, ctx), a = 0;
                if (p && rd && rd(ctx, nvn_direct_host_of(p + b->soff), &a, 8) && a) { r.what = VKA_R_BUF; r.addr = nvn_direct_host_of(a); r.size = 0; }
                {   // Fritchii, FRITCHII_SBUF_IND_TRACE=<key>: that shader's table reads (the table, the entry, the pointer and the words around it)
                    static uint64_t tk = ~0ull; if (tk == ~0ull) { const char *ev = getenv("FRITCHII_SBUF_IND_TRACE"); tk = ev ? strtoull(ev, NULL, 16) : 0; }
                    static _Atomic int nt;
                    if (tk && e->key == tk && atomic_fetch_add(&nt, 1) < 40) {
                        uint64_t w[4] = {0}; if (p && rd) rd(ctx, nvn_direct_host_of(p + (b->soff & ~31u)), w, sizeof w);
                        vka_bind_log("sbuf_ind: shader %016llx binding %u kind %d slot %d: table %#llx + %#x -> %#llx; words at the entry's 32 bytes: %llx %llx %llx %llx",
                                     (unsigned long long)e->key, b->binding, b->kind, reg, (unsigned long long)p, b->soff, (unsigned long long)a, (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3]);
                    }
                }
                break; }
            case VKA_KIND_TEX_HPROG: {   // Lichii: the handle the program computes from this draw's words (a combined handle: texture and sampler)
                const uint32_t *code, *ssbo; uint32_t len, nssbo, h = 0;
                if (!rd || !hp_find(hprog, hprog_words, (unsigned)reg, &code, &len, &ssbo, &nssbo))
                    return refuse(why, whyn, "binding %u: handle program %d not kept", b->binding, reg);
                hx.ssbo = ssbo; hx.nssbo = nssbo;
                static int on = -1; static _Atomic int nlog;   // DM_VK_HPROG_TRACE=1: the first evaluations, with their reads
                if (on < 0) { const char *ev = getenv("DM_VK_HPROG_TRACE"); on = ev && *ev == '1'; }
                hp_trace = on && k == 0 && atomic_load(&nlog) < 300;
                const int ok = hp_eval(&hx, code, len, k, &h);
                if (ok && (h & 0xfffff)) { r.what = VKA_R_TEX; r.tid = ubo_tex_id(s, h); r.sid = ubo_samp_id(s, h); }
                {
                    if (hp_trace && atomic_fetch_add(&nlog, 1) < 300) {
                        char cs[200]; int at = 0;
                        for (uint32_t w = 0; w < len && w < 24 && at < 180; w++) at += snprintf(cs + at, sizeof cs - at, "%x ", code[w]);
                        vka_bind_log("hprog: shader %016llx stage %d binding %u (count %u) program %d: %s handle %#x -> texture %d sampler %d (nvn2 %d) code [%u] %s",
                                     (unsigned long long)e->key, st, b->binding, cnt, reg, ok ? "ok" : "malformed", h, r.tid, r.sid, s->nvn2, len, cs);
                    }
                }
                break; }
            case DMSH_VK_KIND_TBUF: {
                int tid = noslot ? 0 : (int)(vka_tex_slot(s, st, (unsigned)reg) & 0xfffff);
                if (tid) { r.what = VKA_R_TBUF; r.tid = tid; }
                break; }
            case DMSH_VK_KIND_IMG: {
                int tid = noslot || reg >= 32 ? 0 : (int)(s->img[st][reg] & 0xfffff);
                if (tid) { r.what = VKA_R_IMG; r.tid = tid; }
                break; }
            case DMSH_VK_KIND_SAMP: case DMSH_VK_KIND_SAMP_TEX: case DMSH_VK_KIND_SAMP_ID: case DMSH_VK_KIND_SAMP_UBO: case DMSH_VK_KIND_SAMP_C2A: {
                dmsh_vk_bind sb = *b; sb.skind = b->kind; sb.sreg = b->reg; sb.soff = b->kind == DMSH_VK_KIND_SAMP_C2A ? b->soff : b->off;   // a sampler on its own: its own fields name it
                int sid = noslot && b->kind != DMSH_VK_KIND_SAMP_C2A ? 0 : samp_of(s, st, &sb, k, rd, ctx);
                if (sid) { r.what = VKA_R_TEX; r.sid = sid; }
                break; }
            case DMSH_VK_KIND_GMEM_MAP: r.what = VKA_R_GMEM_MAP; break;
            case DMSH_VK_KIND_GMEM_LOG: r.what = VKA_R_GMEM_LOG; break;
            case DMSH_VK_KIND_SHARED_SPILL:
                if (!b->bytes || b->bytes % 4) return refuse(why, whyn, "binding %u: shared memory spill of %u bytes a slice", b->binding, b->bytes);
                r.what = VKA_R_SHARED_SPILL; r.size = b->bytes; break;
            default: return refuse(why, whyn, "binding %u: kind %u", b->binding, b->kind);
            }
            if (r.what == VKA_R_BUF && r.desc == 7 /* VK_DESCRIPTOR_TYPE_STORAGE_BUFFER */) {
                if (ssbo_align) { uint64_t d = r.addr & (ssbo_align - 1); r.addr -= d; if (r.size) r.size += d; }   // the shader adds the difference back
                else if (dev_align && (r.addr & (dev_align - 1)))
                    return refuse(why, whyn, "storage buffer (binding %u, slot %d) at %#llx is not %llu-byte aligned (a library built before ssbo_align)", b->binding, reg, (unsigned long long)r.addr, (unsigned long long)dev_align);
            }
            out[n++] = r;
        }
    }
    return n;
}

static int c0_ssbo_base(int st) { return st == 0 ? DMSH_C0_SSBO_VS : st == 1 ? DMSH_C0_SSBO_PS : st == 2 ? DMSH_C0_SSBO_GS : st == 5 ? DMSH_C0_SSBO_CS : st == 6 ? DMSH_C0_SSBO_TS : st == 7 ? DMSH_C0_SSBO_MS : -1; }

// The pixel stage's sample table (nvn_mtl.m c0_sample_rows): one byte per sample, x in the low nibble and y in the high one in 1/16 pixel, at Vulkan's standard sample locations
static void c0_sample_rows(uint8_t *d, uint32_t samples) {
    static const uint8_t pos[5][16] = {
        {0x88},
        {0xCC, 0x44},
        {0x26, 0x6E, 0xA2, 0xEA},
        {0x59, 0xB7, 0x9D, 0x35, 0xD3, 0x71, 0xFB, 0x1F},
        {0x99, 0x57, 0xA5, 0x7C, 0x63, 0xDA, 0xBD, 0x3B, 0xE6, 0x18, 0x24, 0xC2, 0x80, 0x4F, 0xFE, 0x01},
    };
    if (samples < 1 || samples > 16 || (samples & (samples - 1))) samples = 1;
    int l = __builtin_ctz(samples);
    uint32_t info[4] = {samples - 1, (uint32_t)l, 0, 0};
    memcpy(d + DMSH_C0_SAMPLE_TABLE, pos[l], DMSH_C0_SAMPLE_BYTES); memcpy(d + DMSH_C0_SAMPLE_INFO, info, sizeof info);
}

size_t vka_drv_size(int bank) { return bank == DMSH_PUSH_BANK ? DMSH_PUSH_SIZE : DMSH_DRV_SIZE; }
void vka_drv_fill(const vka_bstate *s, int st, int bank, int pool, uint8_t *d) {
    if (bank == DMSH_PUSH_BANK) { memcpy(d, s->push[st == 5], DMSH_PUSH_SIZE); return; }
    memset(d, 0, DMSH_DRV_SIZE);
    if (bank == 0) {
        int base = c0_ssbo_base(st);
        if (base >= 0) for (int r = 0; r < DMSH_C0_SSBO_SLOTS; r++) {
            uint64_t a = nvn_direct_guest_of(s->ssbo[st][r].addr); uint32_t n = (uint32_t)s->ssbo[st][r].size;   // Lichii: the game's address, as raw global memory's table keys it (vka_mem.c)
            memcpy(d + base + DMSH_C0_ROW * r, &a, 8); memcpy(d + base + DMSH_C0_ROW * r + 8, &n, 4);
        }
        if (st != 5) { const float max_point = 2047.0f; memcpy(d + DMSH_C0_POINT_SIZE, &max_point, 4); memcpy(d + DMSH_C0_BASE_INSTANCE, &s->base_instance, 4); }
        if (st == 1) c0_sample_rows(d, s->samples);
        if (st == 5) {
            memcpy(d + DMSH_C0_DISPATCH, s->dispatch, 12);
            // NVN2's compute driver segment keeps the counts at c0 + 4 too (x, y, z), below the per-shader layout the translator remaps from 0x10 (InstEmitAmpere.AMapC0):
            // a shader that indexes by them reads 0 otherwise and every workgroup of a column writes the same entry (upstream nxnative 0a759de7: nvn_mtl.m encode_args_; Fire Emblem Fortune's Weave's light culling)
            if (s->nvn2) memcpy(d + DMSH_C0_DISPATCH_NVN2, s->dispatch, 12);
            for (int r = 0; r < DMSH_C0_CS_UBO_SLOTS; r++) {
                uint64_t a = nvn_direct_guest_of(s->ubo[5][r].addr); uint32_t n = (uint32_t)s->ubo[5][r].size;   // Lichii: the game's address (as above)
                memcpy(d + DMSH_C0_CS_UBO + DMSH_C0_ROW * r, &a, 8); memcpy(d + DMSH_C0_CS_UBO + DMSH_C0_ROW * r + 8, &n, 4);
            }
        }
    } else if (bank == 2) {
        // A shader that reads the pools indexes them with the IDs it reads, which are the bound pool's own: its c2 holds the handles as the game bound them, without the bases
        int tb = pool ? s->tex_base : 0, sb = pool ? s->samp_base : 0;
#define C2_LOCAL(h) ((uint32_t)((((h) & 0xfffff) ? ((h) & 0xfffff) - (uint64_t)tb : 0) | ((((h) >> 20) & 0xfff) ? ((((h) >> 20) & 0xfff) - (uint64_t)sb) << 20 : 0)))
        const uint64_t *tx = s->tex[st], *im = s->img[st];
        for (int r = 0; r < DMSH_C2_IMG_SLOTS; r++) { uint32_t ih = (uint32_t)(im[r] & 0xfffff ? im[r] - (uint64_t)tb : im[r]); memcpy(d + DMSH_C2_IMG + DMSH_C2_ROW * r, &ih, 4); }   // (bind_handles moves only an image's texture ID)
        for (int r = 0; r < DMSH_C2_TEX_SLOTS; r++) {   // a combined slot past 31 is written when it is bound and the image slot it shares is not
            if (r >= 32 && (!tx[r] || (r - 32 < DMSH_C2_IMG_SLOTS && im[r - 32]))) continue;
            uint32_t v = C2_LOCAL(tx[r]); memcpy(d + DMSH_C2_TEX + DMSH_C2_ROW * r, &v, 4);
        }
        for (int r = 0; r < DMSH_C2_SEP_SLOTS; r++) { uint64_t h = tx[DMSH_SEPTEX_SLOT0 + r]; uint32_t v = C2_LOCAL(h); memcpy(d + DMSH_C2_SEPTEX + DMSH_C2_ROW * r, &v, 4); }
        for (int r = 0; r < DMSH_C2_SEP_SLOTS; r++) { uint64_t h = s->samp[st][r]; uint32_t v = C2_LOCAL(h); memcpy(d + DMSH_C2_SAMP + DMSH_C2_ROW * r, &v, 4); }
#undef C2_LOCAL
    }
}

void vka_support_fill(uint8_t *d, size_t n) {
    memset(d, 0, n);
    for (size_t o = 192; o + 4 <= n && o < 192 + 16 * 73; o += 16) { const float one = 1.0f; memcpy(d + o, &one, 4); }   // render_scale[73] (nvn_mtl.m support_buf)
}
