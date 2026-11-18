// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The replay's shaders from Lichii's recompiler, in place of Banana Native's offline shader library
// (vka_shader.c).
//
// A program stage is translated at its first draw or dispatch, with that draw's state: the
// recompiler reads constant buffer words (bindless handles, storage buffer pointers), the types
// and formats of the textures handles name, the vertex attributes' types and the depth mode, as
// Lichii's Maxwell path reads them from the GPU's state. Its control section is only valid during
// nvnProgramSetShaders, so a copy is kept per shader key (vka_rc_keep, from there). NVN2 stages
// are Ampere code, NVN1 (Switch 1) stages Maxwell code (vk_nvn_direct_shaders.cpp reads both).
//
// NVN1 compute reads its driver constants c[0] in the fixed tables of the graphics stages' layout
// (dmsh_abi.h DMSH_C0_SSBO_CS, DMSH_C0_CS_UBO, DMSH_C0_DISPATCH), which vka_drv_fill fills.
//
// Its SPIR-V numbers its descriptors from the stage's first binding in the replay's set 0
// (dmsh_abi_vk.h: stage s at s * DMSH_VK_STAGE_BINDINGS; compute and mesh use the vertex range, task
// the tess control one), and each descriptor becomes a binding record of the library's ABI
// (rc_bind): what reads the bound NVN state is vka_bind.c's walker, as for library shaders. The
// shader is a one-entry library in memory (rc_lib), so pipelines and binding take it unchanged.
//
// NVN2 compute lays out its driver constants c[0] per shader (control +0xa9 push constant words,
// +0xb1 / +0xb2 / +0xb3 uniform, storage and texture entries, +0xba the size; InstEmitAmpere.cs
// AMapC0 in Banana Native's translator). The library's translator remaps those reads to the fixed
// tables; Lichii's reads them where they are, so a recompiled compute shader's c[0] is filled in
// that layout (vka_rc_cs_c0, binding kind VKA_KIND_NVN2_CS_C0).

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/resource.h>
#include <unistd.h>

#include "bananative/dm/gfx/vk_android/vka.h"
#include "bananative/dm/host/plat/plat.h"
#include "nvn_direct.h"
#include "nvn_direct_front.h"
#include "nvn_recompile_stamp.h"

int vka_shader_translate(const nvn_direct_shader_source* s, nvn_direct_shader* o); // vka_dev.c
void vka_shader_translated_free(nvn_direct_shader* o);

static uint32_t rd32(const uint8_t* p, size_t o) { uint32_t v; memcpy(&v, p + o, 4); return v; }
static int nvn2_control(const uint8_t* c) { return c && rd32(c, 0) == 0x98761234u && rd32(c, 4) == 0xc9; }

// A stage's control section as translation reads it (vka_rc_keep): its NVN stage (an NVN2 section's +0x56, an NVN1 stage's code area's) and the section's
// first size bytes (NVN2's whole section, NVN1's first NVN_DIRECT_NVN1_CONTROL_SIZE)
typedef struct { uint32_t stage, size; uint8_t bytes[]; } rc_control;
static size_t rc_control_bytes(const rc_control* c) { return sizeof *c + c->size; }
// Where the shader's own constants (c[1]) lie in its code area: NVN2 between control +0x40 and +0x44, NVN1 between +0x700 and +0x704 (the code area's end)
static void rc_c1_range(const rc_control* c, uint32_t* begin, uint32_t* end) {
    const int nvn2 = nvn2_control(c->bytes);
    *begin = rd32(c->bytes, nvn2 ? 0x40 : 0x700); *end = rd32(c->bytes, nvn2 ? 0x44 : 0x704);
}
// An NVN1 stage's NVN stage: graphics code starts with a 0x30-byte header (magic 0x12345678) and an SPH whose shader type (word 0 bits 10-13: 0 and 1 vertex,
// 2 tess control, 3 tess evaluation, 4 geometry, 5 pixel) names it; compute code has a header of its own (0x12345679) or none, and a workgroup at control +0x748
// (Luigi's Mansion 3's has none); -1 neither
static int nvn1_stage(const uint8_t* control, const uint8_t* code, uint32_t ncode) {
    static const int8_t of_type[6] = {0, 0, 3, 4, 2, 1};
    if (ncode >= 0x80 && rd32(code, 0) == 0x12345678u) {
        const uint32_t type = rd32(code, 0x30) >> 10 & 0xf;
        return type < 6 ? of_type[type] : -1;
    }
    return rd32(control, 0x748) ? 5 : -1;
}

// ---------- Tables by shader key (open addressing, 0 = empty) ----------
typedef struct { uint64_t key; void* val; } rc_ent;
typedef struct { rc_ent* e; size_t cap, n; } rc_map;
static size_t rc_slot(uint64_t k, size_t cap) { return (size_t)((k * 0x9E3779B97F4A7C15ull) >> 17) & (cap - 1); }
static rc_ent* rc_find(rc_map* m, uint64_t key) {
    if (!m->cap) return NULL;
    for (size_t i = rc_slot(key, m->cap);; i = (i + 1) & (m->cap - 1)) { if (!m->e[i].key) return NULL; if (m->e[i].key == key) return &m->e[i]; }
}
static void rc_put(rc_map* m, uint64_t key, void* val) {
    rc_ent* f = rc_find(m, key);
    if (f) { f->val = val; return; }
    if (2 * (m->n + 1) > m->cap) {
        size_t nc = m->cap ? 2 * m->cap : 1024; rc_ent* n = calloc(nc, sizeof *n);
        for (size_t i = 0; i < m->cap; i++) if (m->e[i].key) { size_t j = rc_slot(m->e[i].key, nc); while (n[j].key) j = (j + 1) & (nc - 1); n[j] = m->e[i]; }
        free(m->e); m->e = n; m->cap = nc;
    }
    size_t j = rc_slot(key, m->cap); while (m->e[j].key) j = (j + 1) & (m->cap - 1);
    m->e[j] = (rc_ent){key, val}; m->n++;
}
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static rc_map g_ctl;      // key -> rc_control
static rc_map g_sh;       // key -> rc_shader
static uint64_t rc_key(uint64_t k) { return k ? k : 1; }   // (0 marks an empty slot)

static void spec_add(uint64_t key, uint64_t data, const rc_control* control);
// nvnProgramSetShaders (mtl_prog_prefetch): the control sections, while they are valid
void vka_rc_keep(const prog_t* p) {
    for (unsigned i = 0; i < p->nstage && i < 6; i++) {
        const uint8_t* c = p->sd[i].control; uint8_t md[8];
        if (!c || rd32(c, 0) != 0x98761234u || !prog_key(p, i, md)) continue;
        uint64_t key = rc_key(vka_shlib_key(md));
        int st; uint32_t size;
        if (nvn2_control(c)) { st = c[0x56]; size = NVN_DIRECT_NVN2_CONTROL_SIZE; }
        else {   // NVN1: the stage its code area names, which is in a memory pool by now
            uint32_t nctl, ncode; nvn_ctl_lens(c, &nctl, &ncode);
            const uint64_t data = p->sd[i].data;
            st = data && ncode && vka_addr(data, ncode, NULL, NULL, NULL) ? nvn1_stage(c, (const uint8_t*)(uintptr_t)nvn_direct_host_of(data), ncode) : -1;
            size = NVN_DIRECT_NVN1_CONTROL_SIZE;
            if (st < 0) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16) VRED("shader %016llx: its code area (%u bytes at %#llx) is not in a memory pool or names no stage: not translated", (unsigned long long)key, ncode, (unsigned long long)data); continue; }
        }
        pthread_mutex_lock(&g_mu);
        rc_control* copy = NULL;
        if (!rc_find(&g_ctl, key)) {
            copy = malloc(sizeof *copy + size);
            if (copy) { copy->stage = (uint32_t)st; copy->size = size; memcpy(copy->bytes, c, size); rc_put(&g_ctl, key, copy); }
        }
        pthread_mutex_unlock(&g_mu);
        if (copy) spec_add(key, p->sd[i].data, copy);
    }
}
static const rc_control* rc_control_of(uint64_t key) {
    pthread_mutex_lock(&g_mu); rc_ent* e = rc_find(&g_ctl, rc_key(key)); const rc_control* c = e ? e->val : NULL; pthread_mutex_unlock(&g_mu);
    return c;
}

// Loops the recompiler's safety check ended (Lichii's 8192 iterations; RuntimeInfo::loop_guard_binding): each shader with loops has a word in static
// memory the check sets, looked at every 4096 draws and dispatches and logged once per shader: a loop that would not end has wrong inputs, which the
// check hides (the library's modules have no such check, and such a loop hangs the GPU)
typedef struct { uint64_t key; const volatile uint32_t* word; int noted; } rc_guard;
static rc_guard* g_guards; static size_t g_nguards, g_guards_cap;
static void guard_add(uint64_t key, const vka_span* s) {
    pthread_mutex_lock(&g_mu);
    if (g_nguards == g_guards_cap) { size_t nc = g_guards_cap ? 2 * g_guards_cap : 256; rc_guard* n = realloc(g_guards, nc * sizeof *n); if (!n) { pthread_mutex_unlock(&g_mu); return; } g_guards = n; g_guards_cap = nc; }
    g_guards[g_nguards++] = (rc_guard){key, (const volatile uint32_t*)s->ptr, 0};
    pthread_mutex_unlock(&g_mu);
}
static void guard_poll(void) {
    static unsigned calls;
    if (++calls % 4096) return;
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < g_nguards; i++) if (!g_guards[i].noted && *g_guards[i].word) {
        g_guards[i].noted = 1;
        VRED("recompiled shader %016llx: a loop ran 8192 iterations and the recompiler's safety check ended it (its inputs would not end it)", (unsigned long long)g_guards[i].key);
    }
    pthread_mutex_unlock(&g_mu);
}

// The draw or dispatch whose shaders are resolved now (vka_draw.c): the state translation reads
static const vka_dstate* g_draw;
void vka_rc_draw(const vka_dstate* d) { g_draw = d; guard_poll(); }

// ---------- NVN2 compute's c[0] ----------
typedef struct { unsigned ub, sb, hb, ib, size, push; } cs_layout;
static cs_layout cs_layout_of(const uint8_t* c) {
    cs_layout l; l.push = c[0xa9] * 4u;
    unsigned ubos = c[0xb1], ssbos = c[0xb2], textures = c[0xb3]; uint16_t size; memcpy(&size, c + 0xba, 2);
    l.ub = 0x10 + ((l.push + 15) & ~15u); l.sb = l.ub + DMSH_C0_ROW * ubos; l.hb = l.sb + DMSH_C0_ROW * ssbos; l.ib = l.hb + DMSH_C2_ROW * textures; l.size = size;
    return l;
}
static void put_range(uint8_t* d, unsigned at, const vka_range* r) {
    if (at + 12 > DMSH_DRV_SIZE) return;
    uint64_t a = nvn_direct_guest_of(r->addr); uint32_t n = (uint32_t)r->size;   // the game's address, as vka_drv_fill writes them
    memcpy(d + at, &a, 8); memcpy(d + at + 8, &n, 4);
}
static void cs_c0_fill(const uint8_t* control, const vka_bstate* s, uint8_t* d) {
    memset(d, 0, DMSH_DRV_SIZE);
    cs_layout l = cs_layout_of(control);
    memcpy(d + DMSH_C0_DISPATCH_NVN2, s->dispatch, 12);
    memcpy(d + 0x10, s->push[1], l.push < DMSH_PUSH_SIZE ? l.push : DMSH_PUSH_SIZE);
    for (unsigned r = 0; l.ub + DMSH_C0_ROW * r < l.sb && r < 16; r++) put_range(d, l.ub + DMSH_C0_ROW * r, &s->ubo[5][r]);
    for (unsigned r = 0; l.sb + DMSH_C0_ROW * r < l.hb && r < 16; r++) put_range(d, l.sb + DMSH_C0_ROW * r, &s->ssbo[5][r]);
    for (unsigned t = 0; l.hb + DMSH_C2_ROW * t < l.ib && l.hb + DMSH_C2_ROW * t + 4 <= DMSH_DRV_SIZE; t++) { uint32_t h = (uint32_t)vka_tex_slot(s, 5, t); memcpy(d + l.hb + DMSH_C2_ROW * t, &h, 4); }
    for (unsigned i = 0; l.ib + DMSH_C2_ROW * i < l.size && i < 32 && l.ib + DMSH_C2_ROW * i + 4 <= DMSH_DRV_SIZE; i++) { uint32_t h = (uint32_t)s->img[5][i]; memcpy(d + l.ib + DMSH_C2_ROW * i, &h, 4); }
}
void vka_rc_cs_c0(const vka_shader* sh, const vka_bstate* s, uint8_t* d) {
    if (sh && sh->nvn2_control) cs_c0_fill(sh->nvn2_control, s, d); else memset(d, 0, DMSH_DRV_SIZE);
}

// ---------- The draw's state as translation reads it ----------
typedef struct {
    const vka_dstate* d; int st; const rc_control* control; const uint8_t* code; uint32_t ncode;
    uint8_t drv[3][DMSH_DRV_SIZE], have[3], cs_c0[DMSH_DRV_SIZE], have_cs;
} rc_ctx;
static int rc_read(void* ctx, uint64_t addr, void* out, size_t n) {
    (void)ctx;
    if (vka_addr(addr, n, NULL, NULL, NULL)) { memcpy(out, (const void*)(uintptr_t)nvn_direct_host_of(addr), n); return 1; }
    return !plat_mem_read(addr, out, n);
}
static uint32_t rc_cbuf(void* ctx, uint32_t bank, uint32_t off) {
    rc_ctx* x = ctx; uint32_t v = 0;
    if (bank == 1) {   // the shader's own constants (rc_c1_range)
        uint32_t a, e; rc_c1_range(x->control, &a, &e);
        if (a <= e && e <= x->ncode && off + 4 <= e - a) memcpy(&v, x->code + a + off, 4);
        return v;
    }
    if (!x->d) return 0;
    if (bank == 0 && x->st == 5 && nvn2_control(x->control->bytes)) {
        if (!x->have_cs) { cs_c0_fill(x->control->bytes, &x->d->b, x->cs_c0); x->have_cs = 1; }
        if (off + 4 <= DMSH_DRV_SIZE) memcpy(&v, x->cs_c0 + off, 4);
        return v;
    }
    int i = bank == 0 ? 0 : bank == 2 ? 1 : bank == DMSH_PUSH_BANK ? 2 : -1;
    if (i >= 0) {
        if (!x->have[i]) { vka_drv_fill(&x->d->b, x->st, (int)bank, 0, x->drv[i]); x->have[i] = 1; }
        if (off + 4 <= vka_drv_size((int)bank)) memcpy(&v, x->drv[i] + off, 4);
        return v;
    }
    if (bank >= 3 && bank < 19) {
        const vka_range* u = &x->d->b.ubo[x->st][bank - 3];
        if (u->addr && (!u->size || off + 4 <= u->size) && !rc_read(NULL, u->addr + off, &v, 4)) v = 0;
    }
    return v;
}
int vka_tex_id_format(int id);   // vka_tex.c
static int rc_texture(void* ctx, uint32_t handle, uint32_t* target, int32_t* vk_format) {
    (void)ctx;
    int id = (int)(handle & 0xfffff), t = id ? vka_tex_id_target(id) : -1, f = id ? vka_tex_id_format(id) : -1;
    if (t < 0) return 0;
    vka_fmt vf; *target = (uint32_t)t; *vk_format = f >= 0 && vka_fmt_of(f, &vf) ? (int32_t)vf.view : 0;
    return 1;
}

// ---------- Descriptors as binding records ----------
static uint8_t dim_of(const nvn_direct_shader_desc* s) {
    static const uint8_t dim[9] = {0, 0 | DMSH_VK_DIM_ARRAY, 1, 1 | DMSH_VK_DIM_ARRAY, 2, 3, 3 | DMSH_VK_DIM_ARRAY, 5, 1};   // Shader::TextureType -> SPIR-V Dim
    uint8_t d = s->texture_type < 9 ? dim[s->texture_type] : 1;
    if (s->is_depth) d |= DMSH_VK_DIM_DEPTH;
    if (s->is_multisample) d |= DMSH_VK_DIM_MS;
    if (s->is_integer) d |= DMSH_VK_DIM_UINT;
    return d;
}
// A stage's storage buffer table in the fixed c[0] layout (NVN1 compute included)
static int c0_ssbo_table(int st) { return st == 0 ? DMSH_C0_SSBO_VS : st == 1 ? DMSH_C0_SSBO_PS : st == 2 ? DMSH_C0_SSBO_GS : st == 5 ? DMSH_C0_SSBO_CS : st == 6 ? DMSH_C0_SSBO_TS : st == 7 ? DMSH_C0_SSBO_MS : -1; }
// A handle word as an offset into the driver's c2 (NVN2 compute: its own handle table in c[0]); -1 none
static int c2_off(const cs_layout* l, uint32_t bank, uint32_t off) {
    if (bank == 2) return (int)off;
    if (l && bank == 0 && off >= l->hb && off < l->ib) return (int)(DMSH_C2_TEX + off - l->hb);
    if (l && bank == 0 && off >= l->ib && off < l->size) return (int)(DMSH_C2_IMG + off - l->ib);
    return -1;
}
// One descriptor's record; 0 = the replay cannot fill it (why says what), the record UNKNOWN. l: an NVN2 compute shader's c[0] layout, else NULL
static int rc_bind(const nvn_direct_shader_desc* s, int st, const cs_layout* l, dmsh_vk_bind* b, char* why, size_t whyn) {
    memset(b, 0, sizeof *b);
    b->desc = s->vk_type; b->binding = s->binding; b->set = DMSH_VK_SET_DRAW; b->count = (uint16_t)(s->count ? s->count : 1); b->reg = -1; b->sreg = -1; b->soff = 0xFFFF;
    b->kind = DMSH_VK_KIND_UNKNOWN;
    if (s->is_written) b->flags |= DMSH_VK_BF_RW;
    if (s->is_atomic) b->flags |= DMSH_VK_BF_ATOMIC;
#define NO(...) do { snprintf(why, whyn, __VA_ARGS__); return 0; } while (0)
    if (s->unsupported == 1) NO("binding %u: a handle computed per draw or a pointer read from memory (c%u + %#x)", s->binding, s->cbuf_index, s->cbuf_offset);
    switch (s->kind) {
    case NVN_DIRECT_DESC_CBUF:
        if (s->cbuf_index == 0) { b->kind = l ? VKA_KIND_NVN2_CS_C0 : DMSH_VK_KIND_DRVCB; b->reg = 0; }
        else if (s->cbuf_index == 1) b->kind = DMSH_VK_KIND_C1;
        else if (s->cbuf_index == 2 || s->cbuf_index == DMSH_PUSH_BANK) { b->kind = DMSH_VK_KIND_DRVCB; b->reg = (int16_t)s->cbuf_index; }
        else if (s->cbuf_index >= 3 && s->cbuf_index < 19) { b->kind = DMSH_VK_KIND_CBUF; b->reg = (int16_t)(s->cbuf_index - 3); }
        else NO("binding %u: constant buffer c%u", s->binding, s->cbuf_index);
        return 1;
    case NVN_DIRECT_DESC_STORAGE:
        if (s->cbuf_index == 0 && l && s->cbuf_offset >= l->sb && s->cbuf_offset < l->hb && !((s->cbuf_offset - l->sb) % DMSH_C0_ROW)) {
            b->kind = DMSH_VK_KIND_SBUF; b->reg = (int16_t)((s->cbuf_offset - l->sb) / DMSH_C0_ROW);
        } else if (s->cbuf_index == 0 && l && s->cbuf_offset >= l->ub && s->cbuf_offset < l->sb && !((s->cbuf_offset - l->ub) % DMSH_C0_ROW)) {
            b->kind = DMSH_VK_KIND_UBO_PTR; b->reg = (int16_t)((s->cbuf_offset - l->ub) / DMSH_C0_ROW);
        } else if (s->cbuf_index == 0 && !l && st == 5 && s->cbuf_offset >= DMSH_C0_CS_UBO && s->cbuf_offset < DMSH_C0_CS_UBO + DMSH_C0_ROW * DMSH_C0_CS_UBO_SLOTS &&
                   !((s->cbuf_offset - DMSH_C0_CS_UBO) % DMSH_C0_ROW)) {
            b->kind = DMSH_VK_KIND_UBO_PTR; b->reg = (int16_t)((s->cbuf_offset - DMSH_C0_CS_UBO) / DMSH_C0_ROW);
        } else if (s->cbuf_index == 0 && !l && c0_ssbo_table(st) >= 0 && s->cbuf_offset >= (uint32_t)c0_ssbo_table(st) &&
                   s->cbuf_offset < (uint32_t)c0_ssbo_table(st) + DMSH_C0_ROW * DMSH_C0_SSBO_SLOTS && !((s->cbuf_offset - c0_ssbo_table(st)) % DMSH_C0_ROW)) {
            b->kind = DMSH_VK_KIND_SBUF; b->reg = (int16_t)((s->cbuf_offset - c0_ssbo_table(st)) / DMSH_C0_ROW);
        } else if (s->cbuf_index >= 3 && s->cbuf_index < 19 && s->cbuf_offset < 0x10000) {
            b->kind = DMSH_VK_KIND_SBUF_UBO; b->reg = (int16_t)(s->cbuf_index - 3); b->off = (uint16_t)s->cbuf_offset;
        } else NO("binding %u: storage buffer pointer at c%u + %#x", s->binding, s->cbuf_index, s->cbuf_offset);
        return 1;
    case NVN_DIRECT_DESC_TEXTURE: case NVN_DIRECT_DESC_TEXTURE_BUFFER: {
        b->dim = dim_of(s);
        if (s->unsupported == 2) { b->kind = DMSH_VK_KIND_TEX; b->flags |= DMSH_VK_BF_NOSLOT; return 1; }   // a pointer word, never a handle: bound as nothing
        if (s->shift_left || (s->has_secondary && s->secondary_shift_left)) NO("binding %u: a shifted handle", s->binding);
        if (b->count > 1 && s->size_shift != 3) NO("binding %u: an array of handles %u bytes apart", s->binding, 1u << s->size_shift);
        int c2 = c2_off(l, s->cbuf_index, s->cbuf_offset);
        if (c2 >= 0) {
            if (s->kind == NVN_DIRECT_DESC_TEXTURE_BUFFER) {
                if (c2 < DMSH_C2_TEX || c2 >= DMSH_C2_SEPTEX || (c2 - DMSH_C2_TEX) % DMSH_C2_ROW) NO("binding %u: a texel buffer at c2 + %#x", s->binding, c2);
                b->kind = DMSH_VK_KIND_TBUF; b->reg = (int16_t)((c2 - DMSH_C2_TEX) / DMSH_C2_ROW); return 1;
            }
            b->kind = DMSH_VK_KIND_TEX_C2A; b->off = (uint16_t)c2; b->skind = DMSH_VK_KIND_SAMP_C2A;
            if (s->has_secondary) {
                int c2s = c2_off(l, s->secondary_index, s->secondary_offset);
                if (c2s < 0) NO("binding %u: a sampler word at c%u + %#x", s->binding, s->secondary_index, s->secondary_offset);
                b->soff = (uint16_t)c2s;
            }
            return 1;
        }
        if (s->cbuf_index >= 3 && s->cbuf_index < 19 && b->count == 1 && s->kind == NVN_DIRECT_DESC_TEXTURE) {
            b->kind = DMSH_VK_KIND_TEX_UBO; b->reg = (int16_t)(s->cbuf_index - 3); b->off = (uint16_t)s->cbuf_offset;
            b->skind = DMSH_VK_KIND_SAMP_UBO; b->sreg = b->reg; b->soff = b->off;
            if (s->has_secondary) {
                if (s->secondary_index < 3 || s->secondary_index >= 19) NO("binding %u: a sampler word at c%u + %#x", s->binding, s->secondary_index, s->secondary_offset);
                b->sreg = (int16_t)(s->secondary_index - 3); b->soff = (uint16_t)s->secondary_offset;
            }
            return 1;
        }
        NO("binding %u: a handle at c%u + %#x", s->binding, s->cbuf_index, s->cbuf_offset);
    }
    case NVN_DIRECT_DESC_LOOP_GUARD: b->kind = VKA_KIND_LOOP_GUARD; b->flags |= DMSH_VK_BF_RW; return 1;
    case NVN_DIRECT_DESC_IMAGE: case NVN_DIRECT_DESC_IMAGE_BUFFER: {
        b->dim = dim_of(s);
        int c2 = c2_off(l, s->cbuf_index, s->cbuf_offset);
        if (c2 < DMSH_C2_IMG || c2 >= DMSH_C2_IMG + DMSH_C2_ROW * 32 || (c2 - DMSH_C2_IMG) % DMSH_C2_ROW) NO("binding %u: an image handle at c%u + %#x", s->binding, s->cbuf_index, s->cbuf_offset);
        b->kind = DMSH_VK_KIND_IMG; b->reg = (int16_t)((c2 - DMSH_C2_IMG) / DMSH_C2_ROW);
        return 1;
    }
    }
    NO("binding %u: descriptor kind %u", s->binding, s->kind);
#undef NO
}

// ---------- A translated stage as a one-entry library ----------
static vka_shlib* rc_lib(uint64_t key, int vks, uint16_t flags, const nvn_direct_shader* o, const dmsh_vk_bind* b, unsigned nb, const uint8_t* c1, uint32_t c1_len, uint64_t vin_types, char* err, size_t errn) {
    size_t bind_off = DMSH_VK_HEADER_SIZE + DMSH_VK_ENTRY_SIZE, mod_off = (bind_off + (size_t)DMSH_VK_BIND_SIZE * nb + 7) & ~(size_t)7, blob_off = (mod_off + DMSH_VK_MOD_SIZE + 15) & ~(size_t)15;
    size_t spv = o->spirv_words * 4, c1_at = (spv + 15) & ~(size_t)15, size = blob_off + c1_at + c1_len;
    uint8_t* m = calloc(1, size); vka_shlib* L = calloc(1, sizeof *L);
    if (!m || !L) { free(m); free(L); snprintf(err, errn, "out of memory"); return NULL; }
    dmsh_vk_header* h = (dmsh_vk_header*)m;
    h->magic = DMSH_VK_LIB_MAGIC; h->version = DMSH_VK_LIB_VERSION;
    h->entry_count = 1; h->entry_off = DMSH_VK_HEADER_SIZE; h->bind_count = nb; h->bind_off = (uint32_t)bind_off; h->mod_count = 1; h->mod_off = (uint32_t)mod_off;
    h->blob_off = blob_off; h->blob_size = c1_at + c1_len; h->subgroup_size = 32; h->ssbo_align = (uint16_t)o->ssbo_align;
    dmsh_vk_entry* e = (dmsh_vk_entry*)(m + DMSH_VK_HEADER_SIZE);
    e->key = key; e->vin_types = vin_types; e->stage = (uint8_t)vks; e->flags = flags; e->mod_count = 1; e->bind_count = (uint16_t)nb;
    for (int i = 0; i < 3; i++) e->local[i] = (uint16_t)o->local[i];
    e->fout_mask = (uint8_t)o->output_mask; e->shared = o->shared; e->vin_mask = o->input_mask;
    e->c1_off = (uint32_t)c1_at; e->c1_len = c1_len;
    memcpy(m + bind_off, b, (size_t)DMSH_VK_BIND_SIZE * nb);
    dmsh_vk_module* md = (dmsh_vk_module*)(m + mod_off);
    md->size = md->stored = (uint32_t)spv; md->variant = DMSH_VK_MOD_BASE; md->codec = DMSH_VK_CODEC_RAW;
    memcpy(m + blob_off, o->spirv, spv);
    if (c1_len) memcpy(m + blob_off + c1_at, c1, c1_len);
    if (!vka_shlib_check(L, m, size, NULL, NULL, NULL, NULL, err, errn)) { free(m); free(L); return NULL; }
    return L;
}

// ---------- Translation ----------
typedef struct { vka_shader* sh; int st; uint32_t prim; } rc_shader;   // prim: a mesh shader's generic outputs written per primitive
// Lichii: the stages that place depth (vertex, geometry, tessellation evaluation, mesh: convert_depth), translated for one depth mode
static int rc_depth_stage(int st) { return st == 0 || st == 2 || st == 4 || st == 7; }
// The slot of a translation: its shader key, with a pixel shader's per-primitive inputs and a depth stage's depth mode. GL's (NVN's default, Mario Kart World's)
// keeps the key's own slot, which every translation had before the mode was part of it: those kept from earlier runs are placed by their record's gl_depth (disk_open)
static uint64_t rc_translation_slot(uint64_t key, uint32_t prim_inputs, int st, int gl_depth) {
    uint64_t slot = prim_inputs ? key ^ ((uint64_t)prim_inputs << 32 | 0x9e1u) : key;
    return rc_key(rc_depth_stage(st) && !gl_depth ? slot ^ 0xd3b7a1e5c4f20961ull : slot);
}
static _Atomic unsigned g_made, g_failed, g_unplaced, g_cached;
static int rc_vks(int st) { return vka_vk_stage(st); }
static unsigned rc_first_binding(int vks) { int r = vks == DMSH_VK_STAGE_CS || vks == DMSH_VK_STAGE_MS ? DMSH_VK_STAGE_VS : vks == DMSH_VK_STAGE_TS ? DMSH_VK_STAGE_TCS : vks; return (unsigned)r * DMSH_VK_STAGE_BINDINGS; }
// A stage translated with draw d's state: 1 with *o filled (given back with vka_shader_translated_free), *vin_types the vertex inputs' types
// it was made for (DMSH_VK_TYPE_* by location); gl_depth: the depth mode it is made for; else 0 and why
static int rc_translate(const prog_t* p, unsigned i, const rc_control* control, const vka_dstate* d, uint32_t prim_inputs, int gl_depth, nvn_direct_shader* o, uint64_t* vin_types, char* why, size_t whyn) {
    int st = (int)control->stage;
    uint32_t nctl, ncode; nvn_ctl_lens(control->bytes, &nctl, &ncode);
    uint64_t data = p->sd[i].data;
    *vin_types = 0;
    if (st > 7 || rc_vks(st) < 0) { snprintf(why, whyn, "NVN stage %d", st); return 0; }
    if (!data || !ncode || !vka_addr(data, ncode, NULL, NULL, NULL)) { snprintf(why, whyn, "its code area (%u bytes at %#llx) is not in a memory pool", ncode, (unsigned long long)data); return 0; }
    rc_ctx* x = calloc(1, sizeof *x); if (!x) { snprintf(why, whyn, "out of memory"); return 0; }
    x->d = d; x->st = st; x->control = control; x->code = (const uint8_t*)(uintptr_t)nvn_direct_host_of(data); x->ncode = ncode;
    nvn_direct_shader_source src = {.nvn_stage = (uint32_t)st, .control = control->bytes, .code = x->code, .code_size = ncode, .code_address = nvn_direct_guest_of(data),
                                    .first_binding = rc_first_binding(rc_vks(st)), .convert_depth = gl_depth, .per_primitive_inputs = st == 1 ? prim_inputs : 0, .context = x,
                                    .read_cbuf = rc_cbuf, .read_memory = rc_read, .texture_info = rc_texture};
    if (st == 0 && d)   // vertex inputs typed as the draw's attributes (the integer formats' classes): vka_draw.c then binds them retyping none
        for (int loc = 0; loc < 16 && loc < d->nattr; loc++) {
            vka_fmt vf; if (!d->attr[loc].fmt || !vka_fmt_of(d->attr[loc].fmt, &vf)) continue;
            src.vertex_input_types[loc] = vf.kind == 1 ? 2 : vf.kind == 2 ? 1 : 0;
            *vin_types |= (uint64_t)(vf.kind == 1 ? DMSH_VK_TYPE_UINT : vf.kind == 2 ? DMSH_VK_TYPE_SINT : DMSH_VK_TYPE_FLOAT) << (2 * loc);
        }
    int ok = vka_shader_translate(&src, o);
    free(x);
    if (!ok) { snprintf(why, whyn, "%s", o->error); return 0; }
    return 1;
}
// A translated stage's binding records (malloc'd, o->desc_count of them); why names the first the replay cannot fill (the record then UNKNOWN)
static dmsh_vk_bind* rc_binds(const nvn_direct_shader* o, int st, const rc_control* control, char* why, size_t whyn) {
    dmsh_vk_bind* b = calloc(o->desc_count ? o->desc_count : 1, sizeof *b);
    if (!b) { snprintf(why, whyn, "out of memory"); return NULL; }
    const int cs2 = st == 5 && nvn2_control(control->bytes);
    cs_layout l = cs2 ? cs_layout_of(control->bytes) : (cs_layout){0}; char w2[200] = "";
    for (unsigned k = 0; k < o->desc_count; k++) if (!rc_bind(&o->descs[k], st, cs2 ? &l : NULL, &b[k], w2, sizeof w2) && !*why) snprintf(why, whyn, "%s", w2);
    return b;
}

// ---------- Translations kept across runs ----------
// $DM_TEMP/vk_recompiled.bin: each translation as it was first made, by shader key (and per-primitive
// inputs), so the next run takes it as it is rather than translating again with that run's first draw's state, whose SPIR-V may differ
// (another texture bound when a slot's type is read) and miss the pipeline cache.
// The file is tied to NVN_RECOMPILE_STAMP, a hash of the sources a translation depends on made at build time
// (recompile_stamp.cmake), and to the device and driver (the recompiler's profile follows them), no longer to the executable's size and
// modification time: every build started it again, so the first run after one translated every shader on the frame path. A record also
// keeps the shader's control section (rc_control) and its own constants (c1, inside the code the key hashes), so a later run makes the shader
// before the game sets it: the prewarm list's pipelines are made at boot from these (vka_rc_by_key).
// Records: magic, size of the rest, slot, the shader key and the pixel shader's per-primitive inputs it was made for, gl_depth, per-primitive outputs,
// vertex input types, the nvn_direct_shader fields, c1's size, the rc_control's size, descriptors, SPIR-V, rc_control, c1. Lichii: also "LRCS" records, the header
// alone (key set, the rest 0): a shader translated speculatively (spec_main) in an earlier run, not done again
#define RC_FILE_MAGIC 0x3543524cu   // "LRC5"
#define RC_REC_MAGIC 0x5243524cu    // "LRCR"
#define RC_SPEC_MAGIC 0x5343524cu   // "LRCS"
typedef struct { uint32_t magic, size; uint64_t slot, key; uint32_t prim_inputs; int32_t gl_depth; uint32_t prim; uint64_t vin_types; uint32_t local[3], shared, input_mask, output_mask, ssbo_align, uses_render_area, desc_count, spirv_words, c1_len, ctl_len; } rc_rec;
static size_t rc_rec_rest(const rc_rec* r) { return (size_t)r->desc_count * sizeof(nvn_direct_shader_desc) + (size_t)r->spirv_words * 4 + r->ctl_len + r->c1_len; }
static const rc_control* rc_rec_control(const rc_rec* r) { return (const rc_control*)((const uint8_t*)r + sizeof *r + (size_t)r->desc_count * sizeof(nvn_direct_shader_desc) + (size_t)r->spirv_words * 4); }
static rc_map g_disk;      // slot -> rc_rec + its descriptors, SPIR-V, control section and c1 (malloc'd, kept)
static rc_map g_spec_done; // key -> 1: translated speculatively in an earlier run or this one
static FILE* g_disk_f; static int g_disk_state;   // 0 not opened, 1 open, -1 none
static void disk_open(void) {
    if (g_disk_state) return;
    g_disk_state = -1;
    const char* t = getenv("DM_TEMP");
    if (!t || !*t) return;
    // (and to the render scale, which RescalingPass bakes into the SPIR-V)
    const uint64_t stamp[2] = {NVN_RECOMPILE_STAMP, (uint64_t)V.props.vendorID << 48 ^ (uint64_t)V.props.deviceID << 32 ^ V.props.driverVersion ^
                                                    (vka_scale_on() ? ((uint64_t)vka_scale_up << 8 | vka_scale_shift) * 0x9E3779B97F4A7C15ull : 0)};
    char path[1024]; snprintf(path, sizeof path, "%s/vk_recompiled.bin", t);
    FILE* f = fopen(path, "rb"); unsigned n = 0; long good = 0, end = 0;
    if (f) {
        uint32_t magic = 0; uint64_t had[2] = {0, 0};
        if (fread(&magic, 4, 1, f) == 1 && magic == RC_FILE_MAGIC && fread(had, 8, 2, f) == 2 && had[0] == stamp[0] && had[1] == stamp[1]) {
            rc_rec r; good = ftell(f); unsigned ns = 0;
            while (fread(&r, sizeof r, 1, f) == 1 && (r.magic == RC_REC_MAGIC || r.magic == RC_SPEC_MAGIC)) {
                if (r.magic == RC_SPEC_MAGIC) { if (r.size != sizeof r) break; rc_put(&g_spec_done, rc_key(r.key), (void*)1); ns++; good = ftell(f); continue; }
                size_t rest = rc_rec_rest(&r);
                if (r.size != sizeof r + rest) break;
                uint8_t* m = malloc(sizeof r + rest); if (!m) break;
                memcpy(m, &r, sizeof r);
                if (fread(m + sizeof r, 1, rest, f) != rest) { free(m); break; }
                if (r.ctl_len < sizeof(rc_control) || rc_control_bytes(rc_rec_control((const rc_rec*)m)) != r.ctl_len) { free(m); break; }
                // (Lichii: by the slot of its depth mode: records written before the mode was part of the slot have the key's own in r.slot)
                rc_put(&g_disk, rc_translation_slot(r.key, r.prim_inputs, (int)rc_rec_control((const rc_rec*)m)->stage, r.gl_depth), m);
                n++; good = ftell(f);
            }
            if (ns) VLOG("recompiler: %u shaders translated speculatively in earlier runs (not done again)", ns);
            // A run killed while it appended a record leaves part of one at the end: cut there, or this run's records would follow it and never be read
            fseek(f, 0, SEEK_END); end = ftell(f);
        } else n = ~0u;
        fclose(f);
        if (n != ~0u && end > good) {
            VLOG("recompiler: %s ends in %ld bytes of a record not written whole (a run that ended while writing it): cut off", path, end - good);
            if (truncate(path, good)) VRED("recompiler: %s not cut (errno %d): translations made now are not read back", path, errno);
        }
    }
    if (n == ~0u || !f) {   // none yet, or another build's translations or device's: started again
        g_disk_f = fopen(path, "wb");
        if (g_disk_f) { uint32_t magic = RC_FILE_MAGIC; fwrite(&magic, 4, 1, g_disk_f); fwrite(stamp, 8, 2, g_disk_f); fflush(g_disk_f); }
        if (n == ~0u) VLOG("recompiler: %s holds another build's or device's translations: started again", path);
        n = 0;
    } else g_disk_f = fopen(path, "ab");
    if (!g_disk_f) { VRED("recompiler: %s not writable: translations are not kept", path); return; }
    g_disk_state = 1;
    VLOG("recompiler: %u translations read from %s (stamp %016llx)", n, path, (unsigned long long)stamp[0]);
}
// A kept translation as an nvn_direct_shader (its arrays point into the record: not to be freed), with the control section and c1 it was made from
static int disk_get(uint64_t slot, nvn_direct_shader* o, uint64_t* vin_types, int* gl_depth, uint32_t* prim, const rc_control** control, const uint8_t** c1, uint32_t* c1_len) {
    pthread_mutex_lock(&g_mu); disk_open(); rc_ent* e = rc_find(&g_disk, slot); uint8_t* m = e ? e->val : NULL; pthread_mutex_unlock(&g_mu);
    if (!m) return 0;
    const rc_rec* r = (const rc_rec*)m;
    memset(o, 0, sizeof *o);
    o->descs = (nvn_direct_shader_desc*)(m + sizeof *r); o->desc_count = r->desc_count;
    o->spirv = (uint32_t*)(m + sizeof *r + (size_t)r->desc_count * sizeof(nvn_direct_shader_desc)); o->spirv_words = r->spirv_words;
    memcpy(o->local, r->local, sizeof o->local); o->shared = r->shared; o->input_mask = r->input_mask; o->output_mask = r->output_mask; o->ssbo_align = r->ssbo_align; o->uses_render_area = r->uses_render_area;
    o->per_primitive_outputs = r->prim;
    *vin_types = r->vin_types; *gl_depth = r->gl_depth; *prim = r->prim;
    *control = rc_rec_control(r); *c1 = (const uint8_t*)*control + r->ctl_len; *c1_len = r->c1_len;
    return 1;
}
static void disk_put(uint64_t slot, uint64_t key, uint32_t prim_inputs, const nvn_direct_shader* o, uint64_t vin_types, int gl_depth, const rc_control* control, const uint8_t* c1, uint32_t c1_len) {
    rc_rec r = {RC_REC_MAGIC, 0, slot, key, prim_inputs, gl_depth, o->per_primitive_outputs, vin_types, {o->local[0], o->local[1], o->local[2]}, o->shared, o->input_mask, o->output_mask, o->ssbo_align,
                o->uses_render_area, o->desc_count, (uint32_t)o->spirv_words, c1_len, (uint32_t)rc_control_bytes(control)};
    r.size = (uint32_t)(sizeof r + rc_rec_rest(&r));
    pthread_mutex_lock(&g_mu); disk_open();
    if (g_disk_f) {
        fwrite(&r, sizeof r, 1, g_disk_f); fwrite(o->descs, sizeof *o->descs, o->desc_count, g_disk_f); fwrite(o->spirv, 4, o->spirv_words, g_disk_f);
        fwrite(control, 1, rc_control_bytes(control), g_disk_f); if (c1_len) fwrite(c1, 1, c1_len, g_disk_f); fflush(g_disk_f);
    }
    pthread_mutex_unlock(&g_mu);
}
// A shader translated speculatively (made or not): marked in the file, so later runs leave it
static void disk_spec_done(uint64_t key) {
    rc_rec r; memset(&r, 0, sizeof r); r.magic = RC_SPEC_MAGIC; r.size = sizeof r; r.key = key;
    pthread_mutex_lock(&g_mu); disk_open(); rc_put(&g_spec_done, rc_key(key), (void*)1);
    if (g_disk_f) { fwrite(&r, sizeof r, 1, g_disk_f); fflush(g_disk_f); }
    pthread_mutex_unlock(&g_mu);
}
// The kept translations' shader keys of NVN stage st (not the pixel shaders' per-primitive variants, valid only in mesh pipelines), malloc'd into *out: how many
// (vka_pso.c lib_ahead_start: their stage libraries are made ahead, so a pair the frame path has not met only needs the fast link)
unsigned vka_rc_stage_keys(int st, uint64_t** out) {
    pthread_mutex_lock(&g_mu); disk_open();
    unsigned n = 0; *out = malloc((g_disk.n ? g_disk.n : 1) * sizeof **out);
    for (size_t i = 0; *out && i < g_disk.cap; i++) {
        const rc_rec* r = g_disk.e[i].key ? g_disk.e[i].val : NULL;
        if (!r || r->prim_inputs) continue;
        if (rc_rec_control(r)->stage == (uint32_t)st) (*out)[n++] = r->key;
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}
int vka_rc_stage(uint64_t key) { const rc_control* c = rc_control_of(key); return c ? (int)c->stage : -1; }
uint32_t vka_rc_prim(uint64_t key) {
    pthread_mutex_lock(&g_mu); rc_ent* e = rc_find(&g_sh, rc_key(key)); rc_shader* r = e ? e->val : NULL; pthread_mutex_unlock(&g_mu);
    return r ? r->prim : 0;
}
// The one-entry libraries of the shaders made (their shaders read them until the teardown, vka_rc_destroy). Under g_mu
static vka_shlib** g_libs; static size_t g_nlibs, g_libs_cap;
// The shader of a translation, new or kept (o: its descriptors and SPIR-V; control and c1: what it was made from): r->sh, NULL when refused (why says what)
static _Atomic unsigned g_ahead;   // shaders made from kept translations before the game set them (vka_rc_by_key: the prewarm list)
static void rc_make(rc_shader* r, uint64_t key, const nvn_direct_shader* o, const rc_control* control, const uint8_t* c1, uint32_t c1_len, uint64_t vin_types, uint32_t prim_inputs, char* why, size_t whyn) {
    dmsh_vk_bind* b = rc_binds(o, r->st, control, why, whyn);
    if (*why) g_unplaced++;   // a record the replay cannot fill: the shader is refused (vka_shader.c refusal) after this line
    char err[200] = "";
    uint16_t flags = ((r->st == 1 && prim_inputs) || (r->st == 7 && r->prim) ? DMSH_VK_F_MESH_PRIM : 0) | (o->uses_render_area ? VKA_F_RENDER_AREA : 0);
    vka_shlib* L = b ? rc_lib(key, rc_vks(r->st), flags, o, b, o->desc_count, c1_len ? c1 : NULL, c1_len, vin_types, err, sizeof err) : NULL;
    free(b);
    if (!L) { if (!*why) snprintf(why, whyn, "%s", err); return; }
    pthread_mutex_lock(&g_mu);
    if (g_nlibs == g_libs_cap) { size_t nc = g_libs_cap ? 2 * g_libs_cap : 1024; vka_shlib** n = realloc(g_libs, nc * sizeof *n); if (n) { g_libs = n; g_libs_cap = nc; } }
    if (g_nlibs < g_libs_cap) g_libs[g_nlibs++] = L;
    pthread_mutex_unlock(&g_mu);
    r->sh = vka_shader_make(L, key);
    if (!r->sh) return;
    r->sh->nvn2_control = nvn2_control(control->bytes) ? control->bytes : NULL; g_made++;
    for (unsigned k = 0; k < o->desc_count; k++) if (o->descs[k].kind == NVN_DIRECT_DESC_TEXTURE) {   // (the descriptors' order is RescalingPass's texture order)
        r->sh->scale_tex_binding = o->descs[k].binding; r->sh->scale_tex_bit = (uint8_t)nvn_direct_scale_texture_bit((uint32_t)r->st);
        r->sh->scale_tex_bits = (uint8_t)nvn_direct_scale_texture_bits((uint32_t)r->st); break; }
    for (unsigned k = 0; k < r->sh->e->bind_count; k++) if (r->sh->b[k].kind == VKA_KIND_LOOP_GUARD && !r->sh->loop_guard.buf && vka_static(16, V.sb_align, &r->sh->loop_guard)) {
        memset(r->sh->loop_guard.ptr, 0, 16); guard_add(key, &r->sh->loop_guard); }
}
// A shader met for the first time in this run: kept from an earlier run, or translated now with the draw's state (caller holds g_make_mu)
static rc_shader* rc_new(const prog_t* p, unsigned i, uint64_t key, uint64_t slot, const rc_control* control, uint32_t prim_inputs, int gl_depth) {
    rc_shader* r = calloc(1, sizeof *r); r->st = (int)control->stage;
    nvn_direct_shader o; uint64_t vin_types = 0; char why[300] = "";
    uint64_t t0 = now_ns();
    const rc_control* kc; const uint8_t* c1; uint32_t c1_len; int kept_depth;
    int kept = disk_get(slot, &o, &vin_types, &kept_depth, &r->prim, &kc, &c1, &c1_len), ok = kept;
    if (!kept) {
        uint32_t a, c1e; rc_c1_range(control, &a, &c1e);
        c1 = (const uint8_t*)(uintptr_t)nvn_direct_host_of(p->sd[i].data) + a; c1_len = a < c1e ? c1e - a : 0;
        if ((ok = rc_translate(p, i, control, g_draw, prim_inputs, gl_depth, &o, &vin_types, why, sizeof why))) {
            r->prim = o.per_primitive_outputs;
            disk_put(slot, key, prim_inputs, &o, vin_types, gl_depth, control, c1, c1_len);
        }
    }
    if (ok) {
        if (kept) g_cached++;
        rc_make(r, key, &o, control, c1, c1_len, vin_types, prim_inputs, why, sizeof why);
        if (!kept) vka_shader_translated_free(&o);
        if (*why || !r->sh) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 64) VRED("recompiled shader %016llx (NVN stage %d): %s", (unsigned long long)key, r->st, *why ? why : "refused"); }
    } else {
        g_failed++;
        static _Atomic int n; if (atomic_fetch_add(&n, 1) < 64) VRED("shader %016llx (NVN stage %d) not translated: %s; its draws are skipped", (unsigned long long)key, r->st, why);
    }
    uint64_t ms = (now_ns() - t0) / 1000000;
    unsigned made = g_made, total = made + g_failed;
    if (ms >= 50 || total <= 3 || total % 256 == 0)
        VLOG("recompiler: %016llx (NVN stage %d) in %llu ms; %u made (%u kept from earlier runs, %u of them ahead), %u not translated, %u with bindings the replay cannot fill", (unsigned long long)key, r->st,
             (unsigned long long)ms, made, (unsigned)g_cached, (unsigned)g_ahead, (unsigned)g_failed, (unsigned)g_unplaced);
    pthread_mutex_lock(&g_mu); rc_put(&g_sh, slot, r); pthread_mutex_unlock(&g_mu);
    return r;
}
// Making a shader (the frame path translating it, a prewarm thread taking it from the file): one at a time, so a key gets one vka_shader, the one
// the pipelines made ahead were keyed with
static pthread_mutex_t g_make_mu = PTHREAD_MUTEX_INITIALIZER;
static rc_shader* rc_lookup(uint64_t slot) { pthread_mutex_lock(&g_mu); rc_ent* e = rc_find(&g_sh, slot); rc_shader* r = e ? e->val : NULL; pthread_mutex_unlock(&g_mu); return r; }
// The recompiled shader of program p's stage i (key), translated at its first use with the draw's state (or as an earlier run kept it); *st its
// NVN stage (-1: no control section kept for the key). prim_inputs: a pixel shader's generic inputs its program's mesh shader writes per primitive
// (PerPrimitiveEXT, flag DMSH_VK_F_MESH_PRIM: a shader of its own, valid only in a mesh pipeline). gl_depth: the depth mode a depth stage is translated
// for (each mode has a translation of its own). NULL: not translated or refused (logged once)
vka_shader* vka_rc_shader(const prog_t* p, unsigned i, uint64_t key, uint32_t prim_inputs, int gl_depth, int* st) {
    *st = -1;
    const rc_control* control = rc_control_of(key);
    if (!control) return NULL;
    uint64_t slot = rc_translation_slot(key, prim_inputs, (int)control->stage, gl_depth);
    rc_shader* r = rc_lookup(slot);
    if (!r) {
        pthread_mutex_lock(&g_make_mu);
        if (!(r = rc_lookup(slot))) r = rc_new(p, i, key, slot, control, prim_inputs, gl_depth);
        pthread_mutex_unlock(&g_make_mu);
    }
    *st = r->st;
    return r->sh;
}
// Only the key is known here (the prewarm), not its stage: in the near-zero depth mode a depth stage's slot is looked at first, then the key's own, which
// then counts only for a stage without depth (in GL's, the key's own slot holds both)
static rc_shader* rc_by_slot(uint64_t key, uint64_t slot, int depth_ok) {
    rc_shader* r = rc_lookup(slot);
    if (r) return depth_ok || !rc_depth_stage(r->st) ? r : NULL;
    // Lichii: not met in this run yet: made from the translation an earlier run kept (prewarm, pipelines made ahead), and kept for the frame path
    nvn_direct_shader o; uint64_t vin_types; const rc_control* control; const uint8_t* c1; uint32_t c1_len; int gl_depth; uint32_t prim;
    if (!disk_get(slot, &o, &vin_types, &gl_depth, &prim, &control, &c1, &c1_len) || (!depth_ok && rc_depth_stage((int)control->stage))) return NULL;
    r = calloc(1, sizeof *r); r->st = (int)control->stage; r->prim = prim;
    char why[300] = "";
    rc_make(r, key, &o, control, c1, c1_len, vin_types, 0, why, sizeof why);
    g_cached++;
    unsigned n = ++g_ahead;
    if (*why || !r->sh) { static _Atomic int k; if (atomic_fetch_add(&k, 1) < 64) VRED("recompiled shader %016llx (NVN stage %d, kept): %s", (unsigned long long)key, r->st, *why ? why : "refused"); }
    if (n == 1 || !(n & (n - 1))) VLOG("recompiler: %u shaders made ahead from kept translations (prewarm)", n);
    pthread_mutex_lock(&g_mu); rc_put(&g_sh, slot, r); pthread_mutex_unlock(&g_mu);
    return r;
}
vka_shader* vka_rc_by_key(uint64_t key, int gl_depth) {
    const uint64_t own = rc_key(key), depth = rc_translation_slot(key, 0, 0, gl_depth);
    rc_shader* r = rc_lookup(depth);
    if (!r) {
        pthread_mutex_lock(&g_make_mu);
        if (!(r = rc_by_slot(key, depth, 1)) && depth != own) r = rc_by_slot(key, own, 0);
        pthread_mutex_unlock(&g_make_mu);
    }
    return r ? r->sh : NULL;
}

// ---------- Speculative translations ----------
// Lichii: a vertex or pixel shader the game sets (nvnProgramSetShaders) that no earlier run kept is translated on a background thread without a draw's
// state (constant buffer words read as 0, no textures) and its stage library made in the most common state (vka_lib_ahead_shader), then let go of: only the pipeline cache
// keeps it. The draw still translates the shader with its own state and makes its own library, so what is drawn is always the draw's own translation; when the two SPIR-V
// modules are the same (2 in 3 of those made in time in mkw-race: about 2 in 3 vertex and 2 in 5 pixel shaders) that library is a cache hit instead of a compile on
// the frame path. Newest first: Mario Kart World sets ~4,000 programs and draws ~900, those it set last soonest. First-met content (cold, the NVIDIA disk cache off):
// libraries compiled on the frame path 0.54 -> 0.41-0.43 per shader made (615 / 596 -> 448-515 a race), frame-path creation 54-56 -> 47-50 s; warm runs unchanged.
// Each shader done is marked in vk_recompiled.bin (disk_spec_done), so a warm run does not translate again the ones the game sets and never draws.
// Threads: an eighth of the host's, 1 to 4 (nice 10).
typedef struct { uint64_t key, data; const rc_control* control; } spec_item;
static spec_item* g_spec; static size_t g_spec_n, g_spec_cap; static int g_spec_up;
static pthread_mutex_t g_spec_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_spec_cv = PTHREAD_COND_INITIALIZER;
static _Atomic unsigned g_spec_made, g_spec_skipped, g_spec_failed;
static _Atomic int g_spec_busy;
// One speculative translation (g_spec_busy counts it while it runs)
static void spec_one(spec_item it) {
    pthread_mutex_lock(&g_mu); disk_open(); int known = rc_find(&g_disk, it.key) || rc_find(&g_sh, it.key) || rc_find(&g_spec_done, it.key); pthread_mutex_unlock(&g_mu);
    if (known) { g_spec_skipped++; return; }   // kept from an earlier run (made ahead), met by a draw already, or done speculatively in an earlier run
    int st = (int)it.control->stage;
    prog_t p; memset(&p, 0, sizeof p); p.nstage = 1; p.sd[0].data = it.data;
    nvn_direct_shader o; uint64_t vin_types; char why[300]; why[0] = 0;
    if (!rc_translate(&p, 0, it.control, NULL, 0, !vka_depth_zero(), &o, &vin_types, why, sizeof why)) { g_spec_failed++; disk_spec_done(it.key); return; }
    dmsh_vk_bind* b = rc_binds(&o, st, it.control, why, sizeof why);
    uint32_t a0, c1e; rc_c1_range(it.control, &a0, &c1e); uint32_t c1_len = a0 < c1e ? c1e - a0 : 0;
    const uint8_t* c1 = (const uint8_t*)(uintptr_t)nvn_direct_host_of(it.data) + a0;
    char err[200]; err[0] = 0;
    vka_shlib* L = b && !*why ? rc_lib(it.key, rc_vks(st), 0, &o, b, o.desc_count, c1_len ? c1 : NULL, c1_len, vin_types, err, sizeof err) : NULL;
    free(b); vka_shader_translated_free(&o);
    vka_shader* s = L ? vka_shader_scratch(L, it.key) : NULL;
    if (s) vka_lib_ahead_shader(s, st == 0 ? 1 : 2);
    vka_shader_scratch_free(s);
    if (L) { free((void*)L->base); free(L); }
    disk_spec_done(it.key);
    if (!s) { g_spec_failed++; return; }
    unsigned n = ++g_spec_made;
    if (n == 1 || !(n & (n - 1))) VLOG("recompiler: %u shaders translated speculatively and their libraries made (%u kept or drawn already, %u not translated without a draw)", n, (unsigned)g_spec_skipped, (unsigned)g_spec_failed);
}
// The emulator shuts down (vka_stop): the threads drop what is queued and end, as what they use (the game's memory, Lichii's recompiler and device) goes away
static void* spec_main(void* a) {
    (void)a; setpriority(PRIO_PROCESS, 0, 10);
    for (;;) {
        pthread_mutex_lock(&g_spec_mu);
        while (!g_spec_n && !vka_stopping()) pthread_cond_wait(&g_spec_cv, &g_spec_mu);
        if (vka_stopping()) { g_spec_n = 0; pthread_mutex_unlock(&g_spec_mu); return NULL; }
        spec_item it = g_spec[--g_spec_n];   // the newest first: the game draws what it set last soonest
        atomic_fetch_add(&g_spec_busy, 1);
        pthread_mutex_unlock(&g_spec_mu);
        spec_one(it);
        atomic_fetch_sub(&g_spec_busy, 1);
    }
    return NULL;
}
// nvn_direct_park (after vka_stop): wakes the speculative threads and waits (bounded) until none translates. 1 = none does
static void spec_wake(void) { pthread_mutex_lock(&g_spec_mu); pthread_cond_broadcast(&g_spec_cv); pthread_mutex_unlock(&g_spec_mu); }
int vka_rc_park(uint64_t timeout_ns) {
    spec_wake();
    const uint64_t t0 = now_ns();
    while (atomic_load(&g_spec_busy) && now_ns() - t0 < timeout_ns) usleep(1000);
    return !atomic_load(&g_spec_busy);
}
static void spec_add(uint64_t key, uint64_t data, const rc_control* control) {
    static int on = -1;
    if (on < 0) {   // (32 host threads: 4, 8: 1)
        long hw = sysconf(_SC_NPROCESSORS_ONLN); on = hw >= 32 ? 4 : hw >= 16 ? (int)(hw / 8) : 1;
    }
    if (control->stage > 1) return;
    pthread_mutex_lock(&g_spec_mu);
    if (g_spec_n == g_spec_cap) { size_t nc = g_spec_cap ? 2 * g_spec_cap : 1024; spec_item* n = realloc(g_spec, nc * sizeof *n); if (!n) { pthread_mutex_unlock(&g_spec_mu); return; } g_spec = n; g_spec_cap = nc; }
    g_spec[g_spec_n++] = (spec_item){key, data, control};
    for (; g_spec_up < on; g_spec_up++) vka_thread(spec_main, NULL, spec_wake);
    pthread_cond_signal(&g_spec_cv);
    pthread_mutex_unlock(&g_spec_mu);
}

// The teardown (vka_dev.c vka_destroy, after every thread left): the tables, the kept translations, the libraries and the file
static void rc_map_free(rc_map* m, int values) {
    if (values) for (size_t i = 0; i < m->cap; i++) if (m->e[i].key) free(m->e[i].val);
    free(m->e);
}
void vka_rc_destroy(void) {
    for (size_t i = 0; i < g_nlibs; i++) { free((void*)g_libs[i]->base); free(g_libs[i]); }
    free(g_libs);
    rc_map_free(&g_sh, 1); rc_map_free(&g_ctl, 1); rc_map_free(&g_disk, 1); rc_map_free(&g_spec_done, 0);
    free(g_guards); free(g_spec);
    if (g_disk_f) fclose(g_disk_f);
}

// Shaders made in this run, and of them the ones an earlier run kept (the rest were translated now: new to this run's DM_TEMP)
void vka_rc_counts(unsigned* made, unsigned* kept) { *made = g_made; *kept = g_cached; }
