// From Banana Native (github.com/hugorosario/BananaNative, Android port of github.com/seedds/nxnative by seedds), MIT: see LICENSE in src/video_core/nvn_direct/bananative.
// Draws and dispatches of the Vulkan replay: nvn_mtl.m's draw path on Vulkan. vka_exec.c walks the records; state records land in the queue's vka_dstate (vka_state_op),
// and each draw resolves its program to shaders (vka_shader.c), opens the dynamic-rendering pass of the bound targets if none is open, takes a pipeline keyed on the state
// Vulkan cannot set dynamically here (vka_pso.c), writes set 0 from the binding records (vka_bind.c's walker: push descriptors when they fit, else a set from the submission
// slot's pool), sets the dynamic state and draws. The pass stays open across draws and closes at anything else (vka_rp_end), followed by a full barrier.
// What is not here yet, each logged once: SINT attachments and integer vertex formats (no variants), transform feedback, DrawTexture, line loops and quads through indirect draws,
// texel buffers, viewport swizzles other than a y flip without VK_NV_viewport_swizzle
#if defined(__ANDROID__)
#include "vka.h"
#include "nvn_direct_front.h"
#include "nvn_direct.h"   // Lichii: the render scale's push constants
#include "../nvn/nvn_mtl.h"
#include "../../host/plat/plat.h"
#include <math.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static VkCommandBuffer g_cb; static unsigned g_slot; static int g_prof, g_dyn_filter = 1, g_pass_keep = 1, g_vb_filter = 1;   // g_prof: DM_VK_PROF
uint64_t vka_n_rp;   // render passes the draws opened (vka_exec.c replay statistics)
static uint64_t g_draws, g_skipped, g_dispatches;
uint64_t vka_skip_line[2048];   // x86-64: skipped draws by the line that skipped them
// Lichii: the draws skipped since the last call, by the vka_draw.c line that skipped them (the replay thread writes, this reads: approximate)
void vka_skip_report(void) {
    static uint64_t last[2048]; char b[512]; size_t o = 0; uint64_t all = 0;
    for (int i = 0; i < 2048; i++) { uint64_t d = vka_skip_line[i] - last[i]; last[i] += d; all += d;
        if (d && o < sizeof b - 32) o += (size_t)snprintf(b + o, sizeof b - o, " line %d: %llu", i, (unsigned long long)d); }
    VLOG("draws skipped since the last report: %llu%s", (unsigned long long)all, o ? b : "");
}
// Render-target sizes (whether a game's dynamic resolution shrinks its scene targets or viewports): passes at least 640 wide by render area, first viewport and whether they
// have depth, since the last report (every 1800 presents, nvn_present.c)
enum { NRTS = 64 };
static struct { uint32_t w, h, vw, vh, depth; uint64_t n; } g_rts[NRTS];
static uint64_t g_rts_other;
static void rts_note(uint32_t w, uint32_t h, const vka_dstate *d, int depth) {
    if (w < 640) return;
    const uint32_t vw = d->has_vp ? (uint32_t)fabsf(d->vp[0][2]) : 0, vh = d->has_vp ? (uint32_t)fabsf(d->vp[0][3]) : 0;
    for (int i = 0; i < NRTS; i++) {
        if (!g_rts[i].n) { g_rts[i].w = w; g_rts[i].h = h; g_rts[i].vw = vw; g_rts[i].vh = vh; g_rts[i].depth = (uint32_t)depth; g_rts[i].n = 1; return; }
        if (g_rts[i].w == w && g_rts[i].h == h && g_rts[i].vw == vw && g_rts[i].vh == vh && g_rts[i].depth == (uint32_t)depth) { g_rts[i].n++; return; }
    }
    g_rts_other++;
}
void vka_rt_size_report(void) {
    char b[700]; size_t o = 0;
    for (int t = 0; t < 10; t++) {
        int best = -1; for (int i = 0; i < NRTS; i++) if (g_rts[i].n && (best < 0 || g_rts[i].n > g_rts[best].n)) best = i;
        if (best < 0) break;
        if (o < sizeof b) o += (size_t)snprintf(b + o, sizeof b - o, " %ux%u vp %ux%u%s x%llu;", g_rts[best].w, g_rts[best].h, g_rts[best].vw, g_rts[best].vh, g_rts[best].depth ? " +depth" : "", (unsigned long long)g_rts[best].n);
        g_rts[best].n = 0;
    }
    VLOG("render passes >= 640 wide since the last report (area, first viewport):%s others %llu", o ? b : " none;", (unsigned long long)g_rts_other);
    memset(g_rts, 0, sizeof g_rts); g_rts_other = 0;
}
static uint32_t g_claim_words;   // the draw's claim words (an upper bound of its primitives): 0 = its geometry shader takes no claim
#define SKIP(...) do { g_skipped++, vka_skip_line[__LINE__ & 2047]++; static _Atomic int n_; if (atomic_fetch_add(&n_, 1) < 4) VRED(__VA_ARGS__); return; } while (0)

// Whether draws and dispatches run. Without placed guest memory (an emulator's driver) the GPU sees no memory pool, so a game's draws would read nothing and only cost:
// pipeline compiles there take minutes each. Off there by default, on with placed memory; DM_VK_DRAW=0 / 1 overrides, the selftest turns them on for its own memory
static int g_force_draws;
void vka_draw_force(int on) { g_force_draws = on; }
static int draws_on(void) {
    int env = VKA_ENV("DM_VK_DRAW", -1); if (env > 0) env = 1;
    int on = g_force_draws || (env >= 0 ? env : V.placed || V.host_import);   // Lichii: or imported pools (vka_mem.c)
    if (!on) VRED_ONCE("draws and dispatches are off (%s): their records are skipped; events, counters and fences still signal", env == 0 ? "DM_VK_DRAW=0" : "no placed guest memory, so the GPU would see no pool; DM_VK_DRAW=1 turns them on");
    return on;
}

// ---------- State ----------
void vka_dstate_reset(vka_dstate *d) {   // NVN's defaults (nvn_mtl.m state_reset)
    memset(d, 0, sizeof *d); d->inited = 1;
    for (int i = 0; i < 8; i++) d->blend[i] = (blend_st){i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    d->color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; d->chmask.mask = 0xffffffffu;
    d->poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    d->ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    d->st_vmask[0] = d->st_vmask[1] = d->st_wmask[0] = d->st_wmask[1] = 0xff; d->dr[1] = 1; d->b.samples = 1;
    d->sample_mask = 0xffff; d->patch = 3; d->tess_inner[0] = d->tess_inner[1] = 1; for (int i = 0; i < 4; i++) d->tess_outer[i] = 1;   // (Lichii)
    memset(d->vrs_pal, 5, sizeof d->vrs_pal);   // one invocation a pixel
}
static int cpu_read(uint64_t addr, void *out, size_t n);
// Fritchii: Ocarina of Time's fog. The froxel pass (OOT_FOG_CS) and the sky average before it in the same submission (1bba09ec4bc8ff67, which adds into it)
// bind a 192-byte block of the sky's light colours (binding 4). Each frame the game clears part of it on the CPU after submitting them, and a later submission
// on another queue fills it again. On hardware they run before the clear; the replay runs them later, so they read the cleared block and the fog loses its
// distant haze. So each submission of their queue keeps a copy of the block as the game submitted it (vka_fog_snapshot, from mtl_sub_commit, before the worker
// records it), and that submission's bindings of the block use the copy. The copy is the block as hardware would read it: the game waits for the refill just before
#define OOT_FOG_CS 0x9e6b00b050e30801ull
enum { FOG_SLOTS = 64, FOG_SLOT = 256, FOG_BLOCK = 192 };
static vka_span g_fog_ring;
static _Atomic uint64_t g_fog_host, g_fog_serial, g_fog_tag[FOG_SLOTS]; static _Atomic unsigned g_fog_newest = ~0u;
void vka_fog_snapshot(uint64_t serial, uint64_t fence) {
    const uint64_t h = atomic_load(&g_fog_host);
    if (!h || serial != atomic_load(&g_fog_serial)) return;
    const unsigned i = (unsigned)(fence % FOG_SLOTS);
    uint8_t *slot = (uint8_t *)g_fog_ring.ptr + (size_t)i * FOG_SLOT;
    memcpy(slot, (const void *)(uintptr_t)h, FOG_BLOCK);
    {   // The refill (21d9f83e6027c565, another queue) may run before the game's clear instead of after it, as the acquire it waits for on hardware is not
        // waited for here: the clear then wipes it and the block is submitted with no light colours (a frame of darker shade and fog). Those rows
        // (+48, 60 bytes) are then the last ones a snapshot found: they follow the sky and change slowly
        static uint8_t last[60]; static int have; static uint64_t reused;
        const float *c = (const float *)(slot + 48);
        if (c[0] != 0.f || c[1] != 0.f || c[2] != 0.f) { memcpy(last, slot + 48, sizeof last); have = 1; }
        else if (have) { memcpy(slot + 48, last, sizeof last);
            if (VKA_ENV("FRITCHII_FOG_LOG", 0) && reused++ < 50) VLOG("fog snapshot at fence %llu: the light colours were cleared; the last ones are used", (unsigned long long)fence); }
    }
    atomic_store(&g_fog_tag[i], fence); atomic_store(&g_fog_newest, i);
}
static void fog_bind(uint64_t key, unsigned binding, uint64_t addr, VkDescriptorBufferInfo *bi) {
    static int on = -1;
    if (on < 0) { int fritchii_oot_fog_snapshot(void); on = fritchii_oot_fog_snapshot(); }
    if (!on) return;
    uint64_t vka_exec_queue_serial(void), vka_exec_cur_fence(void);
    uint64_t h = atomic_load(&g_fog_host); const uint64_t ah = nvn_direct_host_of(addr);
    if (!h) {   // learnt from the froxel pass's binding
        if (key != OOT_FOG_CS || binding != 4) return;
        if (!vka_static(FOG_SLOTS * FOG_SLOT, FOG_SLOT, &g_fog_ring)) { on = 0; VRED("Ocarina of Time: no memory for the fog's light colours: the fog reads them as they are"); return; }
        atomic_store(&g_fog_serial, vka_exec_queue_serial()); atomic_store(&g_fog_host, h = ah);
        VLOG("Ocarina of Time: the fog's light colours are read as the game submitted them");
    }
    if (ah != h || bi->range > FOG_SLOT || vka_exec_queue_serial() != atomic_load(&g_fog_serial)) return;
    const uint64_t f = vka_exec_cur_fence(); const unsigned i = (unsigned)(f % FOG_SLOTS);
    unsigned use = i;
    if (atomic_load(&g_fog_tag[i]) != f) {   // no copy for this submission (taken before the slot was known): the newest one, not the live block
        use = atomic_load(&g_fog_newest);
        if (VKA_ENV("FRITCHII_FOG_LOG", 0)) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 50) VLOG("fog snapshot missing for fence %llu: slot %u used", (unsigned long long)f, use); }
        if (use >= FOG_SLOTS) return;
    }
    *bi = (VkDescriptorBufferInfo){g_fog_ring.buf, g_fog_ring.off + (VkDeviceSize)use * FOG_SLOT, bi->range};
}
// Lichii: a deferred bind (Bind*Deferred) as the memory at its source holds it now, applied as the bind it names (hle_nvn.c bind_handles' pool bases)
static void bind_deferred(vka_dstate *d, const p_bind_defer *b) {
    if (b->stage < 0 || b->stage >= VKA_NSTAGE || b->n <= 0 || b->n > 32) return;
    uint64_t w[64]; const int buf = b->kind == DEFER_UBO || b->kind == DEFER_SSBO;
    if (!cpu_read(b->src, w, (buf ? 16 : 8) * (size_t)b->n)) { VRED_ONCE("deferred bind at %#llx is not readable: not bound", (unsigned long long)b->src); return; }
    for (int i = 0; i < b->n; i++) {
        const int s = b->first + i;
        if (buf) { if (s < 16) (b->kind == DEFER_UBO ? d->b.ubo : d->b.ssbo)[b->stage][s] = (vka_range){nvn_direct_host_of(w[2 * i]), w[2 * i + 1]};
                   if (w[2 * i] >> 40) { static _Atomic int nl; if (atomic_fetch_add(&nl, 1) < 8) VLOG("deferred %s bind stage %d slot %d from %#llx: %#llx+%#llx", b->kind == DEFER_UBO ? "ubo" : "ssbo", b->stage, s, (unsigned long long)b->src, (unsigned long long)w[2 * i], (unsigned long long)w[2 * i + 1]); }
                   continue; }
        uint64_t v = (uint32_t)w[i], tid = v & 0xfffff, sid = v >> 20;
        if (b->kind == DEFER_SAMP) { if (s < 128) d->b.samp[b->stage][s] = sid ? (sid + (uint64_t)b->sbase) << 20 : v; continue; }
        if (tid) tid += (uint64_t)b->tbase;
        if (sid && b->kind == DEFER_TEX) sid += (uint64_t)b->sbase;
        if (b->kind == DEFER_IMG) { if (s < 32) d->b.img[b->stage][s] = tid | sid << 20; }
        else if (s < VKA_NTEX) d->b.tex[b->stage][s] = tid | sid << 20;
    }
}
int nvn_dpool_base(const void *pool, int tex);   // hle_nvn.c: where a descriptor pool's IDs start in the host table
int vka_state_op(vka_dstate *d, uint32_t op, const void *a) {
    if (!d->inited) vka_dstate_reset(d);
    switch (op) {
    case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: { int tex = op == OP_SET_TEX_POOL, b = nvn_dpool_base(((const p_set_pool *)a)->pool, tex); if (tex) d->b.tex_base = b; else d->b.samp_base = b; break; }
    case OP_BIND_PROGRAM: { const p_bind_program *b = a;
        if (b->stages & 0x20) d->cprog = b->prog;
        for (int st = 0; st < VKA_NSTAGE; st++) if (st != 5 && (b->stages >> st & 1)) d->bst[st] = b->prog;   // NVN binds per stage (nvn_mtl.m bind_stages); 6 / 7: NVN2's task and mesh
        break; }
    case OP_BIND_STATE: {
        const p_bind_state *b = a; blend_st bs;
        switch (b->kind) {
        case 0: memcpy(&bs, b->bytes, sizeof bs); d->blend[bs.target] = bs; break;
        case 1: memcpy(&d->color, b->bytes, sizeof d->color); break;
        case 2: memcpy(&d->chmask, b->bytes, sizeof d->chmask); break;
        case 3: memcpy(&d->ms, b->bytes, sizeof d->ms); break;
        case 4: memcpy(&d->poly, b->bytes, sizeof d->poly); break;
        case 5: memcpy(&d->ds, b->bytes, sizeof d->ds); break;
        }
        break; }
    case OP_BIND_VATTR: { const p_bind_vattr *b = a; if (b->first >= 0 && b->first + b->n <= 16) { memcpy(&d->attr[b->first], b->st, 4 * (size_t)b->n); if (b->first + b->n > d->nattr) d->nattr = b->first + b->n; } break; }
    case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; if (b->first >= 0 && b->first + b->n <= 16) { memcpy(&d->stream[b->first], b->st, 8 * (size_t)b->n); if (b->first + b->n > d->nstream) d->nstream = b->first + b->n; } break; }
    case OP_BIND_VBUF: case OP_BIND_VBUF1: { p_bind_vbuf tmp; const p_bind_vbuf *b = vbuf_rec(op, a, &tmp); if (b->first >= 0 && b->first + b->n <= 16) memcpy(&d->vbuf[b->first], b->r, sizeof *b->r * (size_t)b->n); break; }
    case OP_BIND_UBO: case OP_BIND_UBO_SMALL: { p_bind_buf tmp; const p_bind_buf *b = bind_buf_rec(op, a, &tmp); if (b->stage >= 0 && b->stage < VKA_NSTAGE && b->index >= 0 && b->index < 16) d->b.ubo[b->stage][b->index] = (vka_range){b->addr, b->size}; break; }
    case OP_BIND_SSBO: { const p_bind_buf *b = a; if (b->stage >= 0 && b->stage < VKA_NSTAGE && b->index >= 0 && b->index < 16) d->b.ssbo[b->stage][b->index] = (vka_range){b->addr, b->size}; break; }
    case OP_BIND_TEX: case OP_BIND_TEX1: { p_bind_handles t1; const p_bind_handles *b = bind_rec(op, a, &t1);
        if (b->n < 0 || b->n > 32) { VRED_ONCE("texture bind record with count %d: overwritten after recording", b->n); break; }
        uint64_t h[32]; memcpy(h, b->h, 8 * (size_t)b->n);
        if (b->defer && b->src && b->n) {   // BindTexturesDeferred: the handles as memory holds them now (nvn_mtl.m bh_fresh); placed guest memory read directly, else word by word through the fault-safe read
            uint32_t w[32];
            if (vka_addr(b->src, 4u * (uint64_t)b->n, NULL, NULL, NULL)) { memcpy(w, (const void *)(uintptr_t)b->src, 4 * (size_t)b->n); for (int k = 0; k < b->n; k++) h[k] = w[k]; }
            else if (!plat_mem_read(b->src, w, 4 * (size_t)b->n)) for (int k = 0; k < b->n; k++) h[k] = w[k];   // one read for the run (they are in the game's heap; profile p2: 3.4 % in process_vm_readv)
            else for (int k = 0; k < b->n; k++) if (!plat_mem_read(b->src + 4u * k, &w[k], 4)) h[k] = w[k];   // a run across an unreadable page: word by word
        }
        if (b->stage >= 0 && b->stage < VKA_NSTAGE && b->first >= 0 && b->first + b->n <= VKA_NTEX) memcpy(&d->b.tex[b->stage][b->first], h, 8 * (size_t)b->n);
        break; }
    case OP_BIND_SAMP: { const p_bind_handles *b = a; if (b->stage >= 0 && b->stage < VKA_NSTAGE && b->first >= 0 && b->n >= 0 && b->first + b->n <= 128) memcpy(&d->b.samp[b->stage][b->first], b->h, 8 * (size_t)b->n); break; }
    case OP_BIND_IMG: { const p_bind_handles *b = a; if (b->stage >= 0 && b->stage < VKA_NSTAGE && b->first >= 0 && b->n >= 0 && b->first + b->n <= 32) memcpy(&d->b.img[b->stage][b->first], b->h, 8 * (size_t)b->n); break; }
    case OP_STENCIL: { const p_stencil *b = a;
        for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) d->st_vmask[f] = (uint32_t)b->value_mask; else if (b->which == 1) d->st_wmask[f] = (uint32_t)b->mask; else d->st_ref[f] = (uint32_t)b->ref; }
        break; }
    case OP_POLY_OFFSET: d->po = *(const p_poly_offset *)a; break;
    case OP_VIEWPORTS: { const p_viewports *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) memcpy(d->vp[b->first + i], b->v[i], 16); d->has_vp = 1; break; }
    case OP_DEPTH_RANGE: { const p_depth_range *b = a; d->dr[0] = b->n; d->dr[1] = b->f; break; }
    case OP_DEPTH_CLAMP: d->depth_clamp = ((const p_depth_clamp *)a)->on; break;
    case OP_DEPTH_BOUNDS: d->dbounds = *(const p_depth_bounds *)a; break;
    case OP_VP_SWIZZLE: { const p_vp_swizzle *b = a; if (b->first >= 0 && b->first + b->n <= 16) memcpy(d->vp_sw[b->first], b->sw, 4 * (size_t)b->n); break; }
    case OP_BLEND_COLOR: memcpy(d->blend_color, ((const p_blend_color *)a)->rgba, 16); break;
    case OP_ALPHA_REF: d->alpha_ref = ((const p_alpha_ref *)a)->ref; break;
    case OP_PUSH_CONST: { const p_push_const *b = a; if ((uint32_t)b->offset + b->size <= DMSH_PUSH_SIZE) memcpy(d->b.push[b->compute ? 1 : 0] + b->offset, b->data, b->size); break; }
    case OP_TF_BIND: break;
    case OP_TF_STATE: { const p_tf_state *b = a; d->tf_on = b->what == 0 || b->what == 3; break; }
    case OP_PRIM_RESTART: d->restart = *(const p_prim_restart *)a; break;
    case OP_RAST_DISCARD: d->rast_discard = ((const p_rast_discard *)a)->on; break;
    case OP_PATCH_SIZE: d->patch = ((const p_patch_size *)a)->n; break;
    case OP_TESS_LEVELS: { const p_tess_levels *b = a; if (b->outer) memcpy(d->tess_outer, b->v, sizeof d->tess_outer); else memcpy(d->tess_inner, b->v, sizeof d->tess_inner); break; }
    case OP_SAMPLE_MASK: d->sample_mask = ((const p_sample_mask *)a)->mask; break;
    case OP_RASTER: { const p_raster *b = a;
        switch (b->what) {
        case RASTER_CONSERVATIVE: d->cons = b->a; break;
        case RASTER_DILATE: d->dilate = b->f; break;
        case RASTER_CONSERVATIVE_MODE: d->cons_mode = b->a; break;
        case RASTER_SUBPIXEL_BIAS: d->subpixel_bias[0] = b->a; d->subpixel_bias[1] = b->b; break;
        case RASTER_PROVOKING: d->provoking = b->a; break;
        }
        break; }
    case OP_EXCL_SCISSOR: { const p_excl_scissor *b = a;
        if (!b->what) d->excl_on = b->first;
        else if (b->first >= 0 && b->n >= 0 && b->first + b->n <= 16) { memcpy(d->excl[b->first], b->r, 16 * (size_t)b->n); if (b->first + b->n > d->excl_n) d->excl_n = b->first + b->n; }
        break; }
    case OP_VRS_IMAGE: d->vrs = *(const p_vrs_image *)a; break;
    case OP_VRS_PALETTE: { const p_vrs_palette *b = a; memcpy(d->vrs_pal[b->viewport & 15], b->rate, 16);
        static _Atomic int noted; int coarse = 0; for (int i = 0; i < 16; i++) coarse |= b->rate[i] != 5;
        if (coarse && !atomic_exchange(&noted, 1)) VLOG("shading-rate palette: the first with rates other than one invocation a pixel, viewport %d: %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u", b->viewport,
                                                        b->rate[0], b->rate[1], b->rate[2], b->rate[3], b->rate[4], b->rate[5], b->rate[6], b->rate[7], b->rate[8], b->rate[9], b->rate[10], b->rate[11], b->rate[12], b->rate[13], b->rate[14], b->rate[15]);
        break; }
    case OP_BIND_DEFER: bind_deferred(d, a); break;
    case OP_PUSH_DEFER: { const p_push_defer *b = a;
        if ((uint32_t)b->offset + b->size <= DMSH_PUSH_SIZE && !cpu_read(b->src, d->b.push[b->compute ? 1 : 0] + b->offset, b->size)) VRED_ONCE("deferred push constants at %#llx are not readable: not pushed", (unsigned long long)b->src);
        break; }
    default: return 0;
    }
    return 1;
}

// ---------- Static buffers: zeros, a write sink, the support buffer ----------
static vka_span g_zero, g_sink, g_support;
static int statics(void) {
    static int ok = -1; if (ok >= 0) return ok;
    ok = vka_static(65536, V.ub_align > V.sb_align ? V.ub_align : V.sb_align, &g_zero) && vka_static(65536, V.sb_align, &g_sink) && vka_static(VKA_SUPPORT_SIZE, V.ub_align, &g_support);
    if (ok) { memset(g_zero.ptr, 0, 65536); memset(g_sink.ptr, 0, 65536); vka_support_fill(g_support.ptr, VKA_SUPPORT_SIZE); }
    return ok;
}

// ---------- Descriptor sets that do not fit a push (per submission slot) ----------
enum { NSLOT = 8, MAXPOOLS = 64 };
static VkDescriptorPool g_dp[NSLOT][MAXPOOLS]; static int g_ndp[NSLOT], g_cur_dp[NSLOT];
static VkDescriptorBufferInfo g_mesh_out;   // Fritchii: the mesh emulation draw's output range (VKA_R_MESH_OUT, mesh_fb_compute)
static uint64_t g_pre_want[16], g_pre_seen[16]; static uint32_t g_pre_done;   // Fritchii: SET_RT ordinals with emulated mesh draws (the frame before, this one)
void vka_dyn_invalidate(void);
static void drv_memo_reset(void);
static void fsc_reset(void);
void vka_draw_sub_begin(VkCommandBuffer cb, unsigned slot) {
    g_cb = cb; g_slot = slot % NSLOT; vka_dyn_invalidate();
    { void vka_mesh_fb_sub_begin(unsigned); vka_mesh_fb_sub_begin(g_slot); }   // (Fritchii: the slot's mesh emulation output, given back with its fence)
    { void vka_mem_rec_thread(void); vka_mem_rec_thread(); }   // the address lookups' cache outside TLS for the recording thread (vka_mem.c)
    drv_memo_reset();   // the last submission's scratch spans go with its fence
    g_prof = VKA_ENV("DM_VK_PROF", 0) != 0; g_dyn_filter = VKA_ENV("DM_VK_DYN_FILTER", 1); g_pass_keep = VKA_ENV("DM_VK_PASS_KEEP", 1); g_vb_filter = VKA_ENV("DM_VK_VB_FILTER", 1);   // live: from this submission
    for (int i = 0; i < g_ndp[g_slot]; i++) vk.vkResetDescriptorPool(V.dev, g_dp[g_slot][i], 0);   // the slot's last submission is done (mtl_sub_begin waited for it)
    g_cur_dp[g_slot] = 0;
    fsc_reset();   // (Lichii: its sets were the slot's)
    vka_pso_save(0);
    {   // Fritchii: a 1x1x1 white 3D image, for a small 2D texture a shader samples as 3D (vka_tex.c view_of: the game binds its 4x4 white texture to unused 3D slots, which the
        // Switch reads as white; with no view at all the slot read zero, and OoT's translucent water came out magenta from the fog volumes it samples)
        extern VkImageView vka_white3d; static int made;
        if (!made && g_cb) {
            made = 1; VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkMemoryRequirements mr;
            VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_3D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = 1,
                                    .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
            if (!vk.vkCreateImage(V.dev, &ci, NULL, &img)) {
                vk.vkGetImageMemoryRequirements(V.dev, img, &mr);
                VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
                if (ai.memoryTypeIndex != UINT32_MAX && !vk.vkAllocateMemory(V.dev, &ai, NULL, &mem) && !vk.vkBindImageMemory(V.dev, img, mem, 0)) {
                    vka_img_barrier(g_cb, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                    const VkClearColorValue one = {{1, 1, 1, 1}}; const VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    vk.vkCmdClearColorImage(g_cb, img, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &rg); vka_barrier(g_cb);
                    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_3D, .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = rg};
                    VkImageView v = VK_NULL_HANDLE; if (!vk.vkCreateImageView(V.dev, &vi, NULL, &v)) vka_white3d = v;
                }
            }
            VLOG("white 3D stand-in for small 2D textures sampled as 3D: %s", vka_white3d ? "made" : "not made");
        }
    }
    static int ph; VkImageView placeholder(uint8_t dim, int storage);
    if (!V.null_desc && !ph && statics()) {   // no null descriptors: the placeholders are made here, outside any pass (their first use records a clear)
        ph = 1; static const uint8_t dims[] = {1, 2, 3, 1 | DMSH_VK_DIM_ARRAY, 3 | DMSH_VK_DIM_ARRAY};
        for (size_t i = 0; i < sizeof dims; i++) for (int c = 0; c < 3; c++) placeholder(dims[i] | (c == 1 ? DMSH_VK_DIM_SINT : c == 2 ? DMSH_VK_DIM_UINT : 0), 0);
    }
}
int vka_dpool_count(void) { int n = 0; for (int i = 0; i < NSLOT; i++) n += g_ndp[i]; return n; }   // (Lichii: the VRAM line)
static VkDescriptorSet set_alloc(VkDescriptorSetLayout l) {
    for (;;) {
        int i = g_cur_dp[g_slot];
        if (i == g_ndp[g_slot]) {
            if (i == MAXPOOLS) { VRED_ONCE("descriptor pools exhausted in one submission"); return VK_NULL_HANDLE; }
            VkDescriptorPoolSize ps[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8192}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192},
                                         {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024},
                                         {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 256}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 256}};
            VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1024, .poolSizeCount = sizeof ps / sizeof *ps, .pPoolSizes = ps};
            if (vk.vkCreateDescriptorPool(V.dev, &pi, NULL, &g_dp[g_slot][i])) { VRED_ONCE("descriptor pool not made"); return VK_NULL_HANDLE; }
            g_ndp[g_slot]++;
        }
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_dp[g_slot][i], .descriptorSetCount = 1, .pSetLayouts = &l};
        VkDescriptorSet s; VkResult r = vk.vkAllocateDescriptorSets(V.dev, &ai, &s);
        if (!r) return s;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) { VRED_ONCE("vkAllocateDescriptorSets %d", r); return VK_NULL_HANDLE; }
        g_cur_dp[g_slot]++;
    }
}

// ---------- The render pass ----------
static struct {
    int on; VkRect2D area; uint32_t layers, samples;
    uint32_t ncolor; VkFormat cf[8]; uint8_t ckind[8]; VkFormat df, sf;
    vka_img *cim[8]; uint32_t clv[8], cl0[8]; int nci;   // the colour attachments (DM_VK_NANSCAN)
    p_set_rt rt; int has_rt;                             // the targets it was opened for: a later SET_RT of the same targets keeps it open
    uint32_t cnl[8], dnl; VkExtent2D cext[8], dext;      // each attachment's layers and level size (clears in the pass must cover what they would outside it)
    int swrite;                                          // a draw of the pass wrote memory (storage buffers or images, raw global stores, a geometry shader's claim)
    int bar_pending;                                     // a game barrier came inside the pass and was taken there (vka_rp_game_barrier): the draws after it are checked (bar_hazard)
    int scaled;                                          // Lichii: its targets are at the render scale: viewports and scissors scaled
    VkExtent2D own;                                      // Lichii: the area in the targets' own texels (not scaled): the render area shaders read
    int again;                                           // Lichii: every target was drawn in the frames just before too (drawn_again): only such draws are skipped while their pipeline is made asynchronously
    int vrs; uint64_t vrs_serial; view_t vrs_view; uint8_t vrs_has_view, vrs_rate[16];   // Lichii: its shading-rate attachment's image and palette (vrs_key)
 uint32_t setrt; } R;
// Lichii: the render scale's push constants of the draw or dispatch being bound (nvn_direct.h NVN_DIRECT_SCALE_PUSH_SIZE: the scaled textures' bits, the
// pixel stage's down factor; no image is scaled: vka_tex.c img_unscale), and the last pushed, which stays while every layout has the same push constant range (vka_dyn_invalidate forgets it)
static struct { uint32_t tex[4], img[2]; float down, pad; } g_scale_push;
static struct { typeof(g_scale_push) p; int valid; } g_scale_pushed;
// Lichii: the render area pushed last (nvn_direct.h NVN_DIRECT_RENDER_AREA_OFFSET), for draws whose shaders read it (VKA_F_RENDER_AREA)
static struct { float a[4]; int valid; } g_area_pushed;
_Static_assert(sizeof g_scale_push == NVN_DIRECT_SCALE_PUSH_SIZE && offsetof(typeof(g_scale_push), img) == NVN_DIRECT_SCALE_IMAGES_OFFSET && offsetof(typeof(g_scale_push), down) == NVN_DIRECT_SCALE_DOWN_FACTOR_OFFSET, "RescalingLayout");
// Clears waiting for the pass of their targets (vka_rp_clear), in order: the next draw over the same targets opens its pass with them (loadOp CLEAR, or vkCmdClearAttachments),
// anything else does them outside a pass first (pc_flush), as before
enum { NPC = 16 };
static struct { int n; p_set_rt rt; vka_pclear c[NPC]; } PC;
static void pc_flush(void) { int n = PC.n; PC.n = 0; for (int i = 0; i < n; i++) vka_clear_outside(&PC.c[i]); }
// The same targets: the same textures and the same views of them (field by field: the records' padding is not compared)
static int view_same(const view_t *a, const view_t *b) {
    return a->set_levels == b->set_levels && a->set_layers == b->set_layers && a->set_format == b->set_format && (!a->set_levels || (a->base_level == b->base_level && a->nlevels == b->nlevels)) &&
           (!a->set_layers || (a->min_layer == b->min_layer && a->nlayers == b->nlayers)) && (!a->set_format || a->format == b->format);
}
static int rt_same(const p_set_rt *a, int ha, const p_set_rt *b, int hb) {
    if (!ha || !hb) return !ha && !hb;
    if (a->n != b->n || a->depth != b->depth || !!a->has_dview != !!b->has_dview || (a->has_dview && !view_same(&a->dview, &b->dview))) return 0;
    for (int i = 0; i < a->n && i < 8; i++) {
        if (a->color[i] != b->color[i] || !!a->has_cview[i] != !!b->has_cview[i]) return 0;
        if (a->color[i] && a->has_cview[i] && !view_same(&a->cview[i], &b->cview[i])) return 0;
    }
    return 1;
}
// ---------- Fritchii: drawing into block-compressed textures ----------
// A game compresses a texture on the GPU by drawing into it through an uncompressed view of one texel a block (Ocarina of Time: a 64x64 BC4 texture through an
// RGBA16_UINT view). Vulkan cannot draw into a compressed image, so the pass draws into a stand-in image of the view's format at block size, filled from the texture
// before the pass and copied back into it after (vkCmdCopyImage between size-compatible uncompressed and compressed formats). Level 0, one layer
enum { NBCRT = 16 };
static struct { vka_img *im; uint64_t serial; VkFormat f; VkImage img; VkDeviceMemory mem; VkImageView view; uint32_t bw, bh; } g_bcrt[NBCRT];
static struct { vka_img *im; int k; } g_bcrt_pass[8]; static int g_nbcrt_pass;   // the pass's stand-ins, copied back at its end (vka_rp_end)
static int bc_dims(VkFormat f, uint32_t *bw, uint32_t *bh, uint32_t *bytes) {   // block size of a compressed format, 0 = not one handled
    *bw = *bh = 4;
    switch (f) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK: *bytes = 8; return 1;
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK: case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK: *bytes = 16; return 1;
    default: return 0;
    }
}
static void bc_copy(VkImage src, VkImage dst, uint32_t w, uint32_t h) {   // w, h: the source's texels
    VkImageCopy c = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vk.vkCmdCopyImage(g_cb, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}
// The stand-in of compressed image im for a view of format f (bytes a texel): its view, filled from im (no pass open); VK_NULL_HANDLE = none
static VkImageView bcrt_begin(vka_img *im, const vka_fmt *f, int *slot) {
    uint32_t bw, bh, bytes; if (!bc_dims(im->f.vk, &bw, &bh, &bytes) || f->kind >= 3 || (uint32_t)f->bpb != bytes || im->samples > 1) return VK_NULL_HANDLE;
    const uint32_t w = (im->w + bw - 1) / bw, h = (im->h + bh - 1) / bh;
    int k = -1, free_k = -1;
    for (int i = 0; i < NBCRT; i++) { if (g_bcrt[i].im == im && g_bcrt[i].serial == im->serial && g_bcrt[i].f == f->vk) { k = i; break; } if (!g_bcrt[i].im && free_k < 0) free_k = i; }
    if (k < 0) {
        if (free_k < 0) { VRED_ONCE("more than 16 block-compressed textures drawn into: the others are not drawn into"); return VK_NULL_HANDLE; }
        k = free_k;
        VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f->vk, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkMemoryRequirements mr; uint32_t t = UINT32_MAX;
        if (!vk.vkCreateImage(V.dev, &ii, NULL, &img)) { vk.vkGetImageMemoryRequirements(V.dev, img, &mr); t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0); }
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = t == UINT32_MAX ? 0 : mr.size, .memoryTypeIndex = t};
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f->vk, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (!img || t == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &mem) || vk.vkBindImageMemory(V.dev, img, mem, 0) || ((vi.image = img), vk.vkCreateImageView(V.dev, &vi, NULL, &view))) {
            VRED_ONCE("a stand-in for drawing into a block-compressed texture not made: not drawn into");
            if (img) vk.vkDestroyImage(V.dev, img, NULL);
            if (mem) vk.vkFreeMemory(V.dev, mem, NULL);
            return VK_NULL_HANDLE; }
        vka_mem_grew(mr.size);
        vka_img_barrier(g_cb, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        g_bcrt[k] = (typeof(g_bcrt[0])){im, im->serial, f->vk, img, mem, view, w, h};
        static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VLOG("drawing into block-compressed texture serial %llu (%ux%u VkFormat %d) through a %ux%u stand-in of VkFormat %d", (unsigned long long)im->serial, im->w, im->h, im->f.vk, w, h, f->vk);
    }
    bc_copy(im->img, g_bcrt[k].img, im->w, im->h);   // (the texture's blocks as the stand-in's texels: the pass may draw only some)
    vka_barrier(g_cb);
    *slot = k;
    return g_bcrt[k].view;
}
// Game barriers (OP_BARRIER) that came with a pass open: taken inside it (DM_VK_BAR_KEEP), ended it at once, ended it later at a draw that needed it (bar_hazard)
static uint64_t g_bar_in, g_bar_kept, g_bar_late;
void vka_rp_end(void) {
    if (PC.n) pc_flush();
    if (!R.on) return;
    { void vka_oq_pass_end(VkCommandBuffer cb); vka_oq_pass_end(g_cb); }   // Lichii: samples-passed counters (vka_exec.c)
    vk.vkCmdEndRendering(g_cb);
    if (VKA_ENV("DM_VK_RP_BAR", 1))   // Fritchii: after the pass's own stages and writes only (vka_barrier_from)
        vka_barrier_from(g_cb, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT);   // the pass's writes before whatever reads them next (copies, the present, the next pass's samples). DM_VK_RP_BAR=0: only the game's barriers
    R.on = 0; vka_tex_feedback(NULL, NULL, 0);
    if (g_nbcrt_pass) {   // Fritchii: the stand-ins back into their block-compressed textures (bcrt_begin)
        for (int i = 0; i < g_nbcrt_pass; i++) { const int k = g_bcrt_pass[i].k; if (g_bcrt[k].im == g_bcrt_pass[i].im) bc_copy(g_bcrt[k].img, g_bcrt[k].im->img, g_bcrt[k].bw, g_bcrt[k].bh); }
        g_nbcrt_pass = 0; vka_barrier(g_cb);
    }
    if (vka_nan_on()) vka_nan_pass(g_cb, R.cim, R.clv, R.cl0, R.nci);
}
static void rt_range(const vka_img *im, const view_t *v, int has_v, uint32_t *lv, uint32_t *l0, uint32_t *nl) {
    *lv = has_v && v->set_levels ? v->base_level : 0;
    *l0 = has_v && v->set_layers ? v->min_layer : 0;
    if (!vka_img_lv(im, lv)) { *lv = im->levels; *nl = 0; return; }   // a level the image leaves out (a sparse texture's unmapped levels)
    uint32_t all = vka_img_slices(im, *lv);
    *nl = has_v && v->set_layers && v->nlayers ? v->nlayers : all - (*l0 < all ? *l0 : 0);
    if (*l0 + *nl > all) *nl = *l0 < all ? all - *l0 : 0;
}
// Lichii: a pass over scaled targets and targets at their own size (beside a small target, or one made at its own size again) draws them all at their own size
static void rp_scale_targets(const p_set_rt *rt, int has_rt) {
    if (!has_rt) return;
    vka_img *ims[9]; int n = 0, scaled = 0;
    for (int i = 0; i < rt->n && i < 8; i++) if (rt->color[i]) { vka_img *im = vka_tex_get(rt->color[i], g_cb); if (im) { ims[n++] = im; scaled += im->scaled; } }
    if (rt->depth) { vka_img *im = vka_tex_get(rt->depth, g_cb); if (im) { ims[n++] = im; scaled += im->scaled; } }
    if (!scaled || scaled == n) return;
    if (PC.n) pc_flush();   // (their rectangles are in their images' sizes)
    for (int i = 0; i < n; i++) if (ims[i]->scaled) vka_tex_unscale(ims[i], g_cb, "a pass with targets at their own size");
}
// Lichii: an attachment's level size in its texture's own texels (lw, lh: the image's), into the smallest so far
static void own_min(const vka_img *im, uint32_t lv, uint32_t lw, uint32_t lh, uint32_t *ow, uint32_t *oh) {
    if (im->scaled) { lw = im->d.width >> lv ? im->d.width >> lv : 1; lh = im->d.height >> lv ? im->d.height >> lv : 1; }
    if (lw < *ow) *ow = lw; if (lh < *oh) *oh = lh;
}
// Lichii: marks im drawn in this frame (the presents the replay filled so far); returns whether a pass drew into it in one of the 3 frames before too
// (targets the game draws every frame, double or triple buffered)
static int drawn_again(vka_img *im) {
    uint64_t vka_present_filled(void); const uint64_t f = vka_present_filled() + 1;
    if (im->drawn[0] != f) { im->drawn[1] = im->drawn[0]; im->drawn[0] = f; }
    return im->drawn[1] && im->drawn[1] + 3 >= f;
}
// ---------- Variable-rate shading (Lichii) ----------
// NVN's shading-rate image (BindShadingRateImage) holds a palette index per 16x16-pixel tile, and a viewport's palette (ShadingRateImagePalette) the rates, as
// VK_NV_shading_rate_image's VkShadingRatePaletteEntryNV. A pass whose draws have an image bound gets a VK_KHR_fragment_shading_rate attachment made from them just
// before it begins (vulkan_nvn_direct_shading_rate.comp): per tile the rate of its index in viewport 0's palette (the replay draws with one viewport). The pipelines
// replace their own rate with the attachment's (vka_pso.c). One attachment per submission slot (slots may run at once on other queues), at most VRS_MAX texels a side
#include "video_core/host_shaders/vulkan_nvn_direct_shading_rate_comp_spv.h"
enum { VRS_TEXEL = 16, VRS_MAX = 1024 };
static struct { VkImage img; VkDeviceMemory mem; VkImageView view; } g_vrs_att[NSLOT];
static struct { VkDescriptorSetLayout dsl; VkPipelineLayout layout; VkPipeline pipe; int failed; } g_vrs;
typedef struct { const tex_t *tex; uint64_t serial; view_t view; uint8_t has_view, rate[16]; } vrs_key;
// A palette entry as a Vulkan rate ((log2 width) << 2 | log2 height): no invocations and several a pixel have none, they shade at full rate
static uint8_t vrs_rate(uint8_t e) {
    static const uint8_t rate[12] = {[6] = 4, [7] = 1, [8] = 5, [9] = 6, [10] = 9, [11] = 10};
    if (e != 5 && (e < 6 || e > 11)) VRED_ONCE("shading-rate palette entry %u (no invocations, or several a pixel) has no Vulkan rate: shaded at full rate", e);
    return e < 12 ? rate[e] : 0;
}
static int vrs_want(const vka_dstate *d, vrs_key *k) {
    if (!VKA_ENV("DM_VK_VRS", 1)) { memset(k, 0, sizeof *k); return 0; }   // DM_VK_VRS=0: no shading-rate attachment (full rate)
    memset(k, 0, sizeof *k);
    if (!d->vrs.tex || d->rast_discard) return 0;
    if (d->vrs.tex->magic != MAGIC('T', 'E', 'X', 0)) { VRED_ONCE("the bound shading-rate image's snapshot is no longer a texture (its command memory was reused): drawn at full rate"); return 0; }
    if (!V.vrs) { VRED_ONCE("a shading-rate image is bound and the device has no VK_KHR_fragment_shading_rate attachment of 16x16-pixel texels: drawn at full rate"); return 0; }
    k->tex = d->vrs.tex; k->serial = d->vrs.tex->serial; k->has_view = d->vrs.has_view; if (k->has_view) k->view = d->vrs.view;
    for (int i = 0; i < 16; i++) k->rate[i] = vrs_rate(d->vrs_pal[0][i]);
    return 1;
}
static int vrs_same(const vrs_key *k, int on) {
    return (R.vrs != 0) == on && (!on || (R.vrs_serial == k->serial && R.vrs_has_view == k->has_view && (!k->has_view || view_same(&R.vrs_view, &k->view)) && !memcmp(R.vrs_rate, k->rate, 16)));
}
static int vrs_pipeline(void) {
    if (g_vrs.pipe || g_vrs.failed) return g_vrs.pipe != VK_NULL_HANDLE;
    g_vrs.failed = 1;
    const VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = V.push_desc ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0, .bindingCount = 2, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof VULKAN_NVN_DIRECT_SHADING_RATE_COMP_SPV, .pCode = VULKAN_NVN_DIRECT_SHADING_RATE_COMP_SPV};
    VkShaderModule m = VK_NULL_HANDLE; VkResult r = vk.vkCreateDescriptorSetLayout(V.dev, &li, NULL, &g_vrs.dsl);
    VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_vrs.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (!r) r = vk.vkCreatePipelineLayout(V.dev, &pi, NULL, &g_vrs.layout);
    if (!r) r = vk.vkCreateShaderModule(V.dev, &mi, NULL, &m);
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}, .layout = g_vrs.layout};
    if (!r) r = vk.vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, &g_vrs.pipe);
    if (m) vk.vkDestroyShaderModule(V.dev, m, NULL);
    if (r) { VRED("shading-rate attachment pipeline: %d; shading-rate images are drawn at full rate", r); g_vrs.pipe = VK_NULL_HANDLE; return 0; }
    g_vrs.failed = 0; return 1;
}
static VkImageView vrs_attachment(void) {
    typeof(g_vrs_att[0]) *a = &g_vrs_att[g_slot];
    if (a->view) return a->view;
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8_UINT, .extent = {VRS_MAX, VRS_MAX, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR};
    VkMemoryRequirements mr; uint32_t t;
    if (vk.vkCreateImage(V.dev, &ii, NULL, &a->img)) { VRED_ONCE("shading-rate attachment not made: drawn at full rate"); a->img = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
    vk.vkGetImageMemoryRequirements(V.dev, a->img, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = a->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8_UINT, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (t == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &a->mem) || vk.vkBindImageMemory(V.dev, a->img, a->mem, 0) || vk.vkCreateImageView(V.dev, &vi, NULL, &a->view)) {
        VRED_ONCE("shading-rate attachment memory not made: drawn at full rate"); a->view = VK_NULL_HANDLE; return VK_NULL_HANDLE; }   // (kept: not tried again for this slot)
    vka_mem_grew(mr.size);
    vka_img_barrier(g_cb, a->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    return a->view;
}
// The slot's attachment filled for a pass of this area (no pass open): VK_NULL_HANDLE = the pass draws at full rate
static VkImageView vrs_fill(const vrs_key *k, VkExtent2D area) {
    const uint32_t tw = (area.width + VRS_TEXEL - 1) / VRS_TEXEL, th = (area.height + VRS_TEXEL - 1) / VRS_TEXEL;
    if (tw > VRS_MAX || th > VRS_MAX) { VRED_ONCE("a pass of %ux%u with a shading-rate image: larger than the attachment's %d texels a side, drawn at full rate", area.width, area.height, VRS_MAX); return VK_NULL_HANDLE; }
    vka_img *im = vka_tex_get(k->tex, g_cb); if (!im) return VK_NULL_HANDLE;
    if (im->f.kind != 1 || !(im->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) { VRED_ONCE("shading-rate image (format %#x) is not a sampled unsigned integer texture: drawn at full rate", im->d.format); return VK_NULL_HANDLE; }
    uint32_t lv = k->has_view && k->view.set_levels ? k->view.base_level : 0, l0 = k->has_view && k->view.set_layers ? k->view.min_layer : 0;
    if (!vka_img_lv(im, &lv) || l0 >= im->layers) return VK_NULL_HANDLE;
    VkImageView src = vka_view(im, im->f.view, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, lv, 1, l0, 1, NULL), att = vrs_attachment();
    if (!src || !att || !vrs_pipeline()) return VK_NULL_HANDLE;
    VkDescriptorImageInfo ii[2] = {{vka_samp_default(), src, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, att, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[0]},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii[1]}};
    vk.vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_vrs.pipe);
    if (V.push_desc) vk.vkCmdPushDescriptorSetKHR(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_vrs.layout, 0, 2, w);
    else { VkDescriptorSet s = set_alloc(g_vrs.dsl); if (!s) return VK_NULL_HANDLE;
           w[0].dstSet = w[1].dstSet = s; vk.vkUpdateDescriptorSets(V.dev, 2, w, 0, NULL); vk.vkCmdBindDescriptorSets(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_vrs.layout, 0, 1, &s, 0, NULL); }
    // An index texel covers 16x16 of the texture's own pixels: with the render scale, the pass's texels are scaled pixels, and the index image may or may not be scaled
    const float s = (float)vka_scale_up / (float)(1u << vka_scale_shift);
    struct { uint8_t rate[16]; uint32_t size[2]; float index_scale; int32_t level; } pc = {.size = {tw, th}, .index_scale = (im->scaled ? s : 1.0f) / (R.scaled ? s : 1.0f)};
    memcpy(pc.rate, k->rate, 16);
    vk.vkCmdPushConstants(g_cb, g_vrs.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vk.vkCmdDispatch(g_cb, (tw + 7) / 8, (th + 7) / 8, 1);
    vka_barrier(g_cb);
    vka_dyn_invalidate();   // (the compute pipeline's push constants and descriptors disturb the draws' ones)
    static _Atomic int noted;
    if (!atomic_exchange(&noted, 1)) VLOG("shading-rate attachment: first pass %ux%u, from texture serial %llu (%ux%u format %#x%s), viewport 0's rates %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                                          area.width, area.height, (unsigned long long)k->serial, im->w, im->h, im->d.format, im->scaled ? ", scaled" : "", k->rate[0], k->rate[1], k->rate[2], k->rate[3],
                                          k->rate[4], k->rate[5], k->rate[6], k->rate[7], k->rate[8], k->rate[9], k->rate[10], k->rate[11]);
    return att;
}
// Opens the pass over the bound targets: 0 = it cannot (no target can be an attachment), logged
static int rp_begin(const p_set_rt *rt, int has_rt, const vka_dstate *d) {
    vrs_key vk_; const int vrs = vrs_want(d, &vk_);
    if (R.on) { if ((!g_pass_keep || rt_same(rt, has_rt, &R.rt, R.has_rt)) && vrs_same(&vk_, vrs)) return 1; vka_rp_end(); }   // a new SET_RT of other targets ends the pass at its first draw (exec does not)
    if (PC.n && !rt_same(rt, has_rt, &PC.rt, 1)) pc_flush();                             // clears of other targets: done before
    if (vka_scale_on()) rp_scale_targets(rt, has_rt);
    VkRenderingAttachmentInfo ca[8], da = {0}, sa; uint32_t w = UINT32_MAX, h = UINT32_MAX, layers = UINT32_MAX, samples = 0, ow = UINT32_MAX, oh = UINT32_MAX;
    vka_img *fb_im[9]; uint32_t fb_lv[9]; int nfb = 0;   // the attachments' images and levels (vka_tex_feedback)
    memset(&R, 0, sizeof R); g_nbcrt_pass = 0;
    if (has_rt) R.rt = *rt; R.has_rt = has_rt;
    int n = has_rt ? (rt->n < 8 ? rt->n : 8) : 0;
    for (int i = 0; i < n; i++) {
        ca[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        R.cf[i] = VK_FORMAT_UNDEFINED;
        const tex_t *t = rt->color[i]; if (!t) continue;
        vka_img *im = vka_tex_get(t, g_cb); vka_fmt f;
        {   // Fritchii, DM_VK_RT_TRACE=1: each distinct render target (the game's texture and the image the replay attaches), to compare with the normal renderer's passes
            static int on = -1; if (on < 0) on = (int)VKA_ENV("DM_VK_RT_TRACE", 0);
            static uint64_t seen[512]; static int ns;
            if (on && im) { uint64_t key = im->serial << 8 | (uint64_t)i; int k = 0; for (; k < ns && seen[k] != key; k++) {}
                if (k == ns && ns < 512) { seen[ns++] = key; VLOG("rt trace: slot %d game texture %ux%u format %#x serial %llu storage %#llx -> image serial %llu %ux%u VkFormat %d", i, t->d.width, t->d.height, t->d.format,
                    (unsigned long long)t->serial, (unsigned long long)nvn_direct_guest_of((uint64_t)(uintptr_t)t->mem), (unsigned long long)im->serial, im->w, im->h, (int)im->f.vk); } }
        }
        if (!im || !vka_fmt_of(rt->has_cview[i] && rt->cview[i].set_format ? rt->cview[i].format : t->d.format, &f)) continue;
        if (!(im->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && f.kind < 3 && g_nbcrt_pass < 8) {   // Fritchii: a block-compressed texture (above rp_begin)
            int k; VkImageView v = bcrt_begin(im, &f, &k);
            if (v) {
                ca[i].imageView = v; R.cf[i] = f.view; R.ckind[i] = f.kind; samples = 1;
                g_bcrt_pass[g_nbcrt_pass].im = im; g_bcrt_pass[g_nbcrt_pass++].k = k;
                R.cnl[i] = 1; R.cext[i] = (VkExtent2D){g_bcrt[k].bw, g_bcrt[k].bh};
                if (g_bcrt[k].bw < w) w = g_bcrt[k].bw; if (g_bcrt[k].bh < h) h = g_bcrt[k].bh; if (1 < layers) layers = 1;
                if (g_bcrt[k].bw < ow) ow = g_bcrt[k].bw; if (g_bcrt[k].bh < oh) oh = g_bcrt[k].bh;
                vka_img_written(im);
                continue;
            }
        }
        if (!(im->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || f.kind >= 3) { static _Atomic int nr; static uint64_t ncount;
            if (atomic_fetch_add(&nr, 1) < 12) VRED("colour target %d (format %#x, view format %#x set %d, %ux%u levels %u, VkFormat %d usage %#x, serial %llu) cannot be a Vulkan colour attachment: not drawn into",
                                                     i, t->d.format, rt->has_cview[i] ? rt->cview[i].format : 0, rt->has_cview[i] && rt->cview[i].set_format, im->w, im->h, im->levels, im->f.vk, im->usage, (unsigned long long)im->serial);
            if (++ncount % 600 == 0) VRED("colour targets not drawn into: %llu so far", (unsigned long long)ncount); continue; }
        if (im->depth > 1 && !(im->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) continue;   // its slices are drawn into through 2D-array views (img_create)
        uint32_t lv, l0, nl; rt_range(im, &rt->cview[i], rt->has_cview[i], &lv, &l0, &nl);
        if (lv >= im->levels || !nl) continue;
        if (samples && im->samples != samples) { VRED_ONCE("colour targets of different sample counts"); continue; }
        vka_tex_own(im, g_cb);   // (a zero-shared image gets memory of its own before it is drawn into: no pass is open here)
        VkImageView v = vka_view(im, f.view, nl > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, lv, 1, l0, nl, NULL);
        if (!v) continue;
        ca[i].imageView = v; R.cf[i] = f.view; R.ckind[i] = f.kind; samples = im->samples; fb_im[nfb] = im; fb_lv[nfb++] = lv;
        R.cim[R.nci] = im; R.clv[R.nci] = lv; R.cl0[R.nci++] = l0; R.scaled |= im->scaled;
        uint32_t lw = im->w >> lv ? im->w >> lv : 1, lh = im->h >> lv ? im->h >> lv : 1;
        R.cnl[i] = nl; R.cext[i] = (VkExtent2D){lw, lh};
        if (lw < w) w = lw; if (lh < h) h = lh; if (nl < layers) layers = nl;
        own_min(im, lv, lw, lh, &ow, &oh);
        vka_img_written(im);
    }
    R.ncolor = (uint32_t)n;
    VkRenderingAttachmentInfo *dp = NULL, *sp = NULL;
    if (has_rt && rt->depth) {
        vka_img *im = vka_tex_get(rt->depth, g_cb);
        if (im && im->f.kind >= 3 && (im->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) && (!samples || im->samples == samples)) {
            uint32_t lv, l0, nl; rt_range(im, &rt->dview, rt->has_dview, &lv, &l0, &nl);
            if (lv < im->levels && nl) vka_tex_own(im, g_cb);
            VkImageView v = lv < im->levels && nl ? vka_view(im, im->f.vk, nl > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, im->aspect, lv, 1, l0, nl, NULL) : VK_NULL_HANDLE;
            if (v) {
                da = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v, .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
                if (im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { dp = &da; R.df = im->f.vk; }
                if (im->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) { sp = &da; R.sf = im->f.vk; }
                samples = im->samples; fb_im[nfb] = im; fb_lv[nfb++] = lv; R.scaled |= im->scaled;
                uint32_t lw = im->w >> lv ? im->w >> lv : 1, lh = im->h >> lv ? im->h >> lv : 1;
                R.dnl = nl; R.dext = (VkExtent2D){lw, lh};
                if (lw < w) w = lw; if (lh < h) h = lh; if (nl < layers) layers = nl;
                own_min(im, lv, lw, lh, &ow, &oh);
                vka_img_written(im);
            }
        } else if (im) VRED_ONCE("depth target (format %#x) cannot be a Vulkan depth attachment: drawn without depth", rt->depth->d.format);
    }
    R.again = nfb > 0;
    for (int i = 0; i < nfb; i++) { const int again = drawn_again(fb_im[i]); R.again &= again; }
    if (w == UINT32_MAX) {   // no attachment at all (rasterizer discard, or targets Vulkan cannot take): the viewport's size, for side effects
        if (!d->has_vp) { VRED_ONCE("a draw with no render target and no viewport: skipped"); return 0; }
        w = (uint32_t)fabsf(d->vp[0][2]); h = (uint32_t)fabsf(d->vp[0][3]); if (!w) w = 1; if (!h) h = 1; layers = 1;
    }
    R.area = (VkRect2D){{0, 0}, {w, h}}; R.layers = layers; R.samples = samples ? samples : 1;
    R.own = ow == UINT32_MAX ? R.area.extent : (VkExtent2D){ow, oh};
    // The pending clears of these targets (PC): loadOp CLEAR when one covers its attachment and the attachment is the render area, else vkCmdClearAttachments once the pass
    // begins when the rectangle is inside the render area and the attachment has the pass's layers, else outside before it begins
    int nlate = 0; vka_pclear *late[NPC];
    sa = da;
    for (int k = 0; k < PC.n; k++) {
        vka_pclear *c = &PC.c[k]; int ci = c->ci;
        int att = ci >= 0 ? ci < n && ca[ci].imageView && R.cnl[ci] == layers && R.cnl[ci] == c->nl
                          : R.dnl == layers && R.dnl == c->nl && (!(c->asp & VK_IMAGE_ASPECT_DEPTH_BIT) || dp) && (!(c->asp & VK_IMAGE_ASPECT_STENCIL_BIT) || sp);
        VkExtent2D e = ci >= 0 ? (ci < 8 ? R.cext[ci] : (VkExtent2D){0, 0}) : R.dext;
        int whole = att && !c->rc.offset.x && !c->rc.offset.y && c->rc.extent.width == w && c->rc.extent.height == h && e.width == w && e.height == h;
        int inarea = att && (uint32_t)c->rc.offset.x + c->rc.extent.width <= w && (uint32_t)c->rc.offset.y + c->rc.extent.height <= h;
        if (whole && !nlate) {   // (after a late one it would come first: late too)
            if (ci >= 0) { ca[ci].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca[ci].clearValue = c->in; }
            else { if (c->asp & VK_IMAGE_ASPECT_DEPTH_BIT) { da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.clearValue.depthStencil.depth = c->in.depthStencil.depth; }
                   if (c->asp & VK_IMAGE_ASPECT_STENCIL_BIT) { sa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; sa.clearValue.depthStencil.stencil = c->in.depthStencil.stencil; } }
        } else if (inarea) late[nlate++] = c;
        else vka_clear_outside(c);
    }
    if (sp) sp = &sa;
    #undef INAREA
    #undef WHOLE
    VkRenderingFragmentShadingRateAttachmentInfoKHR fsr = {VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR, .imageView = vrs ? vrs_fill(&vk_, R.area.extent) : VK_NULL_HANDLE,
                                                           .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .shadingRateAttachmentTexelSize = {VRS_TEXEL, VRS_TEXEL}};
    // (-1: it wanted an attachment and none could be made: the draws after it with the same image do not open the pass again)
    if (vrs) { R.vrs = fsr.imageView ? 1 : -1; R.vrs_serial = vk_.serial; R.vrs_view = vk_.view; R.vrs_has_view = vk_.has_view; memcpy(R.vrs_rate, vk_.rate, 16); }
    vka_tex_feedback(fb_im, fb_lv, nfb);
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .pNext = fsr.imageView ? &fsr : NULL, .renderArea = R.area, .layerCount = layers, .colorAttachmentCount = (uint32_t)n, .pColorAttachments = ca,
                          .pDepthAttachment = dp, .pStencilAttachment = sp};
    if (vka_gp_active) {   // DM_VK_GPUPROF: what the pass is, for the step it ends
        char b[96]; size_t bn = (size_t)snprintf(b, sizeof b, "%ux%ux%u", w, h, layers);
        for (int i = 0; i < n && bn < sizeof b; i++) if (ca[i].imageView) bn += (size_t)snprintf(b + bn, sizeof b - bn, " c%d:%d%s", i, R.cf[i], ca[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? "C" : "");
        if (dp || sp) snprintf(b + bn, bn < sizeof b ? sizeof b - bn : 0, " d:%d%s", R.df ? R.df : R.sf, (dp ? dp : sp)->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? "C" : "");
        vka_gp_pass(b);
    }
    rts_note(w, h, d, dp || sp);
    {   // Fritchii: a pass whose SET_RT had emulated mesh draws in the frame before gets a part of its own with a pre command buffer (vka_exec.c vka_pre_split)
        extern uint32_t g_setrt_n; int vka_pre_split(void); uint64_t vka_frame(void);
        static uint64_t fr = ~0ull; if (vka_frame() != fr) { fr = vka_frame(); memcpy(g_pre_want, g_pre_seen, sizeof g_pre_want); memset(g_pre_seen, 0, sizeof g_pre_seen); g_pre_done = ~0u; }
        static int on = -1; if (on < 0) on = (int)VKA_ENV("DM_VK_MESH_PRE", 0);
        const uint32_t o = g_setrt_n & 1023; R.setrt = o;
        if (on && g_pre_done != o && (g_pre_want[o >> 6] >> (o & 63) & 1)) { g_pre_done = o; vka_pre_split(); }
    }
    vk.vkCmdBeginRendering(g_cb, &ri);
    { void vka_oq_pass_begin(VkCommandBuffer cb); vka_oq_pass_begin(g_cb); }   // Lichii: samples-passed counters (vka_exec.c)
    R.on = 1; vka_n_rp++; if (g_dyn_filter == 2) vka_dyn_invalidate();   // DM_VK_DYN_FILTER=2: the dynamic state set again at each pass (bisect)
    for (int k = 0; k < nlate; k++) { VkClearAttachment c = {late[k]->asp, late[k]->ci >= 0 ? (uint32_t)late[k]->ci : 0, late[k]->in}; VkClearRect cr = {late[k]->rc, 0, layers}; vk.vkCmdClearAttachments(g_cb, 1, &c, 1, &cr); }
    PC.n = 0;
    return 1;
}
int vka_pass_keep(void) { return g_pass_keep; }
// A game barrier (nvnCommandBufferBarrier) with a draw pass open. NVN orders the work before it against the work after it (ORDER_PRIMITIVES / ORDER_FRAGMENTS, the cache
// invalidations); the replay used to end the pass there and record a full barrier, so every barrier between draws into the same targets cost a pass end, a store and a
// load of every attachment on a tiler (the next draw reopens it with LOAD). Inside a pass Vulkan already keeps the attachment writes of draws in rasterization order, so
// what a barrier between two draws of one pass can order is: memory the earlier draws wrote with shader stores (the pass then ends at once, as nvn_mtl.m's OP_BARRIER
// does for a pass with shader writes), and an attachment of the pass that a later draw samples (a feedback read: the pass ends before that draw, bar_hazard). Everything
// else the draws read was written before the pass began, behind the barrier that ended what wrote it. DM_VK_BAR_KEEP=1 takes such barriers inside the pass (live;
// default 0 until phone-checked); the counts are in the replay statistics line either way
int vka_rp_game_barrier(uint32_t bits) {
    (void)bits;
    if (!R.on) return 0;
    g_bar_in++;
    if (R.swrite || !VKA_ENV("DM_VK_BAR_KEEP", 0)) return 0;
    R.bar_pending = 1; g_bar_kept++;
    return 1;
}
void vka_rp_bar_stats(char *b, size_t n, double frames) {
    static uint64_t i0, k0, l0;
    if (frames <= 0 || g_bar_in == i0) { if (n) b[0] = 0; i0 = g_bar_in; k0 = g_bar_kept; l0 = g_bar_late; return; }
    snprintf(b, n, "game barriers inside a draw pass per frame: %.1f, %.1f taken inside it (DM_VK_BAR_KEEP), %.1f of those ended it later at a draw sampling its targets or writing memory",
             (g_bar_in - i0) / frames, (g_bar_kept - k0) / frames, (g_bar_late - l0) / frames);
    i0 = g_bar_in; k0 = g_bar_kept; l0 = g_bar_late;
}
int vka_rp_is_open(void) { return R.on; }
int vka_rp_open_for(const p_set_rt *rt, int has_rt) { return R.on && rt_same(rt, has_rt, &R.rt, R.has_rt); }
// The draw about to come may start a new part of the submission: no pass is open over its targets and no clear waits for them (an open pass over other targets ends at it anyway)
int vka_rp_split_ok(const p_set_rt *rt, int has_rt) { return R.on ? !rt_same(rt, has_rt, &R.rt, R.has_rt) : PC.n ? has_rt && !rt_same(rt, has_rt, &PC.rt, 1) : 1; }
void vka_draw_cb_switch(VkCommandBuffer cb) { g_cb = cb; vka_dyn_invalidate(); }   // the submission goes on in another command buffer (vka_exec.c chunk): its bound state is gone
// Before a clear of these targets records anything (vka_tex_get may record a first upload): a pass over other targets ends, pending clears of other targets are done
void vka_rp_prep_clear(const p_set_rt *rt, int has_rt) {
    if (R.on) { if (!rt_same(rt, has_rt, &R.rt, R.has_rt)) vka_rp_end(); }
    else if (PC.n && !rt_same(rt, has_rt, &PC.rt, 1)) pc_flush();
}
// A clear of one of the bound targets (colour index ci, or depth / stencil when ci < 0) inside the render pass: an open pass over the same targets takes it as
// vkCmdClearAttachments (rasterization-ordered with its draws), a closed one opens over them with it (loadOp CLEAR). 0 = not here: the caller ends the pass and clears outside.
// rc is in the attachment's level (the scissored part), nl the layers the clear names: they must be the attachment's own, within the render area
int vka_rp_clear(const p_set_rt *rt, int has_rt, const vka_dstate *d, const vka_pclear *c) {
    (void)d;
    if (!g_cb || !has_rt || !draws_on()) return 0;
    if (R.on && !rt_same(rt, has_rt, &R.rt, R.has_rt)) return 0;
    if (!R.on) {
        if (PC.n && (!rt_same(rt, has_rt, &PC.rt, 1) || PC.n == NPC)) pc_flush();
        if (!PC.n) PC.rt = *rt;
        PC.c[PC.n++] = *c;
        return 1;
    }
    int ci = c->ci; uint32_t nl = c->nl; VkRect2D rc = c->rc;
    if (ci >= 0 ? ci >= 8 || !R.cf[ci] || R.cnl[ci] != nl || nl != R.layers : ((c->asp & VK_IMAGE_ASPECT_DEPTH_BIT) && !R.df) || ((c->asp & VK_IMAGE_ASPECT_STENCIL_BIT) && !R.sf) || R.dnl != nl || nl != R.layers) return 0;
    if ((uint32_t)rc.offset.x + rc.extent.width > R.area.extent.width || (uint32_t)rc.offset.y + rc.extent.height > R.area.extent.height) return 0;
    VkClearAttachment ca = {c->asp, ci >= 0 ? (uint32_t)ci : 0, c->in}; VkClearRect cr = {rc, 0, nl};
    vk.vkCmdClearAttachments(g_cb, 1, &ca, 1, &cr);
    return 1;
}

// ---------- Formats and fixed-function mappings ----------
// NVN vertex formats 0x01-0x30: four rows (1-4 components) of {UNORM8, SNORM8, UINT8, SINT8, FLOAT16, UNORM16, SNORM16, UINT16, SINT16, FLOAT32, UINT32, SINT32}
#define VF(n, s) VK_FORMAT_##n##_##s
static const VkFormat VROW[4][12] = {
    {VF(R8, UNORM), VF(R8, SNORM), VF(R8, UINT), VF(R8, SINT), VF(R16, SFLOAT), VF(R16, UNORM), VF(R16, SNORM), VF(R16, UINT), VF(R16, SINT), VF(R32, SFLOAT), VF(R32, UINT), VF(R32, SINT)},
    {VF(R8G8, UNORM), VF(R8G8, SNORM), VF(R8G8, UINT), VF(R8G8, SINT), VF(R16G16, SFLOAT), VF(R16G16, UNORM), VF(R16G16, SNORM), VF(R16G16, UINT), VF(R16G16, SINT), VF(R32G32, SFLOAT), VF(R32G32, UINT), VF(R32G32, SINT)},
    {VF(R8G8B8, UNORM), VF(R8G8B8, SNORM), VF(R8G8B8, UINT), VF(R8G8B8, SINT), VF(R16G16B16, SFLOAT), VF(R16G16B16, UNORM), VF(R16G16B16, SNORM), VF(R16G16B16, UINT), VF(R16G16B16, SINT), VF(R32G32B32, SFLOAT), VF(R32G32B32, UINT), VF(R32G32B32, SINT)},
    {VF(R8G8B8A8, UNORM), VF(R8G8B8A8, SNORM), VF(R8G8B8A8, UINT), VF(R8G8B8A8, SINT), VF(R16G16B16A16, SFLOAT), VF(R16G16B16A16, UNORM), VF(R16G16B16A16, SNORM), VF(R16G16B16A16, UINT), VF(R16G16B16A16, SINT), VF(R32G32B32A32, SFLOAT), VF(R32G32B32A32, UINT), VF(R32G32B32A32, SINT)},
};
static const VkFormat VSCALED[4][2][2] = {   // [components][8 / 16 bits][unsigned / signed]
    {{VF(R8, USCALED), VF(R8, SSCALED)}, {VF(R16, USCALED), VF(R16, SSCALED)}}, {{VF(R8G8, USCALED), VF(R8G8, SSCALED)}, {VF(R16G16, USCALED), VF(R16G16, SSCALED)}},
    {{VF(R8G8B8, USCALED), VF(R8G8B8, SSCALED)}, {VF(R16G16B16, USCALED), VF(R16G16B16, SSCALED)}}, {{VF(R8G8B8A8, USCALED), VF(R8G8B8A8, SSCALED)}, {VF(R16G16B16A16, USCALED), VF(R16G16B16A16, SSCALED)}}};
#undef VF
// The Vulkan format for NVN vertex format f whose shader input has class want (DMSH_VK_TYPE_*): the format's own when its class matches, else the same layout in that class
// (Vulkan wants the classes equal). An integer format into a float input keeps its own format and *retype says the input's class must change (DMSH_VK_TYPE_SINT / UINT):
// Maxwell's fetch puts the integer's bits in the register the shader bit-casts, so the vertex module is rewritten to declare that input as an integer vector (vka_spvpatch.c).
// VK_FORMAT_UNDEFINED = not mapped
static VkFormat vin_format(int f, int want, int *changed, int *retype) {
    *changed = 0; *retype = 0;
    int n = 0, col = -1;   // row (components) and column of VROW, or a scaled / packed format
    if (f >= 0x01 && f <= 0x30) { n = (f - 1) / 12 + 1; col = (f - 1) % 12; }
    else if (f >= 0x51 && f <= 0x68) {   // integers read as floats (nvn_mtl.m vconv_i2f): USCALED / SSCALED, none for 32 bits
        int i = (f - 0x51) >> 1, sg = (f - 0x51) & 1, bits = 8 << (i % 3); n = i / 3 + 1;
        if (bits < 32 && want == DMSH_VK_TYPE_FLOAT) return VSCALED[n - 1][bits == 16][sg];
        col = bits == 8 ? 2 + sg : bits == 16 ? 7 + sg : 10 + sg; *changed = want == DMSH_VK_TYPE_FLOAT;
        if (want == DMSH_VK_TYPE_FLOAT) return VROW[n - 1][col];
    } else {
        VkFormat p = f == 0x3D ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : f == 0x69 ? VK_FORMAT_A2B10G10R10_SNORM_PACK32 : f == 0x3F ? VK_FORMAT_B10G11R11_UFLOAT_PACK32 : f == 0x9A ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_UNDEFINED;
        if (p && want != DMSH_VK_TYPE_FLOAT) { *changed = 1; return want == DMSH_VK_TYPE_UINT ? VK_FORMAT_R32_UINT : VK_FORMAT_R32_SINT; }
        return p;
    }
    static const int8_t cls[12] = {0, 0, 2, 1, 0, 0, 0, 2, 1, 0, 2, 1};   // DMSH_VK_TYPE_* of each column
    if (cls[col] == want) return VROW[n - 1][col];
    if (want == DMSH_VK_TYPE_FLOAT && retype) { *retype = cls[col]; return VROW[n - 1][col]; }
    *changed = 1;
    int bits = col < 4 ? 8 : col < 9 ? 16 : 32, sg = col == 1 || col == 3 || col == 6 || col == 8 || col == 11;
    if (want == DMSH_VK_TYPE_FLOAT) return bits < 32 ? VSCALED[n - 1][bits == 16][sg] : VROW[n - 1][9];
    int base = bits == 8 ? 2 : bits == 16 ? 7 : 10;   // the integer of the same width, in the class asked
    return VROW[n - 1][base + (want == DMSH_VK_TYPE_SINT)];
}
static VkBlendFactor bfac(int f) {
    switch (f) {
    case NVN_BLEND_FUNC_ZERO: return VK_BLEND_FACTOR_ZERO; case NVN_BLEND_FUNC_ONE: return VK_BLEND_FACTOR_ONE;
    case NVN_BLEND_FUNC_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case NVN_BLEND_FUNC_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case NVN_BLEND_FUNC_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case NVN_BLEND_FUNC_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case NVN_BLEND_FUNC_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case NVN_BLEND_FUNC_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}
static VkBlendOp bop(int e) { return e >= 1 && e <= 5 ? (VkBlendOp)(e - 1) : VK_BLEND_OP_ADD; }
static int is_src1(int f) { return f >= NVN_BLEND_FUNC_SRC1_COLOR && f <= NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA; }
static int zero_src1(int f) { return f == NVN_BLEND_FUNC_SRC1_COLOR || f == NVN_BLEND_FUNC_SRC1_ALPHA ? NVN_BLEND_FUNC_ZERO : f == NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR || f == NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA ? NVN_BLEND_FUNC_ONE : f; }
// An advanced blend mode as fixed-function factors where one exists (nvn_mtl.m adv_blend: the Porter-Duff modes with uncorrelated overlap)
static blend_st adv_blend(blend_st b) {
    if (!b.adv) return b;
    enum { Z = NVN_BLEND_FUNC_ZERO, O = NVN_BLEND_FUNC_ONE, SA = NVN_BLEND_FUNC_SRC_ALPHA, ISA = NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, DA = NVN_BLEND_FUNC_DST_ALPHA, IDA = NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA };
    static const uint8_t pd[][4] = {
        [NVN_BLEND_ADVANCED_MODE_ZERO] = {Z, Z, Z, Z},        [NVN_BLEND_ADVANCED_MODE_SRC] = {O, Z, O, Z},           [NVN_BLEND_ADVANCED_MODE_DST] = {Z, O, Z, O},
        [NVN_BLEND_ADVANCED_MODE_SRC_OVER] = {O, ISA, O, ISA}, [NVN_BLEND_ADVANCED_MODE_DST_OVER] = {IDA, O, IDA, O},
        [NVN_BLEND_ADVANCED_MODE_SRC_IN] = {DA, Z, DA, Z},     [NVN_BLEND_ADVANCED_MODE_DST_IN] = {Z, SA, Z, SA},
        [NVN_BLEND_ADVANCED_MODE_SRC_OUT] = {IDA, Z, IDA, Z},  [NVN_BLEND_ADVANCED_MODE_DST_OUT] = {Z, ISA, Z, ISA},
        [NVN_BLEND_ADVANCED_MODE_SRC_ATOP] = {DA, ISA, Z, O},  [NVN_BLEND_ADVANCED_MODE_DST_ATOP] = {IDA, SA, O, Z},
        [NVN_BLEND_ADVANCED_MODE_XOR] = {IDA, ISA, IDA, ISA},  [NVN_BLEND_ADVANCED_MODE_PLUS] = {O, O, O, O},
        [NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED] = {O, O, O, O}, [NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED_ALPHA] = {O, O, O, O},
    };
    const uint8_t *f = b.adv < sizeof pd / sizeof *pd && pd[b.adv][0] ? pd[b.adv] : NULL;
    if (!f || b.adv_ovl || (b.adv_npm && f[0] != O && f[0] != Z)) { VRED_ONCE("advanced blend mode %#x has no fixed-function equivalent: the state's blend functions are used", (unsigned)b.adv); return b; }
    b.src_rgb = b.adv_npm && f[0] == O ? SA : f[0]; b.dst_rgb = f[1]; b.src_a = f[2]; b.dst_a = f[3];
    b.eq_rgb = b.eq_a = NVN_BLEND_EQUATION_ADD;
    return b;
}
// NVN primitive → Vulkan topology; *expand: 1 quads, 2 line loop (indices made on the CPU), 0 none; -1 = not drawable
static VkPrimitiveTopology topology(int prim, int *expand) {
    *expand = 0;
    switch (prim) {
    case NVN_DRAW_PRIMITIVE_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case NVN_DRAW_PRIMITIVE_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case NVN_DRAW_PRIMITIVE_LINE_LOOP: *expand = 2; return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLES: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: case NVN_DRAW_PRIMITIVE_QUAD_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;   // a quad strip rasterizes as the triangle strip of its vertices
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: case NVN_DRAW_PRIMITIVE_POLYGON: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case NVN_DRAW_PRIMITIVE_QUADS: *expand = 1; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_PATCHES: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    }
    *expand = -1; return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
}
static VkPrimitiveTopology topo_class(VkPrimitiveTopology t) {   // the pipeline's topology: one of each class, the draw sets its own dynamically
    switch (t) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return t;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY: case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST: return t;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

// ---------- Guest memory as descriptors ----------
static int cpu_read(uint64_t addr, void *out, size_t n) {   // guest bytes as the CPU sees them now
    // Lichii: addr may be the game's own (a GPU address the game wrote into its data): read at its host pointer
    if (vka_addr(addr, n, NULL, NULL, NULL)) { memcpy(out, (const void *)(uintptr_t)nvn_direct_host_of(addr), n); return 1; }
    return !plat_mem_read(addr, out, n);
}
uint64_t vka_n_mesh_draws;
VkCommandBuffer vka_draw_cb(void) { return g_cb; }
static int rd_cb(void *ctx, uint64_t addr, void *out, size_t n) { (void)ctx; return cpu_read(addr, out, n); }
#include <stdarg.h>
void vka_bind_log(const char *fmt, ...) {   // Lichii: vka_bind.c's traces, in the replay's log
    char m[512]; va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap); VLOG("%s", m);
}
// A uniform buffer: bound in place when Vulkan's offset alignment allows, else a copy of what the CPU sees now (a GPU write earlier in this submission is missed: logged)
static int ubo_desc(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *bi) {
    if (!size || size > V.ub_range) size = size ? V.ub_range : 65536 < V.ub_range ? 65536 : V.ub_range;
    VkBuffer b; VkDeviceSize o;
    uint64_t run = vka_addr_run(addr, size, &b, &o);
    static int snap = -1; if (snap < 0) snap = (int)VKA_ENV("DM_VK_UBO_COPY", 0);   // DM_VK_UBO_COPY=1: every uniform buffer a copy of the CPU's bytes at replay (none read in place)
    if (!snap && run && !(o % V.ub_align) && !(addr % V.ub_align)) { *bi = (VkDescriptorBufferInfo){b, o, run}; return 1; }
    vka_span s; if (!vka_scratch(size, V.ub_align, &s)) return 0;
    if (!cpu_read(addr, s.ptr, size)) { memset(s.ptr, 0, size); VRED_ONCE("uniform buffer at %#llx is not readable: it reads zeros", (unsigned long long)addr); }
    else if (run) VRED_ONCE("uniform buffer at %#llx is not %llu-byte aligned: bound as a copy of the CPU's bytes", (unsigned long long)addr, (unsigned long long)V.ub_align);
    else VRED_ONCE("uniform buffer at %#llx is not in placed guest RAM: bound as a copy of the CPU's bytes", (unsigned long long)addr);
    *bi = (VkDescriptorBufferInfo){s.buf, s.off, size}; return 1;
}
static int ssbo_desc(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *bi) {
    VkBuffer b; VkDeviceSize o;
    uint64_t run = vka_addr_run(addr, size ? size : V.sb_range, &b, &o);
    if (!run) { VRED_ONCE("storage buffer at %#llx is not in placed guest RAM: it is not bound", (unsigned long long)addr); return 0; }
    if (run > V.sb_range) run = V.sb_range;
    *bi = (VkDescriptorBufferInfo){b, o, run}; return 1;
}
// Driver segments (c0, c2, the push bank) and raw global memory's table, kept from the last draw of this submission that filled them: a draw whose segment holds the same
// bytes binds the same scratch span (DM_VK_DRV_MEMO, live, default 1). Each draw used to fill a fresh 2.5 KB span per segment it reads (memset and fill into scratch, under
// the scratch lock: ~10 KB of writes a draw, ~80 MB a frame in DKB's level), so no two draws' pushes were ever the same and the push dedupe (push_same) never matched them.
// Graphics stages only: an indirect dispatch's counts are copied into its c0 by the GPU (vka_dispatch). A span lives until the submission's fence (vka_scratch_retire at
// its commit), so the memo is forgotten at each submission's start (vka_draw_sub_begin)
typedef struct { VkBuffer buf; VkDeviceSize off; } c0_at;
static struct { int valid; vka_span s; uint8_t b[DMSH_DRV_SIZE]; } g_dm[VKA_NSTAGE][3];
static struct { int valid; uint64_t gen; vka_span s; } g_gm;
static struct { int valid; uint32_t ref; vka_span s; } g_sm_ref;   // the support buffer with the alpha test's reference (stage_writes)
static int g_drv_memo = 1; static uint64_t g_dm_hit, g_dm_miss;
static void drv_memo_reset(void) { for (int st = 0; st < VKA_NSTAGE; st++) for (int b = 0; b < 3; b++) g_dm[st][b].valid = 0; g_gm.valid = 0; g_sm_ref.valid = 0; g_drv_memo = VKA_ENV("DM_VK_DRV_MEMO", 1); }
static int drv_seg(const vka_dstate *d, int st, int bank, int pool, VkDescriptorBufferInfo *bi, c0_at *c0, const vka_shader *sh) {
    size_t sz = vka_drv_size(bank); vka_span s;
    if (bank == VKA_BANK_NVN2_CS_C0) {   // Lichii: a recompiled compute shader's c[0] in its own layout (nvn_recompile.c)
        if (!vka_scratch(DMSH_DRV_SIZE, V.ub_align, &s)) return 0;
        vka_rc_cs_c0(sh, &d->b, s.ptr);
        *bi = (VkDescriptorBufferInfo){s.buf, s.off, DMSH_DRV_SIZE};
        if (c0) { c0->buf = s.buf; c0->off = s.off; }
        return 1;
    }
    // One memo slot per segment kind: c0, c2 and the push bank. Any other driver bank (an all-zero c17-style read of DMSH_DRV_SIZE bytes) takes the fill below: sharing
    // the push bank's slot, an all-zero push block matched it and the 0x100-byte span was bound with a 0xA00-byte range (other draws' scratch, or past the buffer)
    int bi_ = bank == 0 ? 0 : bank == 2 ? 1 : bank == DMSH_PUSH_BANK ? 2 : -1;
    if (g_drv_memo && bi_ >= 0 && st != 5 && sz <= DMSH_DRV_SIZE) {
        static uint8_t tmp[DMSH_DRV_SIZE];
        vka_drv_fill(&d->b, st, bank, pool, tmp);
        typeof(g_dm[0][0]) *m = &g_dm[st][bi_];
        if (m->valid && !memcmp(m->b, tmp, sz)) { g_dm_hit++; *bi = (VkDescriptorBufferInfo){m->s.buf, m->s.off, sz}; return 1; }
        if (!vka_scratch(sz, V.ub_align, &s)) return 0;
        memcpy(s.ptr, tmp, sz); memcpy(m->b, tmp, sz); m->s = s; m->valid = 1; g_dm_miss++;
        *bi = (VkDescriptorBufferInfo){s.buf, s.off, sz};
        return 1;
    }
    if (!vka_scratch(sz, V.ub_align, &s)) return 0;
    vka_drv_fill(&d->b, st, bank, pool, s.ptr);
    *bi = (VkDescriptorBufferInfo){s.buf, s.off, sz};
    if (bank == 0 && c0) { c0->buf = s.buf; c0->off = s.off; }
    return 1;
}
// Raw global memory's table (dmsh_abi.h DMSH_GMEM_MAP_*): entry 0 empty, then the placed chunks by address with delta = device address - guest address, unused entries at ~0
// (kept while no segment comes or goes: vka_mem_gen, and DM_VK_DRV_MEMO)
uint64_t vka_mem_gen(void);
static int gmem_map(VkDescriptorBufferInfo *bi) {
    size_t n = (size_t)DMSH_GMEM_MAP_ENTRIES * DMSH_GMEM_MAP_ENTRY;
    uint64_t gen = vka_mem_gen();
    if (g_drv_memo && g_gm.valid && g_gm.gen == gen) { *bi = (VkDescriptorBufferInfo){g_gm.s.buf, g_gm.s.off, n}; return 1; }
    vka_span s; if (!vka_scratch(n, V.sb_align, &s)) return 0;
    memset(s.ptr, 0xff, n); memset(s.ptr, 0, DMSH_GMEM_MAP_ENTRY);
    static uint64_t lo[DMSH_GMEM_MAP_ENTRIES], hi[DMSH_GMEM_MAP_ENTRIES], bda[DMSH_GMEM_MAP_ENTRIES];
    int k = vka_mem_chunks(lo, hi, bda, DMSH_GMEM_MAP_ENTRIES - 1);
    for (int i = 0; i < k && i + 1 < DMSH_GMEM_MAP_ENTRIES; i++) { uint64_t e[4] = {lo[i], hi[i], bda[i] - lo[i], 0}; memcpy(s.ptr + (size_t)(i + 1) * DMSH_GMEM_MAP_ENTRY, e, sizeof e); }
    g_gm.valid = 1; g_gm.gen = gen; g_gm.s = s;   // (a segment that came between the read of gen and the table: the next draw sees a new gen and builds it again)
    *bi = (VkDescriptorBufferInfo){s.buf, s.off, n}; return 1;
}
// A compute program's shared memory past the device's limit (DMSH_VK_KIND_SHARED_SPILL; Ryujinx StructuredIr/SharedSpillInfo.cs): one buffer for the process
// (DM_VK_SHARED_SPILL_MB, default 16 MiB, at least one slice), reused by every such dispatch: each dispatch ends with a barrier (vka_dispatch), so the next one finds it free.
// A dispatch binds slices for as many workgroups as it has, or as the buffer holds; workgroup w takes slice w mod the slices bound. A dispatch with more workgroups than
// that runs in parts (vkCmdDispatchBase, a barrier between them), each part's linear indices a contiguous run no longer than the slices, so no two workgroups that may run
// at once share a slice. An indirect dispatch's group count is known only on the GPU: it gets the whole buffer (open: correct while no two workgroups the buffer's slice
// count apart run at once, which in-order workgroup launch makes very likely but Vulkan does not promise)
static vka_span g_spill; static VkDeviceSize g_spill_cap, g_spill_range;
static uint32_t spill_bytes(const vka_shader *cs) {
    for (unsigned i = 0; i < cs->e->bind_count; i++) if (cs->b[i].kind == DMSH_VK_KIND_SHARED_SPILL) return cs->b[i].bytes;
    return 0;
}
static int spill_buf(uint32_t slice) {
    static int failed;
    if (!g_spill.buf && !failed) {   // made once, at the first such dispatch (one try: a failure is logged once and their dispatches are skipped)
        VkDeviceSize cap = (VkDeviceSize)VKA_ENV("DM_VK_SHARED_SPILL_MB", 16) << 20, lim = V.props.limits.maxStorageBufferRange;
        if (lim && cap > lim) cap = lim;
        if (cap < (VkDeviceSize)DMSH_VK_SHARED_MAX * 2) cap = (VkDeviceSize)DMSH_VK_SHARED_MAX * 2;   // at least one slice of any program the build moves (they declare under twice the limit)
        if ((lim && cap > lim) || !vka_static(cap, V.sb_align, &g_spill)) { failed = 1; g_spill.buf = VK_NULL_HANDLE; VRED("no shared memory spill buffer of %llu bytes", (unsigned long long)cap); return 0; }
        g_spill_cap = cap;
        VLOG("shared memory spill buffer: %llu KiB (%llu slices of the first program's %u bytes)", (unsigned long long)(cap >> 10), (unsigned long long)(cap / slice), slice);
    }
    return g_spill.buf && g_spill_cap >= slice;
}
static void spill_dispatch(uint32_t x, uint32_t y, uint32_t z, uint64_t ns) {
    int first = 1;
    #define PART(bx, by, bz, cx, cy, cz) do { if (!first) vka_barrier(g_cb); first = 0; vk.vkCmdDispatchBase(g_cb, bx, by, bz, cx, cy, cz); } while (0)
    #define MIN(a, b) ((a) < (b) ? (a) : (b))
    if ((uint64_t)x * y <= ns) { uint32_t c = (uint32_t)(ns / ((uint64_t)x * y)); for (uint32_t z0 = 0; z0 < z; z0 += c) PART(0, 0, z0, x, y, MIN(c, z - z0)); }
    else if (x <= ns) { uint32_t c = (uint32_t)(ns / x); for (uint32_t zz = 0; zz < z; zz++) for (uint32_t y0 = 0; y0 < y; y0 += c) PART(0, y0, zz, x, MIN(c, y - y0), 1); }
    else for (uint32_t zz = 0; zz < z; zz++) for (uint32_t yy = 0; yy < y; yy++) for (uint32_t x0 = 0; x0 < x; x0 += (uint32_t)ns) PART(x0, yy, zz, MIN((uint32_t)ns, x - x0), 1, 1);
    #undef PART
    #undef MIN
}
static int gmem_log(VkDescriptorBufferInfo *bi) {
    if (vka_tex_gml_take(bi)) return 1;   // read when the submission is done: the textures over the pages its stores wrote are uploaded again
    size_t n = ((size_t)1 + (1u << DMSH_GMEM_LOG_LOG2)) * 4;
    vka_span s; if (!vka_scratch(n, V.sb_align, &s)) return 0;
    memset(s.ptr, 0, n);
    *bi = (VkDescriptorBufferInfo){s.buf, s.off, n}; return 1;
}

// Placeholders for unbound images when the device has no null descriptors: 1x1 images of the shapes a shader may declare, zero (float, sint and uint views)
VkImageView placeholder(uint8_t dim, int storage) {
    static vka_img *im2, *im3, *im1; static VkImageView cache[3][7][3];
    int cls = dim & DMSH_VK_DIM_SINT ? 1 : dim & DMSH_VK_DIM_UINT ? 2 : 0, d = dim & DMSH_VK_DIM_MASK, a = (dim & DMSH_VK_DIM_ARRAY) != 0;
    if ((dim & DMSH_VK_DIM_MS) || d == 0) return VK_NULL_HANDLE;   // 1D shapes: the replay makes no 1D images (vka_tex.c img_create) and the translator declares none
    VkImageViewType vt = d == 0 ? (a ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D) : d == 2 ? VK_IMAGE_VIEW_TYPE_3D : d == 3 ? (a ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE) : a ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    if (cache[storage][vt][cls]) return cache[storage][vt][cls];
    vka_img **pim = d == 2 ? &im3 : d == 0 ? &im1 : &im2;
    if (!*pim) {   // a texture of our own, made through the same path as the game's (zeroed at first use)
        static tex_t t[3]; tex_t *tt = &t[d == 2 ? 1 : d == 0 ? 2 : 0]; memset(tt, 0, sizeof *tt);
        tt->d.target = d == 2 ? NVN_TEXTURE_TARGET_3D : d == 0 ? NVN_TEXTURE_TARGET_1D_ARRAY : NVN_TEXTURE_TARGET_CUBEMAP_ARRAY; tt->d.format = NVN_FORMAT_RGBA8;
        tt->d.width = 1; tt->d.height = 1; tt->d.depth = d == 2 ? 1 : 6; tt->d.levels = 1; tt->d.flags = NVN_TEXTURE_FLAGS_IMAGE;
        nvn_tex_layout(&tt->d, &tt->L); tt->serial = nvn_new_serial();
        *pim = vka_tex_get(tt, g_cb);
        if (!*pim) return VK_NULL_HANDLE;
    }
    static const VkFormat f[3] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_UINT};
    uint32_t na = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY ? 6 : vt == VK_IMAGE_VIEW_TYPE_2D_ARRAY || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? (*pim)->layers : 1;
    return cache[storage][vt][cls] = vka_view(*pim, f[cls], vt, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, na, NULL);
}

// Where a draw's CPU time goes (DM_VK_PROF=1, logged with the replay statistics): the virtual counter at each phase boundary of vka_draw
static uint64_t g_pt_last, g_pt[9], g_ps[6]; static int g_pt_ph;
#if defined(__x86_64__)   // x86-64: a nanosecond counter stands in for the generic timer
static inline uint64_t cntvct(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
#else
static inline uint64_t cntvct(void) { uint64_t v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v; }
#endif
// ---------- Set 0 from the walk ----------
typedef struct { VkWriteDescriptorSet w[512]; VkDescriptorBufferInfo bi[512]; VkDescriptorImageInfo ii[512]; VkBufferView bv[512]; uint32_t n; } writes_t;
static writes_t g_w;
static vka_res g_res[512];   // the NaN scan's own walk (DM_VK_NANSCAN_FS)
// Fritchii, DM_VK_BIND_TRACE_FS=<key>: every binding of that shader's first 2 draws, logged with what it was given (a draw's inputs, to compare with what the game meant)
static int bind_trace(const vka_shader *sh) {
    static const char *e; static int ei; if (!ei) { e = getenv("DM_VK_BIND_TRACE_FS"); ei = 1; }   // (a list of keys: any stage, compute included)
    if (!e || !sh) return 0;
    char kx[17]; snprintf(kx, sizeof kx, "%016llx", (unsigned long long)sh->key);
    static _Atomic int n; return strstr(e, kx) && atomic_load(&n) < 16 * 64 && (atomic_fetch_add(&n, 1), 1);
}
// One stage's results as descriptor writes. 0 = the draw cannot run (logged)
static int stage_writes(const vka_dstate *d, int st, const vka_shader *sh, const vka_res *r, int n, int pool, c0_at *c0) {
    for (int i = 0; i < n; i++) {
        if (g_w.n == 512) { VRED_ONCE("more than 512 descriptors in one draw"); return 0; }
        uint32_t k = g_w.n;
        VkWriteDescriptorSet *w = &g_w.w[k]; VkDescriptorBufferInfo *bi = &g_w.bi[k]; VkDescriptorImageInfo *ii = &g_w.ii[k];
        *w = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = r[i].binding, .dstArrayElement = r[i].elem, .descriptorCount = 1, .descriptorType = (VkDescriptorType)r[i].desc};
        int ok = 1;
        switch (r[i].desc) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
            int storage = r[i].desc == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            switch (r[i].what) {
            case VKA_R_BUF: ok = storage ? ssbo_desc(r[i].addr, r[i].size, bi) : ubo_desc(r[i].addr, r[i].size, bi);
                if (ok && storage) fog_bind(sh->key, r[i].binding, r[i].addr, bi);
                {   // Fritchii, FRITCHII_SSBO_WATCH=<key>:<binding>: that shader's storage buffer is watched; every shader binding storage over it is logged once (its writers)
                    extern uint64_t vka_watch_addr; static uint64_t wk = ~0ull; static unsigned wb; uint64_t wa = vka_watch_addr; if (wk == ~0ull) { const char *ev = getenv("FRITCHII_SSBO_WATCH"); wk = 0; if (ev) sscanf(ev, "%llx:%u", (unsigned long long *)&wk, &wb); }
                    if (wk && storage) {
                        if (sh->key == wk && r[i].binding == wb) vka_watch_addr = wa = r[i].addr;
                        if (wa && r[i].addr < wa + 0xc0 && wa < r[i].addr + (r[i].size ? r[i].size : bi->range) && (r[i].flags & DMSH_VK_BF_RW || sh->key == wk)) {   // (any overlap with its 192 bytes)
                            static _Atomic int nlog; uint64_t vka_exec_queue_serial(void), vka_exec_cur_fence(void);
                            if (atomic_fetch_add(&nlog, 1) < 600) { char md[300] = ""; void vka_mem_describe(uint64_t, char *, size_t); vka_mem_describe(r[i].addr, md, sizeof md);
                              VLOG("ssbo watch: shader %016llx stage %d binding %u %s (+%#llx), queue %llu fence %llu: VkBuffer %p + %#llx range %#llx; %s", (unsigned long long)sh->key, st, r[i].binding, (r[i].flags & DMSH_VK_BF_RW) ? "WRITES" : "reads", (unsigned long long)(wa - r[i].addr), (unsigned long long)vka_exec_queue_serial(), (unsigned long long)vka_exec_cur_fence(), ok ? (void *)bi->buffer : NULL, ok ? (unsigned long long)bi->offset : 0ull, ok ? (unsigned long long)bi->range : 0ull, md); }
                        }
                    }
                }
                if (ok && storage && (r[i].flags & DMSH_VK_BF_RW)) vka_tex_gw_opaque(r[i].addr, bi->range);   // the shader may store into it
                break;
            case VKA_R_C1: if (sh->c1_len) *bi = (VkDescriptorBufferInfo){sh->c1.buf, sh->c1.off, (sh->c1_len + 15) & ~(VkDeviceSize)15}; else ok = 0; break;
            case VKA_R_SUPPORT:
                if (st == 1 && (sh->e->flags & DMSH_VK_F_ALPHA_TEST) && d->color.alpha_func && d->color.alpha_func != NVN_ALPHA_FUNC_ALWAYS) {   // word 0: the alpha test's reference
                    vka_span s;
                    if (g_drv_memo && g_sm_ref.valid && !memcmp(&g_sm_ref.ref, &d->alpha_ref, 4)) s = g_sm_ref.s;   // the same reference as the last alpha-tested draw: its copy (DM_VK_DRV_MEMO)
                    else { if (!vka_scratch(VKA_SUPPORT_SIZE, V.ub_align, &s)) return 0;
                           memcpy(s.ptr, g_support.ptr, VKA_SUPPORT_SIZE); memcpy(s.ptr, &d->alpha_ref, 4);
                           g_sm_ref.valid = 1; memcpy(&g_sm_ref.ref, &d->alpha_ref, 4); g_sm_ref.s = s; }
                    *bi = (VkDescriptorBufferInfo){s.buf, s.off, VKA_SUPPORT_SIZE};
                } else *bi = (VkDescriptorBufferInfo){g_support.buf, g_support.off, VKA_SUPPORT_SIZE};
                break;
            case VKA_R_DRV: { uint64_t td_ = g_prof ? cntvct() : 0;
                if (!drv_seg(d, st, r[i].bank, pool, bi, c0, sh)) return 0;
                if (g_prof) g_ps[3] += cntvct() - td_;
                break; }
            case VKA_R_GMEM_MAP: ok = gmem_map(bi); break;
            case VKA_R_GMEM_LOG: ok = gmem_log(bi); break;
            case VKA_R_SHARED_SPILL: ok = st == 5 && g_spill_range && g_spill.buf; if (ok) *bi = (VkDescriptorBufferInfo){g_spill.buf, g_spill.off, g_spill_range}; break;
            case VKA_R_MESH_OUT: ok = g_mesh_out.buffer != VK_NULL_HANDLE; if (ok) *bi = g_mesh_out; break;   // (Fritchii: the mesh emulation, mesh_fb_draw)
            case VKA_R_LOOP_GUARD: ok = sh->loop_guard.buf != VK_NULL_HANDLE; if (ok) *bi = (VkDescriptorBufferInfo){sh->loop_guard.buf, sh->loop_guard.off, 16}; break;   // (Lichii, nvn_recompile.c)
            default: ok = 0; break;
            }
            if (!ok && r[i].what == VKA_R_BUF && getenv("FRITCHII_UNBOUND_LOG")) {   // Fritchii: each shader's unbound buffers, once per shader and binding
                static uint64_t seen[256]; static _Atomic int ns; const uint64_t k = sh->key ^ ((uint64_t)r[i].binding << 56) ^ ((uint64_t)st << 48); int found = 0;
                for (int j = 0; j < ns && j < 256; j++) found |= seen[j] == k;
                if (!found) { int j = atomic_fetch_add(&ns, 1); if (j < 256) { seen[j] = k; uint64_t vka_exec_queue_serial(void);
                    VLOG("unbound: shader %016llx stage %d binding %u %s %s at %#llx size %#llx, queue %llu", (unsigned long long)sh->key, st, r[i].binding, storage ? "storage" : "uniform", (r[i].flags & DMSH_VK_BF_RW) ? "written" : "read", (unsigned long long)r[i].addr, (unsigned long long)r[i].size, (unsigned long long)vka_exec_queue_serial()); } }
            }
            if (!ok) { if (V.null_desc) *bi = (VkDescriptorBufferInfo){VK_NULL_HANDLE, 0, VK_WHOLE_SIZE}; else *bi = (VkDescriptorBufferInfo){storage ? g_sink.buf : g_zero.buf, storage ? g_sink.off : g_zero.off, 65536}; }
            w->pBufferInfo = bi;
            if (bind_trace(sh) && r[i].what == VKA_R_BUF && r[i].size && r[i].size <= (1u << 20)) {   // (its bytes as the CPU sees them now, to <DM_TEMP>/cb_<binding>.bin)
                static uint8_t buf[1 << 20]; const char *t = getenv("DM_TEMP");
                if (t && cpu_read(r[i].addr, buf, r[i].size)) { char path[600]; snprintf(path, sizeof path, "%s/cb_%u.bin", t, r[i].binding); FILE *f = fopen(path, "wb"); if (f) { fwrite(buf, 1, r[i].size, f); fclose(f); } }
            }
            if (bind_trace(sh)) VLOG("bind trace %016llx: binding %u buffer what %d addr %#llx size %#llx -> %s range %#llx", (unsigned long long)sh->key, r[i].binding, r[i].what, (unsigned long long)r[i].addr, (unsigned long long)r[i].size, ok ? "bound" : "UNBOUND", (unsigned long long)bi->range);
            if (!ok && bind_trace(sh)) { uint64_t nvn_direct_guest_of(uint64_t); const void *nvn_direct_readable(uint64_t); void vka_mem_describe(uint64_t, char *, size_t); char md[600];
                vka_mem_describe(r[i].addr, md, sizeof md); uint64_t g = nvn_direct_guest_of(r[i].addr);
                VLOG("bind trace %016llx: binding %u unbound: guest %#llx (%s arena), readable %d; segments: %s", (unsigned long long)sh->key, r[i].binding, (unsigned long long)g, g != r[i].addr ? "in" : "not in", nvn_direct_readable(r[i].addr) != NULL, md); }
            break; }
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: case VK_DESCRIPTOR_TYPE_SAMPLER: case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
            int storage = r[i].desc == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; uint64_t ti_ = g_prof ? cntvct() : 0;
            VkImageView v = VK_NULL_HANDLE; VkSampler s = VK_NULL_HANDLE; VkImageLayout lay = VK_IMAGE_LAYOUT_GENERAL;
            if (r[i].desc != VK_DESCRIPTOR_TYPE_SAMPLER) {
                int tid = (r[i].what == VKA_R_TEX || r[i].what == VKA_R_IMG) ? r[i].tid : 0, scaled = 0;
                if (tid) v = vka_tex_id_view_rec(tid, g_cb, r[i].dim, storage, &lay, &scaled);
                {   // Fritchii, FRITCHII_SAMPLE_LOG=<w>x<h>x<d>: each shader that binds an image of that size (once per shader and binding)
                    static int sw = -1, shh, sd; if (sw < 0) { const char *ev = getenv("FRITCHII_SAMPLE_LOG"); sw = 0; if (ev) sscanf(ev, "%dx%dx%d", &sw, &shh, &sd); }
                    if (sw && tid) { vka_img *im = vka_tex_id_img(tid, g_cb);
                        if (im && (int)im->w == sw && (int)im->h == shh && (int)(im->depth > 1 ? im->depth : im->layers) == sd) {
                            static uint64_t seen[256]; static int ns; uint64_t kk = sh->key ^ ((uint64_t)r[i].binding << 56) ^ ((uint64_t)st << 52); int f = 0;
                            for (int q = 0; q < ns; q++) f |= seen[q] == kk;
                            if (!f && ns < 256) { seen[ns++] = kk; VLOG("sample log: shader %016llx stage %d binding %u %s serial %llu %ux%ux%u format %#x", (unsigned long long)sh->key, st, r[i].binding, storage ? "storage" : "sampled", (unsigned long long)im->serial, im->w, im->h, im->depth, im->d.format); } } }
                }
                if (scaled && r[i].what == VKA_R_TEX && r[i].binding >= sh->scale_tex_binding && r[i].binding - sh->scale_tex_binding < sh->scale_tex_bits) {   // (the render scale: RescalingPass's bit)
                    unsigned b = sh->scale_tex_bit + r[i].binding - sh->scale_tex_binding; g_scale_push.tex[b >> 5] |= 1u << (b & 31); }
                // A 2D texture in a 3D or cube slot: what the game leaves bound where a shader's branch does not sample (DKB's materials keep a 1x1 2D texture in their volume slot 12).
                // It reads as unbound, zeros, as nvn_mtl.m's zero 3D / zero cube fallback does; noted once. Other mismatches are reported
                int dd = r[i].dim & DMSH_VK_DIM_MASK, tt = tid && !v ? vka_tex_id_target(tid) : -1;
                if (tid && !v && (dd == 2 || dd == 3) && (tt == NVN_TEXTURE_TARGET_2D || tt == NVN_TEXTURE_TARGET_2D_ARRAY)) {
                    static _Atomic int noted; if (!atomic_exchange(&noted, 1)) { char why[320]; vka_tex_id_desc(tid, why, sizeof why);
                        VLOG("%s binding fallback: shader %016llx binding %u, texture ID %d is a 2D texture (%s): it reads as zeros (the shader may leave it unused)", dd == 2 ? "3D" : "cube", (unsigned long long)sh->key, r[i].binding, tid, why); }
                } else if (tid && !v && getenv("FRITCHII_UNBOUND_TEX_LOG") && sh->key == strtoull(getenv("FRITCHII_UNBOUND_TEX_LOG"), NULL, 16)) {   // Fritchii: that shader's unresolved textures, every time
                    char why[320]; vka_tex_id_desc(tid, why, sizeof why); uint64_t vka_present_filled(void);
                    VLOG("unbound texture: present %llu shader %016llx binding %u texture ID %d shape %#x: %s", (unsigned long long)vka_present_filled(), (unsigned long long)sh->key, r[i].binding, tid, r[i].dim, why);
                } else if (tid && !v) { static _Atomic int nm; if (atomic_fetch_add(&nm, 1) < 8) { char why[320]; vka_tex_id_desc(tid, why, sizeof why);
                    VRED("shader %016llx binding %u: texture ID %d has no view of shape %#x%s: it reads as unbound (%s)", (unsigned long long)sh->key, r[i].binding, tid, r[i].dim, storage ? " (storage)" : "", why); } }
                if (!v && !V.null_desc) v = placeholder(r[i].dim, storage);
                if (!v && !V.null_desc) { VRED_ONCE("an unbound image of shape %#x and no null descriptors: the draw is skipped", r[i].dim); return 0; }
            }
            if (r[i].desc == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || r[i].desc == VK_DESCRIPTOR_TYPE_SAMPLER) {
                s = r[i].sid ? vka_samp_id_rec(r[i].sid) : VK_NULL_HANDLE;
                if (!s) s = vka_samp_default();
            }
            *ii = (VkDescriptorImageInfo){s, v, lay};
            if (bind_trace(sh)) { char why[320] = ""; int tid = (r[i].what == VKA_R_TEX || r[i].what == VKA_R_IMG) ? r[i].tid : 0; if (tid) vka_tex_id_desc(tid, why, sizeof why);
                vka_img *tim = tid ? vka_tex_id_img(tid, g_cb) : NULL;
                VLOG("bind trace %016llx: binding %u image dim %#x tid %d sid %d -> view %s %p image %p mem %p+%#llx z%d (%s)", (unsigned long long)sh->key, r[i].binding, r[i].dim, tid, r[i].sid, v ? "ok" : "NULL", (void *)v, tim ? (void *)tim->img : NULL, tim ? (void *)tim->m.dm : NULL, tim ? (unsigned long long)tim->m.off : 0ull, tim ? (int)tim->m.z : -1, why); }
            w->pImageInfo = ii; if (g_prof) { g_ps[4] += cntvct() - ti_; g_ps[5]++; }
            break; }
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            if (r[i].what != VKA_R_NULL) VRED_ONCE("texel buffers are not on Vulkan yet (shader %016llx binding %u): bound as null", (unsigned long long)sh->key, r[i].binding);
            if (!V.null_desc) { VRED_ONCE("a texel buffer binding and no null descriptors: the draw is skipped"); return 0; }
            g_w.bv[k] = VK_NULL_HANDLE; w->pTexelBufferView = &g_w.bv[k];
            break;
        default: VRED_ONCE("descriptor type %u in a binding record", r[i].desc); return 0;
        }
        g_w.n++;
    }
    return 1;
}
// Each stage's bindings, walked once a draw or dispatch (walk_all): the images first used are made from them (make_images), then set 0 is written from them (bind_sets)
static struct { int n[VKA_NSTAGE], pool[VKA_NSTAGE]; char why[VKA_NSTAGE][200]; vka_res r[VKA_NSTAGE][512]; } g_wk;
uint64_t vka_frame(void);
static void walk_all(const vka_dstate *d, vka_shader *const sh[VKA_NSTAGE]) {
    uint64_t t0_ = g_prof ? cntvct() : 0;
    for (int st = 0; st < VKA_NSTAGE; st++) {
        vka_shader *s = sh[st]; g_wk.n[st] = 0; g_wk.pool[st] = 0; if (!s) continue;
        g_wk.n[st] = vka_walk(&d->b, st, s->e, s->b, s->lib->h->ssbo_align, V.sb_align, rd_cb, NULL, g_wk.r[st], 512, &g_wk.pool[st], g_wk.why[st], sizeof g_wk.why[st], s->hprog, s->hprog_words);
    }
    if (g_prof) g_ps[0] += cntvct() - t0_;
}
// Writes set 0 from the walk (walk_all, just before) and binds it (and set 1 for shaders that read the pools). c0: where the compute stage's c0 went (an indirect dispatch copies its counts there)
// The last push descriptor set per bind point, as the words that decide it (layout, each write's binding, type and count, each descriptor's info): a draw whose writes
// are the same word for word skips its push (DM_VK_PUSH_DEDUP=0: every draw pushes). Pushed descriptors stay bound across draws and pipeline binds while the layout stays
// the same (Vulkan's pipeline layout compatibility); a new command buffer or a set bound in their place forgets them (vka_dyn_invalidate, bind_sets)
static struct { uint64_t *w; size_t n, cap; } g_pd[2];
static uint64_t g_pd_skip, g_pd_push, g_pd_skip_all, g_pd_push_all, g_pd_delta_w, g_pd_delta_all;
// The replay statistics' line: driver segments bound again (DM_VK_DRV_MEMO) and pushes skipped (DM_VK_PUSH_DEDUP) since the last line
void vka_drv_memo_stats(char *b, size_t n) {
    static uint64_t h0, m0, s0, p0;
    uint64_t h = g_dm_hit - h0, m = g_dm_miss - m0, sk = g_pd_skip_all - s0, pu = g_pd_push_all - p0;
    h0 = g_dm_hit; m0 = g_dm_miss; s0 = g_pd_skip_all; p0 = g_pd_push_all;
    if (!h && !m && !sk && !pu) { if (n) b[0] = 0; return; }
    snprintf(b, n, "descriptors: driver segments %.0f %% the same bytes as the draw before (bound again: DM_VK_DRV_MEMO); push descriptor sets %.0f %% skipped as the same (DM_VK_PUSH_DEDUP); "
             "partial pushes (DM_VK_PUSH_DELTA) wrote %.0f %% of their descriptors", h + m ? 100.0 * h / (double)(h + m) : 0.0, sk + pu ? 100.0 * sk / (double)(sk + pu) : 0.0,
             g_pd_delta_all ? 100.0 * g_pd_delta_w / (double)g_pd_delta_all : 0.0);
    g_pd_delta_w = g_pd_delta_all = 0;
}
static VkWriteDescriptorSet g_dw[512]; static uint32_t g_dwn;   // the writes that changed (push_same returns 2)
static int push_same(VkPipelineBindPoint bp, VkPipelineLayout layout) {
    static uint64_t *buf; static size_t cap;
    size_t need = 2 + (size_t)g_w.n * 5, k = 0;
    if (need > cap) { cap = need * 2; buf = realloc(buf, cap * sizeof *buf); if (!buf) { cap = 0; return 0; } }
    buf[k++] = (uint64_t)layout; buf[k++] = g_w.n;
    for (uint32_t i = 0; i < g_w.n; i++) {
        const VkWriteDescriptorSet *w = &g_w.w[i];
        buf[k++] = (uint64_t)w->dstBinding | (uint64_t)w->dstArrayElement << 16 | (uint64_t)w->descriptorType << 32 | (uint64_t)w->descriptorCount << 48;
        if (w->descriptorCount != 1) return (g_pd[bp == VK_PIPELINE_BIND_POINT_COMPUTE].n = 0), 0;   // (the walk writes one descriptor per write)
        if (w->pBufferInfo) { buf[k++] = (uint64_t)w->pBufferInfo->buffer; buf[k++] = w->pBufferInfo->offset; buf[k++] = w->pBufferInfo->range; }
        else if (w->pImageInfo) { buf[k++] = (uint64_t)w->pImageInfo->sampler; buf[k++] = (uint64_t)w->pImageInfo->imageView; buf[k++] = w->pImageInfo->imageLayout; }
        else if (w->pTexelBufferView) { buf[k++] = (uint64_t)*w->pTexelBufferView; buf[k++] = 0; buf[k++] = 0; }
        else { buf[k++] = 1; buf[k++] = 1; buf[k++] = 1; }
    }
    int c = bp == VK_PIPELINE_BIND_POINT_COMPUTE;
    if (VKA_ENV("DM_VK_PUSH_DEDUP", 1) && g_pd[c].n == k && !memcmp(g_pd[c].w, buf, k * sizeof *buf)) { g_pd_skip++; g_pd_skip_all++; return 1; }
    // DM_VK_PUSH_DELTA=1 (live, default 0 until phone-checked): the same layout and the same writes in the same order as the last push, some of them with other descriptors:
    // only those are pushed. Vulkan lets push descriptors be updated incrementally; the others stay as the last push left them (until the set is disturbed: a new command
    // buffer or part, a set bound in its place, which forget g_pd)
    g_dwn = 0;
    if (VKA_ENV("DM_VK_PUSH_DELTA", 0) && g_pd[c].n == k && g_pd[c].w[0] == buf[0] && g_pd[c].w[1] == buf[1]) {
        int ok = 1;
        for (uint32_t i = 0; i < g_w.n && ok; i++) {
            const uint64_t *a = g_pd[c].w + 2 + 4 * (size_t)i, *b = buf + 2 + 4 * (size_t)i;
            if (a[0] != b[0]) ok = 0; else if (memcmp(a + 1, b + 1, 3 * sizeof *a)) g_dw[g_dwn++] = g_w.w[i];
        }
        if (!ok) g_dwn = 0;
    }
    if (k > g_pd[c].cap) { g_pd[c].cap = k * 2; g_pd[c].w = realloc(g_pd[c].w, g_pd[c].cap * sizeof *buf); if (!g_pd[c].w) { g_pd[c].cap = g_pd[c].n = 0; return 0; } }
    memcpy(g_pd[c].w, buf, k * sizeof *buf); g_pd[c].n = k; g_pd_push++; g_pd_push_all++;
    if (g_dwn) { g_pd_delta_w += g_dwn; g_pd_delta_all += g_w.n; return 2; }
    return 0;
}
// Lichii: the pixel shader's set (VKA_SET_FS) of a draw, allocated from the submission slot's pools and written, or the one an earlier draw of the slot
// wrote with the same descriptors (draws repeat a material's textures and constants). Its own set lets the fragment shader library depend on that shader only (vka_pso.c)
enum { FSC = 8192, FSC_PROBE = 16 };
static struct { uint64_t h; VkDescriptorSetLayout l; uint64_t *w; uint32_t n; VkDescriptorSet set; } g_fsc[FSC];
static unsigned g_fsc_n; static uint64_t g_fsc_hit, g_fsc_miss;
static void fsc_reset(void) {
    if (!g_fsc_n) return;
    for (unsigned i = 0; i < FSC; i++) free(g_fsc[i].w);
    memset(g_fsc, 0, sizeof g_fsc); g_fsc_n = 0;
}
void vka_fsc_stats(uint64_t *hit, uint64_t *miss) { *hit = g_fsc_hit; *miss = g_fsc_miss; }
static VkDescriptorSet fs_set_of(VkDescriptorSetLayout l, VkWriteDescriptorSet *w, uint32_t n) {
    static uint64_t *buf; static size_t cap;
    size_t need = 1 + 4 * (size_t)n, k = 0; int keyed = 1;
    if (need > cap) { cap = need * 2; buf = realloc(buf, cap * sizeof *buf); if (!buf) { cap = 0; keyed = 0; } }
    uint64_t h = 1469598103934665603ull ^ (uint64_t)l;
    for (uint32_t i = 0; keyed && i < n; i++) {
        if (w[i].descriptorCount != 1) { keyed = 0; break; }
        uint64_t a = (uint64_t)w[i].dstBinding | (uint64_t)w[i].dstArrayElement << 16 | (uint64_t)w[i].descriptorType << 32, b = 0, c = 0, e = 0;
        if (w[i].pBufferInfo) { b = (uint64_t)w[i].pBufferInfo->buffer; c = w[i].pBufferInfo->offset; e = w[i].pBufferInfo->range; }
        else if (w[i].pImageInfo) { b = (uint64_t)w[i].pImageInfo->sampler; c = (uint64_t)w[i].pImageInfo->imageView; e = w[i].pImageInfo->imageLayout; }
        else if (w[i].pTexelBufferView) b = (uint64_t)*w[i].pTexelBufferView;
        buf[k++] = a; buf[k++] = b; buf[k++] = c; buf[k++] = e;
        for (int q = 0; q < 4; q++) { h ^= buf[k - 4 + q]; h *= 0x100000001b3ull; h ^= h >> 29; }
    }
    unsigned slot = 0;
    if (keyed) for (unsigned p = 0; p < FSC_PROBE; p++) {
        slot = (unsigned)(h + p) & (FSC - 1);
        if (!g_fsc[slot].set) break;
        if (g_fsc[slot].h == h && g_fsc[slot].l == l && g_fsc[slot].n == n && !memcmp(g_fsc[slot].w, buf, k * sizeof *buf)) { g_fsc_hit++; return g_fsc[slot].set; }
        slot = FSC;   // (full probe: not kept)
    }
    g_fsc_miss++;
    VkDescriptorSet set = set_alloc(l); if (!set) return VK_NULL_HANDLE;
    for (uint32_t i = 0; i < n; i++) w[i].dstSet = set;
    if (n) vk.vkUpdateDescriptorSets(V.dev, n, w, 0, NULL);
    if (keyed && slot < FSC && !g_fsc[slot].set && 4 * (g_fsc_n + 1) <= 3 * FSC) {
        uint64_t *c = malloc((k ? k : 1) * sizeof *c);
        if (c) { memcpy(c, buf, k * sizeof *c); g_fsc[slot] = (typeof(g_fsc[0])){h, l, c, n, set}; g_fsc_n++; }
    }
    return set;
}
// One stage's descriptor writes, appended to g_w. 0 = the draw cannot run (logged)
static int stage_bind(const vka_dstate *d, vka_shader *const sh[VKA_NSTAGE], int st, c0_at *c0, int *any_pool) {
    vka_shader *s = sh[st]; if (!s) return 1;
    if (st == 1 && d->rast_discard) return 1;   // no fragments: its stage's records are not read (the layout keeps them unwritten: partially bound is not needed for push descriptors)
    int n = g_wk.n[st];
    if (n < 0) { static _Atomic int k; if (atomic_fetch_add(&k, 1) < 16) VRED("shader %016llx: %s; skipped", (unsigned long long)s->key, g_wk.why[st]); return 0; }
    *any_pool |= g_wk.pool[st];
    return stage_writes(d, st, s, g_wk.r[st], n, g_wk.pool[st], st == 5 ? c0 : NULL);
}
static int bind_sets(const vka_dstate *d, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, VkPipelineBindPoint bp, c0_at *c0) {
    g_w.n = 0;
    if (vka_scale_on()) memset(&g_scale_push, 0, sizeof g_scale_push);
    int any_pool = 0;
    const int fs_own = bp == VK_PIPELINE_BIND_POINT_GRAPHICS && lay->set_fs;   // (Lichii: the pixel shader's writes go to its own set, after the others)
    uint64_t t1_ = g_prof ? cntvct() : 0;
    for (int st = 0; st < VKA_NSTAGE; st++) if (!(fs_own && st == 1) && !stage_bind(d, sh, st, c0, &any_pool)) return 0;
    if (g_prof) g_ps[1] += cntvct() - t1_;
    if (sh[2] && sh[2]->gs_fx > 0 && bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {   // the claim words, zeroed (vka_spv_gs_claim); bound also when the draw takes no claim (the layout has them)
        if (g_w.n == 512) { VRED_ONCE("more than 512 descriptors in one draw"); return 0; }
        VkDeviceSize n = 4 * (VkDeviceSize)(g_claim_words ? g_claim_words : 1); vka_span cs;
        if (!vka_scratch(n, V.sb_align, &cs)) return 0;
        memset(cs.ptr, 0, (size_t)n);
        uint32_t k = g_w.n++;
        g_w.bi[k] = (VkDescriptorBufferInfo){cs.buf, cs.off, n};
        g_w.w[k] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = VKA_GS_CLAIM_BINDING, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &g_w.bi[k]};
    }
    const uint32_t npre = g_w.n;
    if (fs_own && !stage_bind(d, sh, 1, c0, &any_pool)) return 0;
    const uint32_t nall = g_w.n;
    {   // DM_VK_WATCH_FS=<key>: the descriptor writes of that pixel shader's last draws, dumped at a device loss (vka_watch_dump)
        static uint64_t wk = ~0ull; if (wk == ~0ull) { const char *e = getenv("DM_VK_WATCH_FS"); wk = e ? strtoull(e, NULL, 16) : 0; }
        if (wk && sh[1] && sh[1]->key == wk) {
            extern char vka_watch[4][4096]; extern unsigned vka_watch_n; char *o = vka_watch[vka_watch_n++ % 4]; size_t at = 0; o[0] = 0;
            for (uint32_t i = 0; i < nall && at < 4000; i++) { const VkWriteDescriptorSet *w = &g_w.w[i];
                if (w->pBufferInfo) at += (size_t)snprintf(o + at, 4096 - at, " b%u:t%d buf %llx +%llx r%llx", w->dstBinding, w->descriptorType, (unsigned long long)(uintptr_t)w->pBufferInfo->buffer, (unsigned long long)w->pBufferInfo->offset, (unsigned long long)w->pBufferInfo->range);
                else if (w->pImageInfo) at += (size_t)snprintf(o + at, 4096 - at, " b%u:t%d view %llx smp %llx", w->dstBinding, w->descriptorType, (unsigned long long)(uintptr_t)w->pImageInfo->imageView, (unsigned long long)(uintptr_t)w->pImageInfo->sampler);
                else if (w->pTexelBufferView) at += (size_t)snprintf(o + at, 4096 - at, " b%u:t%d tbv %llx", w->dstBinding, w->descriptorType, (unsigned long long)(uintptr_t)*w->pTexelBufferView); }
        }
    }
    g_w.n = npre;   // set 0: the writes before the pixel shader's
    uint64_t t2_ = g_prof ? cntvct() : 0;
    if (lay->push) { int ps = g_w.n ? push_same(bp, lay->layout) : 1;
                     if (ps == 0) vk.vkCmdPushDescriptorSetKHR(g_cb, bp, lay->layout, 0, g_w.n, g_w.w);
                     else if (ps == 2) vk.vkCmdPushDescriptorSetKHR(g_cb, bp, lay->layout, 0, g_dwn, g_dw); }
    else if (lay->set0) {
        g_pd[bp == VK_PIPELINE_BIND_POINT_COMPUTE].n = 0;   // set 0 bound as a set: the last push is no longer what the draws see
        VkDescriptorSet set = set_alloc(lay->set0); if (!set) return 0;
        for (uint32_t i = 0; i < g_w.n; i++) g_w.w[i].dstSet = set;
        if (g_w.n) vk.vkUpdateDescriptorSets(V.dev, g_w.n, g_w.w, 0, NULL);
        vk.vkCmdBindDescriptorSets(g_cb, bp, lay->layout, 0, 1, &set, 0, NULL);
    }
    g_w.n = nall;
    if (fs_own) {
        VkDescriptorSet fs = fs_set_of(lay->set_fs, g_w.w + npre, nall - npre); if (!fs) return 0;
        vk.vkCmdBindDescriptorSets(g_cb, bp, lay->layout, VKA_SET_FS, 1, &fs, 0, NULL);
    }
    if (g_prof) g_ps[2] += cntvct() - t2_;
    if (any_pool && bp == VK_PIPELINE_BIND_POINT_COMPUTE && vka_tex_lazy_pending()) vka_tex_lazy_make(g_cb);   // (Lichii: as make_images does for draws; no pass is open)
    if (any_pool) { VkDescriptorSet ps = vka_bindless_set(); if (ps) vk.vkCmdBindDescriptorSets(g_cb, bp, lay->layout, 1, 1, &ps, 0, NULL); else VRED_ONCE("a shader reads the texture pool and there is no pool set"); }
    if (vka_scale_on()) {   // the render scale's push constants (Lichii's RescalingLayout): every layout has them while it scales (vka_pso.c pipe_layout)
        g_scale_push.down = bp == VK_PIPELINE_BIND_POINT_GRAPHICS && R.scaled ? (float)(1u << vka_scale_shift) / (float)vka_scale_up : 1.0f;
        if (!g_scale_pushed.valid || memcmp(&g_scale_pushed.p, &g_scale_push, sizeof g_scale_push)) {
            vk.vkCmdPushConstants(g_cb, lay->layout, VK_SHADER_STAGE_ALL, 0, sizeof g_scale_push, &g_scale_push);
            g_scale_pushed.p = g_scale_push; g_scale_pushed.valid = 1;
        }
    }
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {   // the render area, for shaders that read it in place of the console's blit parameters (Lichii's BlitConstantsPass)
        int area = 0;
        for (int st = 0; st < VKA_NSTAGE; st++) if (sh[st] && (sh[st]->e->flags & VKA_F_RENDER_AREA)) area = 1;
        float a[4] = {(float)R.own.width, (float)R.own.height, 0, 0};
        if (area && (!g_area_pushed.valid || memcmp(g_area_pushed.a, a, sizeof a))) {
            vk.vkCmdPushConstants(g_cb, lay->layout, VK_SHADER_STAGE_ALL, NVN_DIRECT_RENDER_AREA_OFFSET, sizeof a, a);
            memcpy(g_area_pushed.a, a, sizeof a); g_area_pushed.valid = 1;
        }
    }
    return 1;
}
// Images first used by a draw (from the walk, walk_all), or remade with storage use (vka_tex.c img_storage_upgrade): dry = only whether there is one
static int make_images(const vka_dstate *d, vka_shader *const sh[VKA_NSTAGE], int dry) {
    (void)d; int any = 0;
    for (int st = 0; st < VKA_NSTAGE; st++) {
        if (!sh[st]) continue;
        const vka_res *r = g_wk.r[st];
        for (int i = 0; i < g_wk.n[st]; i++) if ((r[i].what == VKA_R_TEX || r[i].what == VKA_R_IMG) && r[i].tid
                                                 && (vka_tex_id_pending_rec(r[i].tid, r[i].dim) || (r[i].what == VKA_R_IMG && vka_tex_id_storage_pending(r[i].tid)))) {   // (an image remade with storage use copies its content: outside the pass)
            any = 1; if (dry) return 1;
            vka_tex_id_view(r[i].tid, g_cb, r[i].dim, r[i].what == VKA_R_IMG, NULL);
        }
        if (g_wk.pool[st] && vka_tex_lazy_pending()) { any = 1; if (dry) return 1; vka_tex_lazy_make(g_cb); }   // (Lichii: the pool read by index: every ID's image first)
    }
    return any;
}

// The draw writes memory: a storage buffer or image it writes, raw global stores (their write log), a geometry shader's claimed writes (from the walk, walk_all)
static int draw_writes(vka_shader *const sh[VKA_NSTAGE], const vka_dstate *d) {
    if (sh[2] && sh[2]->gs_fx > 0) return 1;
    for (int st = 0; st < VKA_NSTAGE; st++) {
        if (!sh[st] || (st == 1 && d->rast_discard)) continue;
        const vka_res *r = g_wk.r[st];
        for (int i = 0; i < g_wk.n[st]; i++) if ((r[i].flags & DMSH_VK_BF_RW) || r[i].what == VKA_R_IMG || r[i].what == VKA_R_GMEM_LOG) return 1;
    }
    return 0;
}
// After a game barrier taken inside the pass (vka_rp_game_barrier): this draw needs the pass ended first when it writes memory (earlier draws of the pass may read what it
// overwrites), reads the texture pool directly (any texture, the pass's targets among them), or samples a texture whose image the pass draws into
static int bar_hazard(vka_shader *const sh[VKA_NSTAGE], const vka_dstate *d) {
    if (draw_writes(sh, d)) return 1;
    for (int st = 0; st < VKA_NSTAGE; st++) {
        if (!sh[st] || (st == 1 && d->rast_discard)) continue;
        if (g_wk.pool[st] || g_wk.n[st] < 0) return 1;
        const vka_res *r = g_wk.r[st];
        for (int i = 0; i < g_wk.n[st]; i++) if ((r[i].what == VKA_R_TEX || r[i].what == VKA_R_TBUF) && r[i].tid && vka_tex_id_in_pass(r[i].tid)) return 1;
    }
    return 0;
}

// ---------- Draws ----------
// A program's shaders by its serial (a program that gets new shaders gets a new serial, hle_nvn.c), remembered for the draws after: the shader table's lock and lookup
// were taken once per stage of every draw
static struct { uint64_t serial; vka_progsh ps; } g_pm[64];
static int resolve(const prog_t *p, int st, vka_shader **out, unsigned *bad) {
    *out = NULL; if (!p) return 1;
    vka_progsh ps;
    uint64_t sr = p->serial; typeof(g_pm[0]) *m = &g_pm[(sr ^ sr >> 6) & 63];
    if (sr && m->serial == sr && vka_progsh_current(&m->ps)) ps = m->ps;
    else { if (!vka_prog_shaders(p, &ps)) return 0; if (sr) { m->serial = sr; m->ps = ps; } }
    if (ps.bad & (1u << st | VKA_BAD_MISSING)) { *bad |= 1u << st; return 1; }
    *out = ps.sh[st]; return 1;
}
// Vertex input: an attribute per input location the vertex shader reads (Location = NVN attribute index), each from its stream; an input no attribute feeds reads a zero buffer.
// With VK_EXT_vertex_input_dynamic_state the descriptions are set per draw, else they go into the pipeline key
static struct { VkVertexInputBindingDescription2EXT b[33]; VkVertexInputAttributeDescription2EXT a[32]; uint32_t nb, na, used, zb; int zero; } VI;
static void vin_build(const vka_dstate *d, const vka_shader *vs, vka_gkey *k) {
    VI.nb = VI.na = VI.used = 0; VI.zero = 0;
    VI.zb = V.max_vin_bindings > 32 ? 31 : V.max_vin_bindings > 16 ? V.max_vin_bindings - 1 : 15;   // the zero buffer's binding: past the 16 streams when the device has room
    for (uint32_t loc = 0; loc < 32; loc++) {
        if (!(vs->e->vin_mask >> loc & 1)) continue;
        int want = (int)(vs->e->vin_types >> (2 * loc) & 3), changed = 0, retype = 0;
        const va_st *a = loc < 16 && (int)loc < d->nattr ? &d->attr[loc] : NULL;
        VkFormat f = a && a->fmt ? vin_format(a->fmt, want, &changed, &retype) : VK_FORMAT_UNDEFINED;
        if (retype == DMSH_VK_TYPE_UINT) k->vin_u |= 1u << loc; else if (retype == DMSH_VK_TYPE_SINT) k->vin_s |= 1u << loc;
        if (a && a->fmt && !f) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 4) VRED("vertex attribute format %#x is not mapped: the input reads zeros", a->fmt); }
        if (changed) { static uint64_t seen[4]; unsigned b = a->fmt & 255; if (!(seen[b >> 6] >> (b & 63) & 1)) { seen[b >> 6] |= 1ull << (b & 63);
            VRED("vertex attribute format %#x feeds a %s input: bound as VkFormat %d of that class (values may differ from Maxwell's fetch)", a->fmt, want == 0 ? "float" : want == 1 ? "sint" : "uint", f); } }
        if (!f) {
            f = want == DMSH_VK_TYPE_UINT ? VK_FORMAT_R32G32B32A32_UINT : want == DMSH_VK_TYPE_SINT ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_SFLOAT;
            VI.a[VI.na++] = (VkVertexInputAttributeDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, NULL, loc, VI.zb, f, 0};
            VI.zero = 1; continue;
        }
        uint32_t st = a->stream & 15;
        VI.a[VI.na++] = (VkVertexInputAttributeDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, NULL, loc, st, f, a->offset};
        VI.used |= 1u << st;
    }
    for (uint32_t st = 0; st < 16; st++) if (VI.used >> st & 1) {
        uint32_t div = d->stream[st].divisor;
        if (div > 1 && !V.divisor) { VRED_ONCE("instance divisor %u and no vertexAttributeInstanceRateDivisor: drawn with divisor 1", div); div = 1; }
        VI.b[VI.nb++] = (VkVertexInputBindingDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, NULL, st, d->stream[st].stride,
                                                               div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, div ? div : 1};
    }
    if (VI.zero) VI.b[VI.nb++] = (VkVertexInputBindingDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, NULL, VI.zb, 0, VK_VERTEX_INPUT_RATE_VERTEX, 1};
    if (!V.vin_dyn) {
        k->vin_static = 1;
        for (uint32_t i = 0; i < VI.na; i++) { uint32_t l = VI.a[i].location; k->va[l].fmt = VI.a[i].format; k->va[l].offset = VI.a[i].offset; k->va[l].binding = (uint8_t)VI.a[i].binding; k->va[l].used = 1; }
        for (uint32_t i = 0; i < VI.nb; i++) { uint32_t b = VI.b[i].binding; k->vb[b].stride = VI.b[i].stride; k->vb[b].divisor = VI.b[i].inputRate ? VI.b[i].divisor : 0; k->vb[b].used = 1; }
    }
}
// The vertex buffers the command buffer holds (slot: buffer, offset, size): a draw binds only the slots that changed, consecutive ones in one call. Vulkan keeps vertex
// buffer bindings across pipeline binds and passes for the life of the command buffer; forgotten with the dynamic state (vka_dyn_invalidate: a new command buffer or
// part). Profile p2: vin_bind was 13 % of the replay thread, one vkCmdBindVertexBuffers2 a stream a draw. DM_VK_VB_FILTER=0 (live): every slot every draw
static struct { int valid, vin_stale; VkBuffer b[33]; VkDeviceSize o[33], sz[33]; uint32_t vnb, vna; VkVertexInputBindingDescription2EXT vb[33]; VkVertexInputAttributeDescription2EXT va[32]; } VB;
static void vb_set(uint32_t slot, VkBuffer b, VkDeviceSize o, VkDeviceSize sz, uint32_t *n0, uint32_t *nn, VkBuffer *rb, VkDeviceSize *ro, VkDeviceSize *rs) {
    if (slot >= 33) return;
    if (VB.valid && g_vb_filter && VB.b[slot] == b && VB.o[slot] == o && VB.sz[slot] == sz) return;
    VB.b[slot] = b; VB.o[slot] = o; VB.sz[slot] = sz;
    if (*nn && *n0 + *nn != slot) { vk.vkCmdBindVertexBuffers2(g_cb, *n0, *nn, rb, ro, rs, NULL); *nn = 0; }   // a run ends: bound in one call
    if (!*nn) *n0 = slot;
    rb[*nn] = b; ro[*nn] = o; rs[*nn] = sz; (*nn)++;
}
static void vin_bind(const vka_dstate *d) {
    if (!VB.valid) memset(&VB, 0, sizeof VB);
    // The vertex input state too only when it changed (it was set every draw: up to 65 descriptions copied into the encoder's ring and the driver), and again after
    // every pipeline bind: Vulkan keeps dynamic vertex input across vkCmdBindPipeline, but drivers have lost it there before (hardening; the buffers stay filtered)
    if (V.vin_dyn) {
        int same = VB.valid && !VB.vin_stale && g_vb_filter && VB.vnb == VI.nb && VB.vna == VI.na && !memcmp(VB.vb, VI.b, VI.nb * sizeof *VI.b) && !memcmp(VB.va, VI.a, VI.na * sizeof *VI.a);
        if (!same) { vk.vkCmdSetVertexInputEXT(g_cb, VI.nb, VI.b, VI.na, VI.a); VB.vnb = VI.nb; VB.vna = VI.na; memcpy(VB.vb, VI.b, VI.nb * sizeof *VI.b); memcpy(VB.va, VI.a, VI.na * sizeof *VI.a); }
        VB.vin_stale = 0;
    }
    VkBuffer rb[33]; VkDeviceSize ro[33], rs[33]; uint32_t n0 = 0, nn = 0;
    for (uint32_t st = 0; st < 16; st++) if (VI.used >> st & 1) {   // each stream at its address, as far as it is bound and placed
        VkBuffer b = g_zero.buf; VkDeviceSize o = g_zero.off, sz = 65536;
        uint64_t addr = d->vbuf[st].address, size = d->vbuf[st].size;
        if (addr) { VkBuffer cb; VkDeviceSize co; uint64_t run = vka_addr_run(addr, size ? size : 1ull << 30, &cb, &co);
                    if (run) { b = cb; o = co; sz = run; } else VRED_ONCE("vertex stream at %#llx is not in placed guest RAM: it reads zeros", (unsigned long long)addr); }
        vb_set(st, b, o, sz, &n0, &nn, rb, ro, rs);
    }
    if (VI.zero) vb_set(VI.zb, g_zero.buf, g_zero.off, 65536, &n0, &nn, rb, ro, rs);
    if (nn) vk.vkCmdBindVertexBuffers2(g_cb, n0, nn, rb, ro, rs, NULL);
    VB.valid = 1;
}

// The guest indices of an indexed draw as 32-bit values (the CPU's view): for primitives Vulkan has no topology for and 8-bit indices on a device without them
static uint32_t *read_indices(uint64_t addr, int itype, uint32_t count) {
    size_t isz = itype == 0 ? 1 : itype == 1 ? 2 : 4; uint8_t *raw = malloc(isz * count + 4); uint32_t *o = malloc(4 * (size_t)count + 4);
    if (!raw || !o || !cpu_read(addr, raw, isz * count)) { free(raw); free(o); return NULL; }
    for (uint32_t i = 0; i < count; i++) o[i] = isz == 1 ? raw[i] : isz == 2 ? ((uint16_t *)raw)[i] : ((uint32_t *)raw)[i];
    free(raw); return o;
}

// The dynamic state the command buffer holds: each draw sets only what changed (the draw path set some 25 states a draw). Valid from the first draw of a submission until
// something else binds a graphics pipeline with static state (the masked stencil clear: vka_dyn_invalidate) or the submission ends; every graphics pipeline of the draws has
// the same dynamic states (vka_pso.c), so binding one keeps them, except a mesh pipeline, which has no topology or primitive restart (vka_draw resets DY.topo / DY.pr after one)
static struct { int valid; VkPipeline pipe; VkViewport vp; VkRect2D sc; uint32_t cull, ff, topo, dt, dw, dc, dbt, stt, dbe, rd, pr, sop[8], smask[6]; float db[3], bc[4], dbr[2]; } DY;
#define DY_SET(f, val, call) do { if (!DY.valid || !g_dyn_filter || memcmp(&DY.f, &(val), sizeof DY.f)) { memcpy(&DY.f, &(val), sizeof DY.f); call; } } while (0)
#define DY_SET1(f, val, call) do { uint32_t v_ = (val); if (!DY.valid || !g_dyn_filter || DY.f != v_) { DY.f = v_; call; } } while (0)
// The index buffer the command buffer holds: bound again only when it changes (DM_VK_VB_FILTER), forgotten with the dynamic state
static struct { int valid; VkBuffer b; VkDeviceSize o; VkIndexType t; } IB;
static void ib_bind(VkBuffer b, VkDeviceSize o, VkIndexType t) {
    if (IB.valid && g_vb_filter && IB.b == b && IB.o == o && IB.t == t) return;
    vk.vkCmdBindIndexBuffer(g_cb, b, o, t); IB.valid = 1; IB.b = b; IB.o = o; IB.t = t;
}
void vka_dyn_invalidate(void) { DY.valid = 0; VB.valid = 0; IB.valid = 0; g_pd[0].n = g_pd[1].n = 0; g_scale_pushed.valid = 0; g_area_pushed.valid = 0; }
#define PT(i) do { if (g_prof) { uint64_t t_ = cntvct(); if ((i) > 0 && g_pt_ph == (i) - 1) g_pt[(i) > 0 ? (i) - 1 : 0] += t_ - g_pt_last; g_pt_last = t_; g_pt_ph = (i); } } while (0)
void vka_draw_prof(char *b, size_t n, double draws) {
    if (!g_prof || draws <= 0) { if (n) b[0] = 0; return; }
    uint64_t f;
#if defined(__x86_64__)
    f = 1000000000ull;
#else
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
#endif
    double us = 1e6 / (double)f / draws;
    static const char *nm[8] = {"resolve", "layout", "images+pass+key", "vertex input", "pipeline", "sets", "vertex buffers+dynamic", "draw"};
    size_t o = 0; for (int i = 0; i < 8 && o < n; i++) o += snprintf(b + o, n - o, "%s%s %.2f", i ? ", " : "", nm[i], g_pt[i] * us);
    if (o < n) o += snprintf(b + o, n - o, "; pushes %.0f%% skipped as the last one", g_pd_skip + g_pd_push ? 100.0 * g_pd_skip / (double)(g_pd_skip + g_pd_push) : 0.0);
    g_pd_skip = g_pd_push = 0;
    if (o < n) o += snprintf(b + o, n - o, "; sets: walk %.2f, writes %.2f (driver segments %.2f, %.1f images %.2f), push %.2f", g_ps[0] * us, g_ps[1] * us, g_ps[3] * us, g_ps[5] / draws, g_ps[4] * us, g_ps[2] * us);
    memset(g_ps, 0, sizeof g_ps);
    memset(g_pt, 0, sizeof g_pt);
}
typedef struct { VkCommandBuffer cb; VkRect2D r; } excl_arg;   // Lichii: the exclusive scissor, through the encoder thread when it records
static void excl_call(const void *a) { const excl_arg *x = a; V.set_excl_scissor(x->cb, 0, 1, &x->r); }
// The records of an indirect mesh draw of one dimension are {count, first} (DrawMeshTasksIndirect: derived from the NVN2 driver in OoT's sdk NSO, function at sdk+0x6814e0,
// a 2-word DMA into the draw method that DrawMeshTasks writes count, first to); VK_EXT_mesh_shader reads {x, y, z}. Each record's count is copied on the GPU into {count, 1, 1}
// of scratch memory, outside the render pass (the game's culling writes the records earlier in the frame). first is not applied, as for the direct draw
static int mesh_records(const p_draw *dw, vka_span *s) {
    const uint32_t n = dw->mdi_max > 0 ? (uint32_t)dw->mdi_max : 1, st = dw->mdi_stride > 0 ? (uint32_t)dw->mdi_stride : 8;
    VkBuffer b; VkDeviceSize o;
    if (!vka_addr(dw->ind, (uint64_t)(n - 1) * st + 4, &b, &o, NULL) || !vka_scratch(12 * (VkDeviceSize)n, 16, s)) return 0;
    VkBufferCopy *c = malloc(n * sizeof *c);
    if (!c) return 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t w[3] = {0, 1, 1}; memcpy(s->ptr + 12 * (size_t)i, w, 12);
        c[i] = (VkBufferCopy){o + (VkDeviceSize)i * st, s->off + 12 * (VkDeviceSize)i, 4};
    }
    { extern const volatile uint32_t *vka_mrec_last; vka_mrec_last = (const volatile uint32_t *)s->ptr; }   // (the crumbs: the count the draw read, at a device loss)
    vka_rp_end(); vka_barrier(g_cb);
    vk.vkCmdCopyBuffer(g_cb, b, s->buf, n, c);
    vka_barrier(g_cb);
    free(c);
    return 1;
}
// ---------- Fritchii: the mesh emulation ----------
// A mesh draw without a task program, on a device where native mesh draws lose the device (DM_VK_MESH_FB, default 1): the mesh program's compute shader writes each
// workgroup's block into the submission slot's output buffer (device-local, MESH_FB_MB a slot, given back with the slot's fence), then the draw goes on as a draw of
// group_vertices vertices a workgroup with the emulation's vertex shader (nvn_recompile.c vka_rc_mesh_fb). An indirect draw's count is the GPU's: a one-invocation pass
// (vulkan_nvn_direct_mesh_args.comp) turns its record into the dispatch's and the draw's arguments. A draw that does not fit the room left is drawn natively
#include "video_core/host_shaders/vulkan_nvn_direct_mesh_args_comp_spv.h"
enum { MESH_FB_MB = 1024, MESH_FB_MAX_IND = 2048, MESH_FB_SEGS = 8 };   // MESH_FB_MAX_IND: the workgroups an indirect draw gets room for (its count is clamped there)
// One device-local ring every submission slot shares (MESH_FB_MB): a slot's submission keeps the ranges it took until its fence; a range wanted again is waited for first
static struct { VkBuffer buf; VkDeviceMemory mem; VkDeviceSize head; int failed; } g_mr;
static struct { VkDeviceSize lo[MESH_FB_SEGS], hi[MESH_FB_SEGS]; int n; } g_mseg[NSLOT];
static uint64_t g_fb_draws, g_fb_native;
static struct { VkDescriptorSetLayout dsl; VkPipelineLayout layout; VkPipeline pipe; int failed; } g_margs;
static struct { const vka_mesh_fb *fb; uint32_t groups; VkDeviceSize args; } g_fbd;   // the draw in progress (vka_draw)
static VkDeviceSize g_mo_peak, g_mo_req_max, g_mo_full, g_mo_sub, g_mo_waits; static unsigned g_mo_subs;
uint64_t vka_slot_fence(unsigned slot);   // vka_exec.c: the fence of the slot's last submission (0: the one being recorded)
void vka_mesh_fb_sub_begin(unsigned slot) {
    if (g_mo_sub > g_mo_peak) g_mo_peak = g_mo_sub;
    g_mo_sub = 0; g_mseg[slot % NSLOT].n = 0;   // (its last submission is done: vka_exec.c waits for its fence before reusing the slot)
    if (++g_mo_subs % 600 == 0) { VLOG("mesh emulation: %llu draws emulated, %llu native; output peak %.1f MiB a submission, largest request %.1f MiB, %llu requests that did not fit, %llu waits for the ring",
                                       (unsigned long long)g_fb_draws, (unsigned long long)g_fb_native, g_mo_peak / 1048576.0, g_mo_req_max / 1048576.0, (unsigned long long)g_mo_full, (unsigned long long)g_mo_waits); g_mo_peak = 0; }
}
static int mesh_fb_room(VkDeviceSize n, VkDeviceSize align, VkDeviceSize *off) {
    const VkDeviceSize cap = (VkDeviceSize)MESH_FB_MB << 20;
    if (!g_mr.buf && !g_mr.failed) {
        g_mr.failed = 1;
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VkMemoryRequirements mr = {0}; uint32_t t = UINT32_MAX;
        if (!vk.vkCreateBuffer(V.dev, &bi, NULL, &g_mr.buf)) { vk.vkGetBufferMemoryRequirements(V.dev, g_mr.buf, &mr); t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0); }
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = t == UINT32_MAX ? 0 : mr.size, .memoryTypeIndex = t};
        if (!g_mr.buf || t == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &g_mr.mem) || vk.vkBindBufferMemory(V.dev, g_mr.buf, g_mr.mem, 0)) {
            VRED("mesh emulation output ring (%d MiB) not made: mesh draws are native", MESH_FB_MB);
            if (g_mr.buf) vk.vkDestroyBuffer(V.dev, g_mr.buf, NULL);
            if (g_mr.mem) vk.vkFreeMemory(V.dev, g_mr.mem, NULL);
            g_mr.buf = VK_NULL_HANDLE; g_mr.mem = VK_NULL_HANDLE; return 0; }
        vka_mem_grew(mr.size); g_mr.failed = 0;
    }
    if (!g_mr.buf) return 0;
    if (n > g_mo_req_max) g_mo_req_max = n;
    if (n > cap / 4) { g_mo_full++; return 0; }
    VkDeviceSize at = (g_mr.head + align - 1) / align * align;
    if (at + n > cap) at = 0;   // (wraps)
    typeof(g_mseg[0]) *me = &g_mseg[g_slot];
    for (int k = 0; k < me->n; k++) if (at < me->hi[k] && me->lo[k] < at + n) { g_mo_full++; return 0; }   // this submission would overwrite itself
    if (me->n == MESH_FB_SEGS && !(me->n && me->hi[me->n - 1] == at)) { g_mo_full++; return 0; }
    for (unsigned s = 0; s < NSLOT; s++) {   // earlier submissions still on the GPU over that range: waited for
        if (s == g_slot || !g_mseg[s].n) continue;
        int hit = 0; for (int k = 0; k < g_mseg[s].n; k++) hit |= at < g_mseg[s].hi[k] && g_mseg[s].lo[k] < at + n;
        if (!hit) continue;
        const uint64_t f = vka_slot_fence(s);
        if (f && vka_done() < f) { g_mo_waits++; if (!vka_wait_done(f, 2000000000ull)) { g_mo_full++; return 0; } }
        g_mseg[s].n = 0;
    }
    if (me->n && me->hi[me->n - 1] == at) me->hi[me->n - 1] = at + n;
    else { me->lo[me->n] = at; me->hi[me->n] = at + n; me->n++; }
    g_mr.head = at + n; g_mo_sub += n;
    *off = at; return 1;
}
static int mesh_args_pipeline(void) {
    if (g_margs.pipe || g_margs.failed) return g_margs.pipe != VK_NULL_HANDLE;
    g_margs.failed = 1;
    const VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = V.push_desc ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0, .bindingCount = 2, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof VULKAN_NVN_DIRECT_MESH_ARGS_COMP_SPV, .pCode = VULKAN_NVN_DIRECT_MESH_ARGS_COMP_SPV};
    VkShaderModule m = VK_NULL_HANDLE; VkResult r = vk.vkCreateDescriptorSetLayout(V.dev, &li, NULL, &g_margs.dsl);
    VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_margs.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (!r) r = vk.vkCreatePipelineLayout(V.dev, &pi, NULL, &g_margs.layout);
    if (!r) r = vk.vkCreateShaderModule(V.dev, &mi, NULL, &m);
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}, .layout = g_margs.layout};
    if (!r) r = vk.vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, &g_margs.pipe);
    if (m) vk.vkDestroyShaderModule(V.dev, m, NULL);
    if (r) { VRED("mesh emulation arguments pipeline: %d; indirect mesh draws are native", r); g_margs.pipe = VK_NULL_HANDLE; return 0; }
    g_margs.failed = 0; return 1;
}
static void walk_all(const vka_dstate *d, vka_shader *const sh[VKA_NSTAGE]);
static int bind_sets(const vka_dstate *d, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, VkPipelineBindPoint bp, c0_at *c0);
// The emulation's compute pass of mesh draw dw (no pass open after): 1 with g_fbd and g_mesh_out set for the draw; 0: draw it natively
static const char *g_fb_why;
static int mesh_fb_compute(const vka_dstate *d, const vka_mesh_fb *fb, const p_draw *dw) {
    g_fb_why = "";
    const VkDeviceSize bw = (VkDeviceSize)fb->block_words * 4, room = ((VkDeviceSize)MESH_FB_MB << 20) / 4;
    uint32_t maxg = (uint32_t)((room / 4) / bw); if (maxg > V.mesh_groups[0]) maxg = V.mesh_groups[0];
    if (dw->ind && maxg > MESH_FB_MAX_IND) maxg = MESH_FB_MAX_IND;
    uint32_t g = 0; VkBuffer rb = VK_NULL_HANDLE; VkDeviceSize ro = 0, rbase = 0;
    if (!dw->ind) { g = (uint32_t)dw->count; if (!g || g > maxg) { g_fb_why = "count past the room"; return 0; } }
    else {
        if (!mesh_args_pipeline() || !vka_addr(dw->ind, 4, &rb, &ro, NULL) || (ro & 3)) { g_fb_why = "record not placed"; return 0; }
        rbase = ro / V.sb_align * V.sb_align;
    }
    vka_shader *c5[VKA_NSTAGE] = {0}; c5[5] = fb->cs;
    vka_layout lay; if (!vka_layout_of(c5, 1, &lay)) { g_fb_why = "no layout"; return 0; }
    VkPipeline p = vka_cs_pipeline(fb->cs, &lay); if (!p) { g_fb_why = "no compute pipeline (yet)"; return 0; }
    VkDeviceSize out = 0, args = 0, outn = dw->ind ? (VkDeviceSize)maxg * bw : (VkDeviceSize)g * bw;
    if (!mesh_fb_room(outn, V.sb_align, &out)) { g_fb_why = "output buffer full"; return 0; }
    if (dw->ind && !mesh_fb_room(32, V.sb_align, &args)) { g_fb_why = "output buffer full"; return 0; }
    { extern uint32_t g_setrt_n; const uint32_t o = R.on ? R.setrt : g_setrt_n & 1023; g_pre_seen[o >> 6] |= 1ull << (o & 63); }
    VkCommandBuffer vka_pre_cb(void); VkCommandBuffer pre = R.on ? vka_pre_cb() : VK_NULL_HANDLE, part = g_cb;
    if (pre) g_cb = pre;   // (the pass stays open in the part: these run before it)
    else vka_rp_end();
    VkBuffer ob = g_mr.buf;
    if (dw->ind) {   // the record's count, as the dispatch's and the draw's arguments
        VkDescriptorBufferInfo bi[2] = {{rb, rbase, ro - rbase + 4}, {ob, args, 32}};
        VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[0]},
                                     {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[1]}};
        vk.vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_margs.pipe);
        if (V.push_desc) vk.vkCmdPushDescriptorSetKHR(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_margs.layout, 0, 2, w);
        else { VkDescriptorSet st = set_alloc(g_margs.dsl); if (!st) { g_cb = part; return 0; }
               w[0].dstSet = w[1].dstSet = st; vk.vkUpdateDescriptorSets(V.dev, 2, w, 0, NULL); vk.vkCmdBindDescriptorSets(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_margs.layout, 0, 1, &st, 0, NULL); }
        const uint32_t pc[3] = {(uint32_t)((ro - rbase) / 4), fb->group_vertices, maxg};
        vk.vkCmdPushConstants(g_cb, g_margs.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        vk.vkCmdDispatch(g_cb, 1, 1, 1);
        vka_barrier(g_cb);
    }
    g_mesh_out = (VkDescriptorBufferInfo){ob, out, outn};
    vka_shader *w7[VKA_NSTAGE] = {0}; w7[7] = fb->cs;   // (walked as the mesh stage: its constant buffers, textures and storage buffers)
    walk_all(d, w7);
    c0_at c0 = {0};
    if (!bind_sets(d, w7, &lay, VK_PIPELINE_BIND_POINT_COMPUTE, &c0)) { g_cb = part; vka_dyn_invalidate(); g_fb_why = "descriptors not bound"; return 0; }
    vk.vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    if (dw->ind) vk.vkCmdDispatchIndirect(g_cb, ob, args); else vk.vkCmdDispatch(g_cb, g, 1, 1);
    if (!pre) vka_barrier(g_cb);   // (a pre command buffer ends with one: vka_exec.c pre_submit)
    g_cb = part;
    vka_dyn_invalidate();   // (the compute pipeline's push constants and descriptors disturb the draws' ones)
    g_fbd.fb = fb; g_fbd.groups = g; g_fbd.args = args;
    return 1;
}
void vka_draw(vka_dstate *d, const p_set_rt *rt, int has_rt, const p_scissor *sc, int has_sc, const p_draw *dw, const p_draw_idx *di) {
    if (!g_cb || !draws_on() || !statics()) { g_skipped++, vka_skip_line[__LINE__ & 2047]++; return; }
    if (!d->inited) vka_dstate_reset(d);
    // An NVN2 mesh draw (hle_nvn.c): a multi-draw of them is one record, drawn whole below
    int mesh = dw && (dw->prim == NVN_PRIM_MESH || dw->prim == NVN_PRIM_MESH_3D);
    g_fbd.fb = NULL;
    uint64_t mdi = mesh ? 0 : dw ? dw->mdi_cnt : di->mdi_cnt; int mdi_i = dw ? dw->mdi_i : di->mdi_i;
    // An indirect-count multi-draw (hle_nvn.c records one draw per possible record): on a device with drawIndirectCount the first record draws them all with the count the
    // GPU reads (vkCmdDraw*IndirectCount, at most mdi_max), and the other records are skipped; else each record reads the count on the CPU at replay
    int mdi_gpu = mdi && V.draw_indirect_count && (dw ? dw->mdi_max : di->mdi_max) > 0;
    if (mdi_gpu && mdi_i > 0) return;
    if (mdi && !mdi_gpu) { uint32_t cnt = 0; if (!cpu_read(mdi, &cnt, 4) || (uint32_t)mdi_i >= cnt) return;   // past the multi-draw's count (read now: a count the GPU writes in this submission is not seen)
               VRED_ONCE("indirect-count multi-draws read their count on the CPU at replay"); }
    d->b.base_instance = mesh ? 0 : (uint32_t)(dw ? dw->base_instance : di->base_instance);   // (a 3D mesh draw's z: hle_nvn.c)
    vka_shader *sh[VKA_NSTAGE] = {0}; unsigned bad = 0;
    vka_rc_draw(d);   // Lichii: the state shaders recompiled at this draw read (nvn_recompile.c)
    PT(0);
    // An NVN2 mesh draw (nvnCommandBufferDrawMeshTasks and its indirect, 3D and multi-draw forms, hle_nvn.c): the task (optional) and mesh shaders of one program, the
    // fragment stage's shader, in a mesh pipeline (vka_pso.c pstate_fill) drawn with vkCmdDrawMeshTasks*EXT: no vertex input, primitive type or index buffer
    if (mesh) {
        if (!V.mesh) SKIP("mesh draw (nvnCommandBufferDrawMeshTasks) on a device without VK_EXT_mesh_shader (or DM_VK_MESH=0): skipped");
        // The program whose task and mesh shaders run: the fragment stage's when it has a mesh shader (a program's stages are bound together), else the one bound at the
        // mesh stage bit. Task and mesh come from the same program (a program's task shader feeds its own mesh shader)
        const prog_t *mp = d->bst[7];
        { vka_shader *m = NULL; unsigned b2 = 0; if (d->bst[1] && d->bst[1] != mp && resolve(d->bst[1], 7, &m, &b2) && (m || (b2 & 1u << 7))) mp = d->bst[1]; }
        if (!resolve(mp, 6, &sh[6], &bad) || !resolve(mp, 7, &sh[7], &bad) || !resolve(d->bst[1], 1, &sh[1], &bad)) SKIP("mesh draw skipped: a program without shaders");
        if (bad) { g_skipped++, vka_skip_line[__LINE__ & 2047]++; return; }   // a refused or missing shader (logged when it was made)
        if (!sh[7]) SKIP("mesh draw with no mesh shader bound: skipped");
        if (d->bst[6] && d->bst[6] != mp) VRED_ONCE("mesh draw: a program bound at the task stage bit apart from the mesh program; drawn with the mesh program's own task shader, if any");
        // A pixel shader with per-primitive inputs reads what its mesh shader writes per primitive (Vulkan matches only equally decorated interfaces). The other way round
        // (a mesh shader with per-primitive outputs, a pixel shader without the decoration: vk_android_build.py leaves a pixel shader also drawn with vertex shaders per
        // vertex) the flags cannot tell whether it reads those locations: a red line
        if (sh[1] && (sh[1]->e->flags & DMSH_VK_F_MESH_PRIM) && !(sh[7]->e->flags & DMSH_VK_F_MESH_PRIM))
            SKIP("mesh draw: pixel shader %016llx reads per-primitive inputs that mesh shader %016llx does not write: skipped", (unsigned long long)sh[1]->key, (unsigned long long)sh[7]->key);
        if (sh[1] && !(sh[1]->e->flags & DMSH_VK_F_MESH_PRIM) && (sh[7]->e->flags & DMSH_VK_F_MESH_PRIM))
            VRED_ONCE("mesh draw: mesh shader %016llx writes per-primitive outputs and pixel shader %016llx has no per-primitive inputs: any it reads there are undefined", (unsigned long long)sh[7]->key, (unsigned long long)sh[1]->key);
        if (dw->prim == NVN_PRIM_MESH && !dw->ind && dw->first) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 4) VRED("mesh draw with first workgroup %d (count %d): drawn from 0 (the translated task / mesh shaders read their workgroup index with no base; open)", dw->first, dw->count); }
        if (!dw->ind && (dw->count <= 0 || (dw->prim == NVN_PRIM_MESH_3D && (dw->instances <= 0 || dw->base_instance <= 0)))) return;
        if (dw->ind && (!vk.vkCmdDrawMeshTasksIndirectEXT || !vk.vkCmdDrawMeshTasksIndirectCountEXT)) SKIP("indirect mesh draw on a device without vkCmdDrawMeshTasksIndirect(Count)EXT: skipped");
        if (d->tf_on) VRED_ONCE("transform feedback with a mesh draw: drawn without capture");
        { static int st = -1, sp = -1; if (st < 0) { st = (int)VKA_ENV("DM_VK_SKIP_TASK", 0); sp = (int)VKA_ENV("DM_VK_SKIP_MESHPRIM", 0); }   // diagnostics: mesh draws with a task program / per-primitive outputs skipped
          if ((st && sh[6]) || (sp && ((sh[7]->e->flags & DMSH_VK_F_MESH_PRIM) || (sh[1] && (sh[1]->e->flags & DMSH_VK_F_MESH_PRIM))))) { g_skipped++; return; } }   // DM_VK_SKIP_TASK=1: mesh draws with a task program skipped (diagnostic)
        static int fb_on = -1; if (fb_on < 0) fb_on = (int)VKA_ENV("DM_VK_MESH_FB", 1);   // Fritchii: the mesh emulation (above vka_draw)
        if (fb_on) {
            const char *no = sh[6] ? "a task program" : (sh[7]->e->flags & DMSH_VK_F_MESH_PRIM) ? "per-primitive outputs" : (sh[1] && (sh[1]->e->flags & DMSH_VK_F_MESH_PRIM)) ? "a pixel shader with per-primitive inputs"
                           : dw->prim != NVN_PRIM_MESH ? "a 3D mesh draw" : dw->ind && (dw->mdi_cnt || dw->mdi_max > 1) ? "a multi-draw" : NULL;
            if (!no) {   // DM_VK_MESH_FB_SKIP=<keys>: those mesh programs drawn natively (to find one the emulation draws wrong)
                static const char *skip; static int si; if (!si) { skip = getenv("DM_VK_MESH_FB_SKIP"); si = 1; }
                char kx[17]; snprintf(kx, sizeof kx, "%016llx", (unsigned long long)sh[7]->key);
                if (skip && strstr(skip, kx)) no = "DM_VK_MESH_FB_SKIP";
                static uint64_t em[256]; static int ne; int k3 = 0; for (; k3 < ne && em[k3] != sh[7]->key; k3++) {}
                if (k3 == ne && ne < 256) { em[ne++] = sh[7]->key; VLOG("mesh emulation: program %016llx (pixel %016llx)", (unsigned long long)sh[7]->key, sh[1] ? (unsigned long long)sh[1]->key : 0ull); }
            }
            const vka_mesh_fb *fb = no ? NULL : vka_prog_mesh_fb(mp);
            if (!no && !fb) no = "no emulation made";
            if (!no && !mesh_fb_compute(d, fb, dw)) no = g_fb_why;
            if (!no) { sh[0] = fb->vs; sh[6] = sh[7] = NULL; mesh = 0; g_fb_draws++; }
            else { g_fb_native++; static uint64_t seen[64]; static int ns; int k2 = 0; for (; k2 < ns && seen[k2] != sh[7]->key; k2++) {}
                   if (k2 == ns && ns < 64) { seen[ns++] = sh[7]->key; VRED("mesh draw of %016llx drawn natively: %s", (unsigned long long)sh[7]->key, no); } }
        }
    } else {
        for (int st = 0; st < 5; st++) if (!resolve(d->bst[st], st, &sh[st], &bad)) SKIP("draw skipped: a program without shaders");
        if (bad) { g_skipped++, vka_skip_line[__LINE__ & 2047]++; return; }   // a refused or missing shader (logged when it was made)
        if (!sh[0]) SKIP("draw with no vertex shader bound: skipped");
        if (sh[1] && (sh[1]->e->flags & DMSH_VK_F_MESH_PRIM)) SKIP("pixel shader %016llx reads per-primitive mesh outputs and is drawn with a vertex shader: skipped", (unsigned long long)sh[1]->key);
        if (d->rast_discard && d->tf_on) SKIP("transform feedback is not on Vulkan yet (F_TF): the draw is skipped");
        if (d->tf_on) VRED_ONCE("transform feedback is not on Vulkan yet: the draw is drawn without capture");
    }
    if (d->poly.cull == NVN_FACE_FRONT_AND_BACK && !d->rast_discard) return;
    int prim = dw ? dw->prim : di->prim, expand = 0;
    VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // (a mesh pipeline's: every translated mesh shader outputs triangles)
    if (g_fbd.fb) topo = g_fbd.fb->topology == 2 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : g_fbd.fb->topology == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    else if (!mesh) {
        topo = topology(prim, &expand);
        if (expand < 0) SKIP("primitive type %d not on Vulkan: the draw is skipped", prim);
        if (topo == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) {   // (Lichii: the patch size is the pipeline's)
            if (!sh[4]) SKIP("patches drawn with no tessellation evaluation shader: skipped");
            if (!sh[3]) SKIP("patches drawn with tessellation evaluation shader %016llx and no control shader: NVN's default levels (outer %g %g %g %g, inner %g %g) need a control stage on Vulkan; skipped",
                             (unsigned long long)sh[4]->key, d->tess_outer[0], d->tess_outer[1], d->tess_outer[2], d->tess_outer[3], d->tess_inner[0], d->tess_inner[1]);
            if (d->patch < 1 || d->patch > (int)V.props.limits.maxTessellationPatchSize) SKIP("patch size %d: beyond the device's %u, skipped", d->patch, V.props.limits.maxTessellationPatchSize);
        }
    }
    uint64_t ind = dw ? dw->ind : di->ind;
    // A geometry shader that writes memory makes those writes in the run that claims its primitive (vka_spv_gs_claim): a zeroed word per primitive at VKA_GS_CLAIM_BINDING.
    // Primitive IDs restart with each instance and an indirect draw's count is the GPU's, so those draws take the shader as it is (each run writing)
    g_claim_words = 0;
    if (sh[2] && sh[2]->gs_fx > 0) {
        int inst_n = dw ? dw->instances : di->instances;
        if (!ind && inst_n <= 1) { int32_t c = dw ? dw->count : di->count; g_claim_words = c > 0 ? (uint32_t)c : 1; }
        else VRED_ONCE("geometry shader %016llx writes memory in an %s draw: drawn without the claim, its writes repeated by the device's runs of each primitive", (unsigned long long)sh[2]->key, ind ? "indirect" : "instanced");
    }
    if (expand && ind) SKIP("an indirect draw of quads or a line loop is not on Vulkan yet: skipped");

    PT(1);
    // The last draw's shaders' layout (layouts live as long as the device): the layout table's lock and hash a draw
    static vka_shader *lsh[VKA_NSTAGE]; static vka_layout llay; static int lok;
    vka_layout lay;
    if (lok && !memcmp(lsh, sh, sizeof lsh)) lay = llay;
    else { lok = 0; if (!vka_layout_of(sh, 0, &lay)) SKIP("draw skipped: no pipeline layout"); memcpy(lsh, sh, sizeof lsh); llay = lay; lok = 1; }
    // Images first used by this draw are made (cleared or uploaded) outside the render pass, then the pass opens over the bound targets
    PT(2);
    walk_all(d, sh);
    if (make_images(d, sh, 1)) { vka_rp_end(); make_images(d, sh, 0); }
    if (R.on && R.bar_pending && bar_hazard(sh, d)) { g_bar_late++; vka_rp_end(); }   // the game barrier taken inside the pass is needed here: the pass ends (its full barrier) first
    if (vka_nan_on() >= 2 && sh[1] && !d->rast_discard && R.area.extent.width >= (uint32_t)VKA_ENV("FRITCHII_NANSCAN_MIN_W", 0) && vka_nan_probe(sh[1]->key)) {   // DM_VK_NANSCAN_FS: this draw's sampled textures, scanned before it (Fritchii: FRITCHII_NANSCAN_MIN_W: only passes at least that wide)
        char why[8]; int n = vka_walk(&d->b, 1, sh[1]->e, sh[1]->b, sh[1]->lib->h->ssbo_align, 0, rd_cb, NULL, g_res, 512, NULL, why, sizeof why, sh[1]->hprog, sh[1]->hprog_words), m = 0;
        static vka_img *ims[64]; static int bs[64], ts[64];
        for (int i = 0; i < n && m < 64; i++) if (g_res[i].what == VKA_R_TEX && g_res[i].tid) { ims[m] = vka_tex_id_img(g_res[i].tid, g_cb); bs[m] = g_res[i].binding; ts[m++] = g_res[i].tid;
            static _Atomic int nd; if (atomic_fetch_add(&nd, 1) < 32) { char why[320]; vka_tex_id_desc(g_res[i].tid, why, sizeof why); vka_img *im = ims[m - 1];
                if (getenv("DM_VK_NANSCAN_DUMP") && im && im->d.format >= 0x79 && im->d.format <= 0x94) vka_tex_id_dump(g_res[i].tid);
                vka_tex_id_check(g_res[i].tid);
                VLOG("NaN scan: input binding %u dim %#x texture ID %d sampler %d: %s; image inited %d gpu_owned %d track %d", g_res[i].binding, g_res[i].dim, g_res[i].tid, g_res[i].sid, why, im ? im->inited : -1, im ? im->gpu_owned : -1, im ? im->track : -1); } }
        int on = R.on; vka_rp_end(); (void)on;
        vka_nan_inputs(g_cb, ims, bs, ts, m);
    }
    vka_span mrec = {0};
    // DM_VK_MESH_CPU=1 (test only): {count, first} mesh records read on the CPU at replay and drawn directly, without mesh_records' pass break. Ocarina of Time's
    // foliage records are written by the GPU (its culling): read this way most of its foliage disappeared
    static int mesh_cpu = -1; if (mesh_cpu < 0) mesh_cpu = (int)VKA_ENV("DM_VK_MESH_CPU", 0);
    const int mcpu = mesh_cpu && mesh && ind && dw->prim == NVN_PRIM_MESH && !dw->mdi_cnt;
    if (!mcpu && mesh && ind && dw->prim == NVN_PRIM_MESH && !mesh_records(dw, &mrec))
        SKIP("indirect mesh draw: records at %#llx (%d, stride %d) not in one placed segment: skipped", (unsigned long long)ind, dw->mdi_max, dw->mdi_stride);
    if (!rp_begin(rt, has_rt, d)) { g_skipped++, vka_skip_line[__LINE__ & 2047]++; return; }
    if (!R.swrite && draw_writes(sh, d)) R.swrite = 1;   // (a game barrier in this pass from now on ends it: vka_rp_game_barrier)
    d->b.samples = R.samples;
    vka_gkey k; memset(&k, 0, V.vin_dyn ? offsetof(vka_gkey, va) : sizeof k);   // (va, vb: only without vertex input dynamic state, vin_build; the pipeline lookup reads no further then)
    k.ncolor = (uint8_t)R.ncolor; k.samples = (uint8_t)R.samples; k.dfmt = R.df; k.sfmt = R.sf;
    k.topo = mesh ? 0 : (uint8_t)topo_class(topo);
    k.poly_mode = (uint8_t)(d->poly.mode == NVN_POLYGON_MODE_LINE && V.fill_lines ? VK_POLYGON_MODE_LINE : d->poly.mode == NVN_POLYGON_MODE_POINT && V.fill_lines ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL);
    k.depth_clamp = d->depth_clamp && V.depth_clamp; k.a2c = d->ms.a2c != 0;
    // Lichii: the sample mask, the patch size, conservative rasterization, the provoking vertex, viewport swizzles, the exclusive scissor, VRS
    k.sample_mask = (uint16_t)(d->sample_mask & ((1u << k.samples) - 1));
    if (!mesh && (sh[3] || sh[4])) k.patch = (uint8_t)d->patch;
    if (d->cons) {
        if (!V.cons) VRED_ONCE("conservative rasterization and no VK_EXT_conservative_rasterization: drawn without it");
        else { k.cons = 1; k.dilate = (uint8_t)(d->dilate * 4.0f + 0.5f); }
        if (d->cons_mode) VRED_ONCE("conservative rasterization mode %d (snapping before rasterization) has no Vulkan equivalent: drawn as mode 0", d->cons_mode);
        if (d->subpixel_bias[0] || d->subpixel_bias[1]) VRED_ONCE("subpixel precision bias %d,%d has no Vulkan equivalent: drawn without it", d->subpixel_bias[0], d->subpixel_bias[1]);
    }
    if (d->provoking == 1) { if (V.provoking_last) k.provoking_last = 1; else VRED_ONCE("the last vertex provokes and the device cannot (VK_EXT_provoking_vertex provokingVertexLast): the first does"); }
    uint32_t vsw; memcpy(&vsw, d->vp_sw[0], 4);   // (stored XOR the identity: 0 unchanged; 0x100 alone, y negated, is a viewport flip below)
    if (vsw && vsw != 0x100) { if (V.vp_swizzle) memcpy(k.vp_sw, d->vp_sw[0], 4);
                               else VRED_ONCE("viewport swizzle %d,%d,%d,%d and no VK_NV_viewport_swizzle: drawn unswizzled", d->vp_sw[0][0], d->vp_sw[0][1] ^ 2, d->vp_sw[0][2] ^ 4, d->vp_sw[0][3] ^ 6); }
    const int excl = d->excl_on && d->excl_n > 0 && d->excl[0][2] > 0 && d->excl[0][3] > 0;
    if (excl) { if (V.excl_scissor) k.excl = 1; else VRED_ONCE("an exclusive scissor and no VK_NV_scissor_exclusive: drawn without it"); }
    k.vrs = R.vrs > 0;
    if (d->color.logic_op != NVN_LOGIC_OP_COPY && V.logic_op) { k.logic_en = 1; k.logic_op = (uint8_t)d->color.logic_op; }
    k.gl_depth = !vka_depth_zero();
    const vka_shader *fs = d->rast_discard ? NULL : sh[1];
    // The pixel shader's module (android/SHADERS.md "Variants"): MOD_UINT when every target it writes is a UINT attachment, MOD_UINT_MIXED when the UINT ones are exactly its
    // uint_mask, MOD_DUAL when target 0 blends with a SRC1 factor; else the base. The alpha test is the base's specialization constant in every variant
    uint32_t umask = 0, amask = 0;
    for (uint32_t i = 0; i < R.ncolor; i++) if (R.cf[i]) { amask |= 1u << i; if (R.ckind[i] == 1) umask |= 1u << i; }
    int variant = DMSH_VK_MOD_BASE;
    if (fs) {
        uint32_t fm = fs->e->fout_mask;
        if ((umask & fm) && (umask & fm) == (amask & fm) && vka_shlib_module(fs->lib, fs->e, DMSH_VK_MOD_UINT)) variant = DMSH_VK_MOD_UINT;
        else if ((umask & fm) && fs->e->uint_mask == (umask & fm) && vka_shlib_module(fs->lib, fs->e, DMSH_VK_MOD_UINT_MIXED)) variant = DMSH_VK_MOD_UINT_MIXED;
        else if ((d->color.blend_en & 1) && V.dual_src && vka_shlib_module(fs->lib, fs->e, DMSH_VK_MOD_DUAL)) {
            blend_st b0 = adv_blend(d->blend[0]);
            if (is_src1(b0.src_rgb) || is_src1(b0.dst_rgb) || is_src1(b0.src_a) || is_src1(b0.dst_a)) variant = DMSH_VK_MOD_DUAL;
        }
        if ((fs->e->flags & DMSH_VK_F_ALPHA_TEST) && d->color.alpha_func != NVN_ALPHA_FUNC_ALWAYS) k.alpha_func = (uint8_t)d->color.alpha_func;
    }
    // No library variant for a float output into an integer attachment (the library has MOD_UINT only for outputs made of integer bits or constants): the base module
    // with those outputs retyped as the integer vectors of their bits (VKA_FS_RETYPE, vka_spv_fout_retype), which is what the hardware writes. DM_VK_FOUT_RETYPE=0: not written, as before
    // Only a module that was rewritten is used (one the rewrite refuses falls back to the base module, whose float outputs those targets then do not take: written = 0 below).
    // MOD_UINT_MIXED / MOD_DUAL pipelines keep their variant; a float output there into another integer target stays unwritten (open)
    if (fs && variant == DMSH_VK_MOD_BASE && VKA_ENV("DM_VK_FOUT_RETYPE", 1)) {
        uint32_t fu = 0, fsm = 0;
        for (uint32_t i = 0; i < R.ncolor; i++) if (R.cf[i] && (R.ckind[i] == 1 || R.ckind[i] == 2) && (fs->e->fout_mask >> i & 1) && (fs->e->fout_types >> (2 * i) & 3) == DMSH_VK_TYPE_FLOAT) { if (R.ckind[i] == 1) fu |= 1u << i; else fsm |= 1u << i; }
        if ((fu | fsm) && vka_shader_fout_retyped((vka_shader *)fs, fu, fsm)) variant = VKA_FS_RETYPE;
    }
    k.fs_variant = (uint8_t)variant; k.gs_fx = g_claim_words != 0;
    int src1_used = 0;
    for (uint32_t i = 0; i < R.ncolor; i++) {
        k.cfmt[i] = R.cf[i];
        if (!R.cf[i]) continue;
        uint32_t m = d->chmask.mask >> (4 * i) & 0xf;
        int written = fs && (fs->e->fout_mask >> i & 1);
        int out_cls = !fs ? 0 : variant == DMSH_VK_MOD_UINT || (variant == DMSH_VK_MOD_UINT_MIXED && (fs->e->uint_mask >> i & 1)) ? DMSH_VK_TYPE_UINT : (int)(fs->e->fout_types >> (2 * i) & 3);
        int att_cls = R.ckind[i] == 1 ? DMSH_VK_TYPE_UINT : R.ckind[i] == 2 ? DMSH_VK_TYPE_SINT : DMSH_VK_TYPE_FLOAT;
        if (variant == VKA_FS_RETYPE && out_cls == DMSH_VK_TYPE_FLOAT && att_cls != DMSH_VK_TYPE_FLOAT) out_cls = att_cls;   // (retyped in the module: vka_pso.c fout_masks)
        if (written && out_cls != att_cls) {   // Vulkan leaves a class mismatch undefined: the target is not written (SINT attachments have no variant yet)
            static _Atomic int n; if (atomic_fetch_add(&n, 1) < 4) VRED("fragment shader %016llx writes a %s output to a %s target %u: that target is not written", (unsigned long long)fs->key,
                                                                       out_cls == 0 ? "float" : out_cls == 1 ? "sint" : "uint", att_cls == 0 ? "float" : att_cls == 1 ? "sint" : "uint", i);
            written = 0;
        }
        k.att[i].mask = written ? (uint8_t)m : 0;   // outputs the shader does not write leave the target as it was (Maxwell writes only the shader's outputs)
        if (written && (d->color.blend_en >> i & 1) && att_cls == DMSH_VK_TYPE_FLOAT) {
            blend_st b = adv_blend(d->blend[i]);
            if (variant != DMSH_VK_MOD_DUAL && (is_src1(b.src_rgb) || is_src1(b.dst_rgb) || is_src1(b.src_a) || is_src1(b.dst_a))) {   // no second source: it reads as zero
                src1_used = 1; b.src_rgb = zero_src1(b.src_rgb); b.dst_rgb = zero_src1(b.dst_rgb); b.src_a = zero_src1(b.src_a); b.dst_a = zero_src1(b.dst_a); }
            k.att[i] = (typeof(k.att[i])){1, (uint8_t)bfac(b.src_rgb), (uint8_t)bfac(b.dst_rgb), (uint8_t)bop(b.eq_rgb), (uint8_t)bfac(b.src_a), (uint8_t)bfac(b.dst_a), (uint8_t)bop(b.eq_a), (uint8_t)m};
        }
    }
    if (src1_used) VRED_ONCE("a SRC1 blend factor with no dual-source module (MOD_DUAL; the profile's dual_want.tsv) or no dualSrcBlend: the second source reads as zero");
    {   // Fritchii, DM_VK_BLEND_TRACE_FS=<key>: that shader's first draws' blend state (the game's and the Vulkan factors made of it)
        static uint64_t bk = ~0ull; if (bk == ~0ull) { const char *e = getenv("DM_VK_BLEND_TRACE_FS"); bk = e ? strtoull(e, NULL, 16) : 0; }
        static _Atomic int nb;
        if (bk && fs && fs->key == bk && atomic_fetch_add(&nb, 1) < 4) {
            const blend_st b = d->blend[0];
            VLOG("blend trace %016llx: enable %#x variant %d src1_used %d; NVN target 0 src_rgb %d dst_rgb %d eq_rgb %d src_a %d dst_a %d eq_a %d adv %d; Vulkan att0 en %d src %d dst %d op %d srca %d dsta %d opa %d mask %#x; blend colour %g %g %g %g",
                 (unsigned long long)bk, d->color.blend_en, variant, src1_used, (int)b.src_rgb, (int)b.dst_rgb, (int)b.eq_rgb, (int)b.src_a, (int)b.dst_a, (int)b.eq_a, (int)b.adv,
                 k.att[0].en, k.att[0].src, k.att[0].dst, k.att[0].op, k.att[0].srca, k.att[0].dsta, k.att[0].opa, k.att[0].mask, d->blend_color[0], d->blend_color[1], d->blend_color[2], d->blend_color[3]);
        }
    }

    PT(3);
    {   // DM_VK_SKIP_FS=<keys>: draws with those pixel shaders skipped (to find one that hangs the GPU or draws wrong); DM_VK_FS_LIST=1: each pixel shader drawn, logged once
        static const char *skip; static int on = -1, list; if (on < 0) { skip = getenv("DM_VK_SKIP_FS"); list = VKA_ENV("DM_VK_FS_LIST", 0) != 0; on = skip || list; }
        if (on && sh[1]) {
            char kx[17]; snprintf(kx, sizeof kx, "%016llx", (unsigned long long)sh[1]->key);
            if (list) { static uint64_t seen[4096]; static int ns; int i = 0; for (; i < ns && seen[i] != sh[1]->key; i++) {} if (i == ns && ns < 4096) { seen[ns++] = sh[1]->key; VLOG("fs drawn: %s", kx); } }
            if (skip && strstr(skip, kx)) { g_skipped++; return; }
        } }
    if (!mesh) vin_build(d, sh[0], &k);
    PT(4);
    VkPipeline p = vka_gfx_pipeline(sh, &lay, &k, !R.again);
    if (!p) { g_skipped++, vka_skip_line[__LINE__ & 2047]++; return; }
    if (!DY.valid || !g_dyn_filter || DY.pipe != p) { vk.vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); DY.pipe = p; VB.vin_stale = 1; }
    if (vka_nan_on()) vka_nan_draw(mesh ? sh[7]->key : sh[0]->key, fs ? fs->key : 0);
    PT(5);
    if (!bind_sets(d, sh, &lay, VK_PIPELINE_BIND_POINT_GRAPHICS, NULL)) { g_skipped++, vka_skip_line[__LINE__ & 2047]++; return; }
    PT(6);
    if (!mesh) vin_bind(d);

    // Viewport and scissor. The translator writes clip space for Metal (y up, origin upper left): Metal's viewport, flipped for a lower-left window origin as on Metal, then given
    // a negative height, maps it as Metal does (android/SHADERS.md "Depth and Y")
    float vx = d->has_vp ? d->vp[0][0] : 0, vy = d->has_vp ? d->vp[0][1] : 0, vw = d->has_vp ? d->vp[0][2] : (float)R.area.extent.width, vh = d->has_vp ? d->vp[0][3] : (float)R.area.extent.height;
    if (R.scaled && d->has_vp) { float f = (float)vka_scale_up / (float)(1u << vka_scale_shift); vx *= f; vy *= f; vw *= f; vh *= f; }   // (Lichii: the render scale)
    if (vka_origin_ll()) { vy += vh; vh = -vh; }
    if (vsw == 0x100) { vy += vh; vh = -vh; }   // (other swizzles are the pipeline's: k.vp_sw)
    float n0 = d->dr[0] < 0 ? 0 : d->dr[0] > 1 ? 1 : d->dr[0], n1 = d->dr[1] < 0 ? 0 : d->dr[1] > 1 ? 1 : d->dr[1];
    if (!vw) vw = 1; if (!vh) vh = 1;
    VkViewport vp = {vx, vy + vh, vw, -vh, n0, n1};
    DY_SET(vp, vp, vk.vkCmdSetViewport(g_cb, 0, 1, &vp));
    VkRect2D scr = R.area;
    if (has_sc && !d->rast_discard) {
        int64_t x0 = sc->x < 0 ? 0 : sc->x, y0 = sc->y < 0 ? 0 : sc->y, x1 = (int64_t)sc->x + sc->w, y1 = (int64_t)sc->y + sc->h;
        if (R.scaled) { x0 = x0 * vka_scale_up >> vka_scale_shift; y0 = y0 * vka_scale_up >> vka_scale_shift; x1 = x1 * vka_scale_up >> vka_scale_shift; y1 = y1 * vka_scale_up >> vka_scale_shift; }
        if (x1 > R.area.extent.width) x1 = R.area.extent.width; if (y1 > R.area.extent.height) y1 = R.area.extent.height;
        if (x1 <= x0 || y1 <= y0) return;   // an empty scissor: nothing drawn
        scr = (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    }
    { int xs = VKA_ENV("DM_VK_XP_SCISSOR", 0);   // experiment (wrong picture): every draw's scissor cut to xs x xs pixels, to split a pass's GPU time into per-pixel and per-draw work
      if (xs > 0) { if (scr.extent.width > (uint32_t)xs) scr.extent.width = (uint32_t)xs; if (scr.extent.height > (uint32_t)xs) scr.extent.height = (uint32_t)xs; } }
    DY_SET(sc, scr, vk.vkCmdSetScissor(g_cb, 0, 1, &scr));
    if (k.excl) {   // (Lichii: VK_NV_scissor_exclusive, set at every such draw: other pipelines do not keep it)
        int64_t ex[4] = {d->excl[0][0], d->excl[0][1], (int64_t)d->excl[0][0] + d->excl[0][2], (int64_t)d->excl[0][1] + d->excl[0][3]};
        if (R.scaled) for (int i = 0; i < 4; i++) ex[i] = ex[i] * vka_scale_up >> vka_scale_shift;
        for (int i = 0; i < 2; i++) if (ex[i] < 0) ex[i] = 0;
        excl_arg x = {g_cb, {{(int32_t)ex[0], (int32_t)ex[1]}, {(uint32_t)(ex[2] > ex[0] ? ex[2] - ex[0] : 0), (uint32_t)(ex[3] > ex[1] ? ex[3] - ex[1] : 0)}}};
        if (!vka_enc_call(excl_call, &x, sizeof x)) excl_call(&x);
    }
    // Rasterizer and depth / stencil
    DY_SET1(cull, (uint32_t)(d->poly.cull == NVN_FACE_FRONT ? VK_CULL_MODE_FRONT_BIT : d->poly.cull == NVN_FACE_BACK ? VK_CULL_MODE_BACK_BIT : d->poly.cull == NVN_FACE_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE), vk.vkCmdSetCullMode(g_cb, DY.cull));
    DY_SET1(ff, (uint32_t)((d->poly.front == NVN_FRONT_FACE_CCW) != vka_queue_origin_ll() ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE), vk.vkCmdSetFrontFace(g_cb, DY.ff));   // as nvn_mtl.m's winding
    // A mesh pipeline has no input assembly: the topology and primitive restart are not set for it, and set again at the next draw that has them (its pipeline bind may
    // have left them undefined)
    if (mesh) DY.topo = DY.pr = ~0u;
    else DY_SET1(topo, (uint32_t)(topo), vk.vkCmdSetPrimitiveTopology(g_cb, DY.topo));
    int hd = R.df != VK_FORMAT_UNDEFINED, hs = R.sf != VK_FORMAT_UNDEFINED;
    DY_SET1(dt, (uint32_t)(hd && d->ds.dt), vk.vkCmdSetDepthTestEnable(g_cb, DY.dt));
    DY_SET1(dw, (uint32_t)(hd && d->ds.dt && d->ds.dw), vk.vkCmdSetDepthWriteEnable(g_cb, DY.dw));
    DY_SET1(dc, (uint32_t)(d->ds.df >= 1 && d->ds.df <= 8 ? (VkCompareOp)(d->ds.df - 1) : VK_COMPARE_OP_ALWAYS), vk.vkCmdSetDepthCompareOp(g_cb, DY.dc));
    // Depth bounds (SetDepthBounds): the test against the stored depth; the range clamped to [0, 1] (no VK_EXT_depth_range_unrestricted)
    DY_SET1(dbt, (uint32_t)(hd && V.depth_bounds && d->dbounds.on), vk.vkCmdSetDepthBoundsTestEnable(g_cb, DY.dbt));
    if (DY.dbt) { float r[2] = {d->dbounds.lo < 0 ? 0 : d->dbounds.lo > 1 ? 1 : d->dbounds.lo, d->dbounds.hi < 0 ? 0 : d->dbounds.hi > 1 ? 1 : d->dbounds.hi};
                  DY_SET(dbr, r, vk.vkCmdSetDepthBounds(g_cb, r[0], r[1])); }
    DY_SET1(stt, (uint32_t)(hs && d->ds.st), vk.vkCmdSetStencilTestEnable(g_cb, DY.stt));
    #define SOP(x) ((x) >= 1 && (x) <= 8 ? (VkStencilOp)((x) - 1) : VK_STENCIL_OP_KEEP)
    #define SCMP(x) ((x) >= 1 && (x) <= 8 ? (VkCompareOp)((x) - 1) : VK_COMPARE_OP_ALWAYS)
    { uint32_t so[8] = {SOP(d->ds.fail_f), SOP(d->ds.zpass_f), SOP(d->ds.zfail_f), SCMP(d->ds.sf_f), SOP(d->ds.fail_b), SOP(d->ds.zpass_b), SOP(d->ds.zfail_b), SCMP(d->ds.sf_b)};
      DY_SET(sop, so, (vk.vkCmdSetStencilOp(g_cb, VK_STENCIL_FACE_FRONT_BIT, so[0], so[1], so[2], so[3]), vk.vkCmdSetStencilOp(g_cb, VK_STENCIL_FACE_BACK_BIT, so[4], so[5], so[6], so[7]))); }
    #undef SOP
    #undef SCMP
    { uint32_t sm[6] = {d->st_vmask[0], d->st_vmask[1], d->st_wmask[0], d->st_wmask[1], d->st_ref[0], d->st_ref[1]};
      DY_SET(smask, sm, (vk.vkCmdSetStencilCompareMask(g_cb, VK_STENCIL_FACE_FRONT_BIT, sm[0]), vk.vkCmdSetStencilCompareMask(g_cb, VK_STENCIL_FACE_BACK_BIT, sm[1]),
                         vk.vkCmdSetStencilWriteMask(g_cb, VK_STENCIL_FACE_FRONT_BIT, sm[2]), vk.vkCmdSetStencilWriteMask(g_cb, VK_STENCIL_FACE_BACK_BIT, sm[3]),
                         vk.vkCmdSetStencilReference(g_cb, VK_STENCIL_FACE_FRONT_BIT, sm[4]), vk.vkCmdSetStencilReference(g_cb, VK_STENCIL_FACE_BACK_BIT, sm[5]))); }
    int bias_bit = d->poly.mode == NVN_POLYGON_MODE_POINT ? NVN_POLYGON_OFFSET_ENABLE_POINT : d->poly.mode == NVN_POLYGON_MODE_LINE ? NVN_POLYGON_OFFSET_ENABLE_LINE : NVN_POLYGON_OFFSET_ENABLE_FILL;
    int bias = (d->poly.offset_en & bias_bit) != 0;
    DY_SET1(dbe, (uint32_t)(bias), vk.vkCmdSetDepthBiasEnable(g_cb, DY.dbe));
    { float db[3] = {bias ? d->po.units : 0, bias && V.depth_bias_clamp ? d->po.clamp : 0, bias ? d->po.factor : 0}; DY_SET(db, db, vk.vkCmdSetDepthBias(g_cb, db[0], db[1], db[2])); }
    DY_SET1(rd, (uint32_t)(d->rast_discard != 0), vk.vkCmdSetRasterizerDiscardEnable(g_cb, DY.rd));
    DY_SET(bc, d->blend_color, vk.vkCmdSetBlendConstants(g_cb, d->blend_color));
    if (!DY.valid) vk.vkCmdSetLineWidth(g_cb, 1.0f);
    int restart = di && d->restart.on;
    if (restart) { uint32_t all = di->itype == 0 ? 0xff : di->itype == 1 ? 0xffff : 0xffffffffu; if (d->restart.index != all) VRED_ONCE("primitive restart index %#x is not the all-ones index Vulkan restarts at", d->restart.index); }
    if (!mesh) DY_SET1(pr, (uint32_t)(restart), vk.vkCmdSetPrimitiveRestartEnable(g_cb, DY.pr));
    DY.valid = 1;

    PT(7);
    // The draw
    uint32_t inst = (uint32_t)((dw ? dw->instances : di->instances) > 0 ? (dw ? dw->instances : di->instances) : 1), binst = d->b.base_instance;
    if (mesh) vka_n_mesh_draws++;   // Lichii: the GPU profiler's mesh draws (vka_gprof.c)
    if (g_fbd.fb) {   // Fritchii: the mesh emulation's draw (mesh_fb_compute)
        if (!dw->ind) vk.vkCmdDraw(g_cb, g_fbd.groups * g_fbd.fb->group_vertices, 1, 0, 0);
        else vk.vkCmdDrawIndirect(g_cb, g_mesh_out.buffer, g_fbd.args + 16, 1, 16);
    } else if (mesh && !ind) {   // count workgroups of the task shader, or of the mesh shader without one (the device's limit per dimension: maxTaskWorkGroupCount / maxMeshWorkGroupCount, 65535 on the Adreno 840)
        const int d3 = dw->prim == NVN_PRIM_MESH_3D;
        const uint32_t g[3] = {(uint32_t)dw->count, d3 ? (uint32_t)dw->instances : 1, d3 ? (uint32_t)dw->base_instance : 1}, *maxg = sh[6] ? V.task_groups : V.mesh_groups;
        static uint32_t largest; if (g[0] > largest) { largest = g[0]; static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VLOG("mesh draw: largest so far %u workgroups (task %016llx mesh %016llx)", largest, sh[6] ? (unsigned long long)sh[6]->key : 0ull, (unsigned long long)sh[7]->key); }
        if (g[0] > maxg[0] || g[1] > maxg[1] || g[2] > maxg[2]) SKIP("mesh draw of %ux%ux%u workgroups: more than the device's %ux%ux%u; skipped", g[0], g[1], g[2], maxg[0], maxg[1], maxg[2]);
        vk.vkCmdDrawMeshTasksEXT(g_cb, g[0], g[1], g[2]);
    } else if (mcpu) {
        const uint32_t n = dw->mdi_max > 0 ? (uint32_t)dw->mdi_max : 1, st = dw->mdi_stride > 0 ? (uint32_t)dw->mdi_stride : 8, *maxg = sh[6] ? V.task_groups : V.mesh_groups;
        for (uint32_t i = 0; i < n; i++) { uint32_t x = 0;
            if (!cpu_read(ind + (uint64_t)i * st, &x, 4)) SKIP("indirect mesh draw: record at %#llx not readable: skipped", (unsigned long long)(ind + (uint64_t)i * st));
            if (x > maxg[0]) x = maxg[0];
            if (x) vk.vkCmdDrawMeshTasksEXT(g_cb, x, 1, 1); }
    } else if (mesh) {   // indirect: {x, y, z} records as they are, {count, first} ones as mesh_records made them; a count the GPU reads where the device can
        uint32_t n = dw->mdi_max > 0 ? (uint32_t)dw->mdi_max : 1, st = mrec.buf ? 12 : dw->mdi_stride > 0 ? (uint32_t)dw->mdi_stride : 12;
        VkBuffer b = mrec.buf; VkDeviceSize o = mrec.off;
        if (!b && (!vka_addr(ind, (uint64_t)(n - 1) * st + 12, &b, &o, NULL) || (o & 3) || (st & 3) || st < 12))
            SKIP("indirect mesh draw: records at %#llx (%u, stride %u) not in one placed segment or not aligned: skipped", (unsigned long long)ind, n, st);
        VkBuffer cb_; VkDeviceSize co_;
        if (dw->mdi_cnt && V.draw_indirect_count && vka_addr(dw->mdi_cnt, 4, &cb_, &co_, NULL) && !(co_ & 3)) vk.vkCmdDrawMeshTasksIndirectCountEXT(g_cb, b, o, cb_, co_, n, st);
        else {
            if (dw->mdi_cnt) { uint32_t c = 0; if (!cpu_read(dw->mdi_cnt, &c, 4)) SKIP("indirect-count mesh draw: count at %#llx not readable: skipped", (unsigned long long)dw->mdi_cnt);
                               if (c < n) n = c; VRED_ONCE("indirect-count mesh draws read their count on the CPU at replay"); }
            if (n == 1 || V.multi_draw_indirect) { if (n) vk.vkCmdDrawMeshTasksIndirectEXT(g_cb, b, o, n, st); }
            else for (uint32_t i = 0; i < n; i++) vk.vkCmdDrawMeshTasksIndirectEXT(g_cb, b, o + (VkDeviceSize)i * st, 1, 12);
        }
    } else if (expand) {   // quads or a line loop: the indices made here, 32-bit, in scratch memory
        uint32_t count = (uint32_t)(dw ? dw->count : di->count), *src = NULL;
        if (di && !(src = read_indices(di->ibuf, di->itype, count))) SKIP("indices at %#llx are not readable: the draw is skipped", (unsigned long long)di->ibuf);
        uint32_t nout = expand == 1 ? count / 4 * 6 : count + 1; vka_span s;
        if (!nout || !vka_scratch(4 * (VkDeviceSize)nout, 4, &s)) { free(src); return; }
        uint32_t *o = (uint32_t *)s.ptr, base = dw ? (uint32_t)dw->first : 0;
        #define IX(i) (src ? src[i] : base + (i))
        if (expand == 1) for (uint32_t q = 0; q < count / 4; q++) { static const uint8_t t[6] = {0, 1, 2, 0, 2, 3}; for (int j = 0; j < 6; j++) o[6 * q + j] = IX(4 * q + t[j]); }
        else { for (uint32_t i = 0; i < count; i++) o[i] = IX(i); o[count] = IX(0); }
        #undef IX
        free(src);
        ib_bind(s.buf, s.off, VK_INDEX_TYPE_UINT32);
        vk.vkCmdDrawIndexed(g_cb, nout, inst, 0, di ? di->base_vertex : 0, binst);
    } else if (dw) {
        if (ind) { VkBuffer b; VkDeviceSize o; if (!vka_addr(ind, 16, &b, &o, NULL) || (o & 3)) SKIP("indirect draw arguments at %#llx are not in placed guest RAM: skipped", (unsigned long long)ind);
                   VkBuffer cb_; VkDeviceSize co_;
                   if (mdi_gpu && vka_addr(ind, (uint64_t)(dw->mdi_max - 1) * (uint32_t)dw->mdi_stride + 16, NULL, NULL, NULL) && vka_addr(mdi, 4, &cb_, &co_, NULL) && !(co_ & 3) && dw->mdi_stride >= 16 && !(dw->mdi_stride & 3))
                       vk.vkCmdDrawIndirectCount(g_cb, b, o, cb_, co_, (uint32_t)dw->mdi_max, (uint32_t)dw->mdi_stride);
                   else if (mdi_gpu) SKIP("indirect-count multi-draw: records at %#llx (%d, stride %d) or count at %#llx not in one placed segment: skipped", (unsigned long long)ind, dw->mdi_max, dw->mdi_stride, (unsigned long long)mdi);
                   else vk.vkCmdDrawIndirect(g_cb, b, o, 1, 16); }
        else vk.vkCmdDraw(g_cb, (uint32_t)dw->count, inst, (uint32_t)dw->first, binst);
    } else {
        VkBuffer ib; VkDeviceSize io; VkIndexType it = di->itype == 0 ? VK_INDEX_TYPE_UINT8 : di->itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        size_t isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4;
        if (it == VK_INDEX_TYPE_UINT8 && !V.idx8) {   // 8-bit indices on a device without them: widened on the CPU
            if (ind) SKIP("an indirect draw of 8-bit indices on a device without them: skipped");
            uint32_t *src = read_indices(di->ibuf, 0, (uint32_t)di->count); vka_span s;
            if (!src || !vka_scratch(2 * (VkDeviceSize)di->count + 4, 4, &s)) { free(src); SKIP("8-bit indices at %#llx not readable: skipped", (unsigned long long)di->ibuf); }
            for (int i = 0; i < di->count; i++) ((uint16_t *)s.ptr)[i] = (uint16_t)src[i];
            free(src); ib = s.buf; io = s.off; it = VK_INDEX_TYPE_UINT16;
            if (restart) VRED_ONCE("primitive restart with widened 8-bit indices");
        } else if (!vka_addr(di->ibuf, ind ? isz : isz * (uint64_t)(di->count > 0 ? di->count : 1), &ib, &io, NULL) || io % isz) SKIP("index buffer at %#llx is not in placed guest RAM: skipped", (unsigned long long)di->ibuf);
        ib_bind(ib, io, it);
        if (ind) { VkBuffer b; VkDeviceSize o; if (!vka_addr(ind, 20, &b, &o, NULL) || (o & 3)) SKIP("indirect draw arguments at %#llx are not in placed guest RAM: skipped", (unsigned long long)ind);
                   VkBuffer cb_; VkDeviceSize co_;
                   if (mdi_gpu && vka_addr(ind, (uint64_t)(di->mdi_max - 1) * (uint32_t)di->mdi_stride + 20, NULL, NULL, NULL) && vka_addr(mdi, 4, &cb_, &co_, NULL) && !(co_ & 3) && di->mdi_stride >= 20 && !(di->mdi_stride & 3))
                       vk.vkCmdDrawIndexedIndirectCount(g_cb, b, o, cb_, co_, (uint32_t)di->mdi_max, (uint32_t)di->mdi_stride);
                   else if (mdi_gpu) SKIP("indirect-count multi-draw: records at %#llx (%d, stride %d) or count at %#llx not in one placed segment: skipped", (unsigned long long)ind, di->mdi_max, di->mdi_stride, (unsigned long long)mdi);
                   else {
                       static int chk = -1; if (chk < 0) chk = (int)VKA_ENV("DM_VK_IND_CHECK", 0);
                       if (chk) {   // the arguments as the CPU sees them now: indices past the index buffer's placed segment, or huge counts
                           void *nvn_direct_writable(uint64_t); const uint32_t *w = nvn_direct_writable(ind);
                           if (w && (w[0] > 4000000u || w[1] > 100000u || !vka_addr(di->ibuf, (uint64_t)isz * ((uint64_t)w[2] + w[0] + 1), NULL, NULL, NULL))) {
                               static _Atomic int n; if (atomic_fetch_add(&n, 1) < 40) VRED("indirect indexed draw %016llx/%016llx at %#llx: count %u instances %u first %u base %d baseinst %u (index buffer %#llx, %u bytes each)%s",
                                   sh[0] ? (unsigned long long)sh[0]->key : 0ull, sh[1] ? (unsigned long long)sh[1]->key : 0ull, (unsigned long long)ind, w[0], w[1], w[2], (int)w[3], w[4], (unsigned long long)di->ibuf, (unsigned)isz,
                                   vka_addr(di->ibuf, (uint64_t)isz * ((uint64_t)w[2] + w[0] + 1), NULL, NULL, NULL) ? "" : ": past the placed segment"); }
                       }
                       static int cpu = -1; if (cpu < 0) cpu = (int)VKA_ENV("DM_VK_IND_CPU", 0);   // DM_VK_IND_CPU=1: the arguments read now and drawn directly
                       void *nvn_direct_writable(uint64_t); const uint32_t *w = cpu ? nvn_direct_writable(ind) : NULL;
                       if (w) { if (w[0] && w[1]) vk.vkCmdDrawIndexed(g_cb, w[0], w[1], w[2], (int32_t)w[3], w[4]); }
                       else vk.vkCmdDrawIndexedIndirect(g_cb, b, o, 1, 20);
                   } }
        else vk.vkCmdDrawIndexed(g_cb, (uint32_t)di->count, inst, 0, di->base_vertex, binst);
    }
    { void vka_crumb(VkCommandBuffer, uint64_t, uint64_t, uint64_t); extern const volatile uint32_t *vka_mrec_last;
      vka_crumb(g_cb, sh[7] ? sh[7]->key : sh[0] ? sh[0]->key : 0, sh[1] ? sh[1]->key : 0, mrec.buf && vka_mrec_last ? (1ull << 62) | (uint64_t)(uintptr_t)vka_mrec_last : (uint64_t)ind ? (uint64_t)ind : ((uint64_t)(uint32_t)inst << 32 | (uint32_t)(di ? di->count : dw->count))); }
    g_draws++;
    PT(8);
    if (vka_nan_on() >= 2 && vka_nan_split(R.cim, R.nci)) vka_rp_end();   // DM_VK_NANSCAN=2: a scan inside the game's pass
}

// Fritchii: Ocarina of Time's No sharpening (oot_anti_aliasing, docs/nx2/oot-network-bypass.md) in Direct NVN. The game runs a prepass on the tonemapped image, two
// passes, then the tensor network, which writes the scene image (same format and size as the tonemapped one). With No sharpening the network's store is replaced by a copy
// of the image the prepass samples, and while that copy works the three passes before it are skipped too (a failed copy runs the whole chain next frame).
// Keys are the translated compute shaders' (game build E0F3CCFA...). FRITCHII_OOT_BYPASS=0 / 1 overrides the setting
#define OOT_PRE 0xf8965bffbdf51d78ull
#define OOT_NET 0x8df148d58b7cbe8eull
static int g_oot_src, g_oot_ok;
static int oot_bypass(uint64_t key) {
    static int on = -1; if (on < 0) { int fritchii_oot_no_sharpening(void); on = (int)VKA_ENV("FRITCHII_OOT_BYPASS", -1); if (on < 0) on = fritchii_oot_no_sharpening(); if (on) VLOG("Ocarina of Time: No sharpening: the anti-aliasing network is replaced by a copy"); }
    if (!on || (key != OOT_PRE && key != 0xd17eefaff4f0bc23ull && key != 0xa335c1db6270c355ull && key != OOT_NET)) return 0;
    const vka_res *r = g_wk.r[5]; int n = g_wk.n[5];
    if (key == OOT_PRE) { g_oot_src = 0; for (int i = 0; i < n; i++) if (r[i].what == VKA_R_TEX && r[i].tid && r[i].desc != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) { g_oot_src = r[i].tid; break; } }
    if (key != OOT_NET) return g_oot_ok;
    int dst = 0; uint8_t ddim = 0; for (int i = 0; i < n; i++) if (r[i].tid && r[i].desc == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) { dst = r[i].tid; ddim = r[i].dim; }
    g_oot_ok = 0;
    if (!g_oot_src || !dst) return 0;
    VkImageLayout lay; int sc = 0;
    if (!vka_tex_id_view_rec(g_oot_src, g_cb, 0x1, 0, &lay, &sc) || !vka_tex_id_view_rec(dst, g_cb, ddim, 1, &lay, &sc)) return 0;   // (their bookkeeping: read, written by the GPU)
    vka_img *a = vka_tex_id_img(g_oot_src, g_cb), *b = vka_tex_id_img(dst, g_cb);
    if (!a || !b || a == b || a->w != b->w || a->h != b->h || a->f.vk != b->f.vk || a->samples > 1 || b->samples > 1) {
        static int nw; if (nw++ < 4) VLOG("Ocarina of Time: No sharpening: unexpected network images (%ux%u, %ux%u): the network runs", a ? a->w : 0, a ? a->h : 0, b ? b->w : 0, b ? b->h : 0);
        return 0;
    }
    vka_barrier(g_cb);
    VkImageCopy c = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {a->w, a->h, 1}};
    vk.vkCmdCopyImage(g_cb, a->img, VK_IMAGE_LAYOUT_GENERAL, b->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    vka_barrier(g_cb);
    g_oot_ok = 1;
    return 1;
}
// Fritchii: Ocarina of Time's movie network (the title screen's movie: 14 dispatches, ~30 ms of GPU a frame with emulated tensor operations) replaced by a bilinear copy of
// the decoded frame into the network's output, as the normal renderer does (kepler_compute.cpp, oot_movie_upscale). The network's first pass (OOT_MOVIE_IN) samples the
// decoded frame and stores no image: it opens a window of skipped dispatches; the first one in it that stores an image upscaling the frame is replaced by the copy
// and closes it. A failed copy runs the network for the rest of the session
#include "video_core/host_shaders/vulkan_nvn_direct_oot_movie_comp_spv.h"
#define OOT_MOVIE_IN 0xba079f68ae288dbaull
static struct { VkDescriptorSetLayout dsl; VkPipelineLayout layout; VkPipeline pipe; VkSampler smp; int failed; } g_mov;
static int g_mov_src, g_mov_left, g_mov_off = -1;
static int movie_pipeline(void) {
    if (g_mov.pipe || g_mov.failed) return g_mov.pipe != VK_NULL_HANDLE;
    g_mov.failed = 1;
    const VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = V.push_desc ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0, .bindingCount = 2, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof VULKAN_NVN_DIRECT_OOT_MOVIE_COMP_SPV, .pCode = VULKAN_NVN_DIRECT_OOT_MOVIE_COMP_SPV};
    VkShaderModule m = VK_NULL_HANDLE; VkResult r = vk.vkCreateDescriptorSetLayout(V.dev, &li, NULL, &g_mov.dsl);
    VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_mov.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (!r) r = vk.vkCreatePipelineLayout(V.dev, &pi, NULL, &g_mov.layout);
    if (!r) r = vk.vkCreateSampler(V.dev, &si, NULL, &g_mov.smp);
    if (!r) r = vk.vkCreateShaderModule(V.dev, &mi, NULL, &m);
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}, .layout = g_mov.layout};
    if (!r) r = vk.vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, &g_mov.pipe);
    if (m) vk.vkDestroyShaderModule(V.dev, m, NULL);
    if (r) { VRED("Ocarina of Time movie copy pipeline: %d; the movie network runs", r); g_mov.pipe = VK_NULL_HANDLE; return 0; }
    g_mov.failed = 0; return 1;
}
static int movie_copy(int src, int dst, uint8_t ddim) {   // 1 = the copy is recorded
    if (!movie_pipeline()) return 0;
    VkImageLayout sl = VK_IMAGE_LAYOUT_GENERAL, dl = VK_IMAGE_LAYOUT_GENERAL; int sc = 0;
    VkImageView sv = vka_tex_id_view_rec(src, g_cb, 0x1, 0, &sl, &sc), dv = sv ? vka_tex_id_view_rec(dst, g_cb, ddim, 1, &dl, &sc) : VK_NULL_HANDLE;
    vka_img *b = vka_tex_id_img(dst, g_cb);
    if (!sv || !dv || !b) return 0;
    { static int once; vka_img *a = vka_tex_id_img(src, g_cb); if (!once++ && a) VLOG("Ocarina of Time movie copy: source NVN format %#x (VkFormat %d, view format %d, sRGB %d) %ux%u -> output NVN format %#x (VkFormat %d) %ux%u", a->d.format, a->f.vk, a->f.view, a->f.srgb, a->w, a->h, b->d.format, b->f.vk, b->w, b->h); }
    VkDescriptorImageInfo ii[2] = {{g_mov.smp, sv, sl}, {VK_NULL_HANDLE, dv, dl}};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[0]},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii[1]}};
    vka_barrier(g_cb);
    vk.vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_mov.pipe);
    if (V.push_desc) vk.vkCmdPushDescriptorSetKHR(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_mov.layout, 0, 2, w);
    else { VkDescriptorSet st = set_alloc(g_mov.dsl); if (!st) return 0;
           w[0].dstSet = w[1].dstSet = st; vk.vkUpdateDescriptorSets(V.dev, 2, w, 0, NULL); vk.vkCmdBindDescriptorSets(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_mov.layout, 0, 1, &st, 0, NULL); }
    // (no sRGB decode: with it the movie came out far darker than the game's own network draws it, brightness 64 against 115; FRITCHII_OOT_MOVIE_DECODE=1 decodes)
    const uint32_t pc[3] = {b->w, b->h, (uint32_t)VKA_ENV("FRITCHII_OOT_MOVIE_DECODE", 0)};
    vk.vkCmdPushConstants(g_cb, g_mov.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vk.vkCmdDispatch(g_cb, (b->w + 7) / 8, (b->h + 7) / 8, 1);
    vka_barrier(g_cb);
    vka_dyn_invalidate();
    return 1;
}
static int oot_movie(uint64_t key) {
    if (g_mov_off < 0) { int fritchii_oot_movie_bilinear(void); g_mov_off = !fritchii_oot_movie_bilinear(); if (!g_mov_off) VLOG("Ocarina of Time: movies drawn with a bilinear copy in place of the game's movie network (oot_movie_upscale)"); }
    if (g_mov_off) return 0;
    const vka_res *r = g_wk.r[5]; int n = g_wk.n[5];
    int nsamp = 0, samp = 0, nstore = 0, dst = 0; uint8_t ddim = 0;
    for (int i = 0; i < n; i++) if (r[i].tid && (r[i].what == VKA_R_TEX || r[i].what == VKA_R_IMG)) {
        if (r[i].desc == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) { nstore++; dst = r[i].tid; ddim = r[i].dim; } else { nsamp++; samp = r[i].tid; } }
    if (key == OOT_MOVIE_IN && nsamp == 1 && !nstore) { g_mov_src = samp; g_mov_left = 32; return 1; }
    if (g_mov_left <= 0) return 0;
    if (nstore && g_mov_src) {
        vka_img *a = vka_tex_id_img(g_mov_src, g_cb), *b = vka_tex_id_img(dst, g_cb);
        if (a && b && a != b && b->w > a->w && b->w <= 4 * a->w) {
            const uint64_t ca = (uint64_t)b->w * a->h, cb = (uint64_t)b->h * a->w;
            if ((ca > cb ? ca - cb : cb - ca) * 32 <= ca) {   // the same aspect: the network's output
                g_mov_left = 0;
                if (movie_copy(g_mov_src, dst, ddim)) return 1;
                g_mov_off = 1; VRED("Ocarina of Time: the movie copy failed: the movie network runs from now on");
                return 0;
            }
        }
    }
    g_mov_left--;
    return 1;
}
int dm_sdk_ounce(void);
static int g_nvn2 = -1;
void vka_set_nvn2(int on) { g_nvn2 = on != 0; }
int vka_nvn2(void) { if (g_nvn2 < 0) g_nvn2 = dm_sdk_ounce(); return g_nvn2; }   // first called at a dispatch or a capture, after the modules are loaded
void vka_dispatch(vka_dstate *d, const p_dispatch *dp) {
    if (!g_cb || !draws_on() || !statics()) { g_skipped++, vka_skip_line[__LINE__ & 2047]++; return; }
    if (!d->inited) vka_dstate_reset(d);
    vka_rp_end();
    vka_shader *sh[VKA_NSTAGE] = {0}; unsigned bad = 0;
    d->b.dispatch[0] = (uint32_t)dp->x; d->b.dispatch[1] = (uint32_t)dp->y; d->b.dispatch[2] = (uint32_t)dp->z; d->b.nvn2 = (uint32_t)vka_nvn2();
    vka_rc_draw(d);   // Lichii: the state shaders recompiled at this dispatch read (nvn_recompile.c)
    if (!resolve(d->cprog, 5, &sh[5], &bad)) SKIP("dispatch skipped: a program without shaders");
    if (bad) { g_skipped++, vka_skip_line[__LINE__ & 2047]++; return; }
    if (!sh[5]) SKIP("dispatch with no compute shader bound: skipped");
    {   // DM_VK_SKIP_CS=<keys>: those compute shaders' dispatches skipped; DM_VK_CS_LIST=1: each compute shader dispatched, logged once (diagnostics)
        static const char *skip; static int on = -1, list; if (on < 0) { skip = getenv("DM_VK_SKIP_CS"); list = VKA_ENV("DM_VK_CS_LIST", 0) != 0; on = skip || list; }
        if (on) {
            char kx[17]; snprintf(kx, sizeof kx, "%016llx", (unsigned long long)sh[5]->key);
            if (list) { static uint64_t seen[2048]; static int ns; int i = 0; for (; i < ns && seen[i] != sh[5]->key; i++) {} if (i == ns && ns < 2048) { seen[ns++] = sh[5]->key; VLOG("cs dispatched: %s", kx); } }
            if (skip && strstr(skip, kx)) { g_skipped++; return; }
        } }
    vka_layout lay; if (!vka_layout_of(sh, 1, &lay)) SKIP("dispatch skipped: no pipeline layout");
    VkPipeline p = vka_cs_pipeline(sh[5], &lay);
    if (vka_nan_on()) vka_tex_watch_dispatch(sh[5]->key);
    if (!p) { g_skipped++, vka_skip_line[__LINE__ & 2047]++; return; }
    VkBuffer ib = VK_NULL_HANDLE; VkDeviceSize io = 0;
    if (dp->ind && (!vka_addr(dp->ind, 12, &ib, &io, NULL) || (io & 3))) SKIP("indirect dispatch arguments at %#llx are not in placed guest RAM: skipped", (unsigned long long)dp->ind);
    uint32_t sb = spill_bytes(sh[5]); uint64_t ns = 0;   // shared memory in the spill buffer: the slices this dispatch binds (spill_buf)
    if (sb) {
        if (!spill_buf(sb)) SKIP("dispatch of %016llx skipped: no shared memory spill buffer of %u bytes", (unsigned long long)sh[5]->key, sb);
        uint64_t cap = g_spill_cap / sb, total = dp->ind ? cap : (uint64_t)(dp->x > 0 ? dp->x : 0) * (uint64_t)(dp->y > 0 ? dp->y : 0) * (uint64_t)(dp->z > 0 ? dp->z : 0);
        ns = total < cap ? total : cap; if (!ns) ns = 1;
        g_spill_range = ns * sb;
        if (dp->ind) VRED_ONCE("indirect dispatch of a program with its shared memory in the spill buffer (%016llx): its %llu slices are shared by workgroups that many apart", (unsigned long long)sh[5]->key, (unsigned long long)ns);
    }
    c0_at c0 = {0};
    walk_all(d, sh);
    if (oot_bypass(sh[5]->key) || oot_movie(sh[5]->key)) { g_dispatches++; return; }
    vka_img *pims[32]; int pbs[32], pn = 0;   // Fritchii, FRITCHII_CS_PROBE (vka_nanscan.c)
    { static int on = -1; if (on < 0) on = getenv("FRITCHII_CS_PROBE") != NULL;
      if (on) { void vka_cs_probe_dump(VkCommandBuffer, uint64_t, int, vka_img *const *, const int *, int);
                for (int i = 0; i < g_wk.n[5] && pn < 32; i++) if (g_wk.r[5][i].tid && (g_wk.r[5][i].what == VKA_R_TEX || g_wk.r[5][i].what == VKA_R_IMG)) { pims[pn] = vka_tex_id_img(g_wk.r[5][i].tid, g_cb); pbs[pn++] = (int)g_wk.r[5][i].binding; }
                int vka_probe_turn(uint64_t, int); if (!vka_probe_turn(sh[5]->key, 0)) pn = 0; else { vka_cs_probe_dump(g_cb, sh[5]->key, 0, pims, pbs, pn); if (!pn) pn = -1; } } }   // (-1: its turn, no images)
    if (!bind_sets(d, sh, &lay, VK_PIPELINE_BIND_POINT_COMPUTE, &c0)) { g_skipped++, vka_skip_line[__LINE__ & 2047]++; return; }
    VkDescriptorBufferInfo pbuf[16]; int pbb[16], pnb = 0;   // Fritchii, FRITCHII_CS_PROBE: the storage buffers, dumped before and after
    if (pn) { void vka_cs_probe_buf(VkCommandBuffer, uint64_t, int, VkBuffer, VkDeviceSize, VkDeviceSize, int);
        for (uint32_t i = 0; i < g_w.n && pnb < 16; i++) if (g_w.w[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && g_w.w[i].pBufferInfo && g_w.w[i].pBufferInfo->range <= (1u << 20)) {
            pbuf[pnb] = *g_w.w[i].pBufferInfo; pbb[pnb++] = (int)g_w.w[i].dstBinding; }
        for (int i = 0; i < pnb; i++) vka_cs_probe_buf(g_cb, sh[5]->key, 0, pbuf[i].buffer, pbuf[i].offset, pbuf[i].range, pbb[i]);
        vka_dyn_invalidate(); if (!bind_sets(d, sh, &lay, VK_PIPELINE_BIND_POINT_COMPUTE, &c0)) { g_skipped++; return; } }
    if (dp->ind && c0.buf) {   // the group counts the GPU wrote, into c0's dispatch words before the dispatch reads them (nvn_mtl.m g_c0_dim_buf); NVN2 reads them at c0 + 4 too
        VkBufferCopy c[2] = {{io, c0.off + DMSH_C0_DISPATCH, 12}, {io, c0.off + DMSH_C0_DISPATCH_NVN2, 12}};
        vk.vkCmdCopyBuffer(g_cb, ib, c0.buf, d->b.nvn2 ? 2 : 1, c);
        vka_barrier(g_cb);
    }
    // DM_VK_DISPATCH_LOG=1: one line per dispatch (its index in the submission's run of dispatches since the last log reset, key, groups, workgroup, shared memory, flags);
    // DM_VK_DISPATCH_SKIP=<from>[,<to>]: dispatches from..to (default: to the end) are recorded without their vkCmdDispatch, to find the one that writes a wrong picture
    uint64_t vka_frame(void); static unsigned long long nd; static uint64_t fr = ~0ull; static int lg; if (vka_frame() != fr) { fr = vka_frame(); lg = (int)VKA_ENV("DM_VK_DISPATCH_LOG", 0); nd = 0; }
    unsigned long long di = nd++; static const char *sk; static int ski; if (!ski) { sk = getenv("DM_VK_DISPATCH_SKIP"); ski = 1; } int skip = 0;   // (read once: getenv scans the environment)
    if (sk && *sk) { char *e; unsigned long long a = strtoull(sk, &e, 10), b = *e == ',' ? strtoull(e + 1, NULL, 10) : ~0ull; skip = di >= a && di <= b; }
    uint64_t vka_exec_queue_serial(void);
    uint64_t vka_exec_cur_fence(void);
    if (lg) VLOG("dispatch %llu (queue %llu fence %llu): %016llx groups %d,%d,%d%s local %u,%u,%u shared %u flags %#x%s", di, (unsigned long long)vka_exec_queue_serial(), (unsigned long long)vka_exec_cur_fence(), (unsigned long long)sh[5]->key, dp->x, dp->y, dp->z, dp->ind ? " (indirect)" : "",
                 sh[5]->e->local[0], sh[5]->e->local[1], sh[5]->e->local[2], sh[5]->e->shared, sh[5]->e->flags, skip ? " SKIPPED" : "");
    if (skip) { vka_barrier(g_cb); g_dispatches++; return; }
    if (vka_gp_active) { char gd[96]; snprintf(gd, sizeof gd, "cs %016llx groups %d,%d,%d local %u,%u,%u", (unsigned long long)sh[5]->key, dp->x, dp->y, dp->z, sh[5]->e->local[0], sh[5]->e->local[1], sh[5]->e->local[2]); vka_gp_pass(gd); }
    vk.vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    if (dp->ind) vk.vkCmdDispatchIndirect(g_cb, ib, io);
    else if (dp->x > 0 && dp->y > 0 && dp->z > 0) {
        if (sb && (uint64_t)dp->x * dp->y * dp->z > ns) spill_dispatch((uint32_t)dp->x, (uint32_t)dp->y, (uint32_t)dp->z, ns);
        else vk.vkCmdDispatch(g_cb, (uint32_t)dp->x, (uint32_t)dp->y, (uint32_t)dp->z);
    }
    { void vka_crumb(VkCommandBuffer, uint64_t, uint64_t, uint64_t); vka_crumb(g_cb, sh[5]->key, 5, dp->ind ? 1ull << 63 : (uint64_t)(uint32_t)dp->x << 32 | (uint32_t)dp->y * (uint32_t)dp->z); }
    if (sb || VKA_ENV("DM_VK_CS_BAR", 1)) vka_barrier_from(g_cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);   // (Fritchii: after the dispatch's writes)   // DM_VK_CS_BAR=0: only the game's barriers (~0.6 ms less in Ocarina of Time, but its HUD flickered)   // (also frees the shared memory spill buffer for the next dispatch). DM_VK_CS_BAR=0: only the game's own barriers
    for (int i = 0; i < pnb; i++) { void vka_cs_probe_buf(VkCommandBuffer, uint64_t, int, VkBuffer, VkDeviceSize, VkDeviceSize, int); vka_cs_probe_buf(g_cb, sh[5]->key, 1, pbuf[i].buffer, pbuf[i].offset, pbuf[i].range, pbb[i]); }
    if (pn) { void vka_cs_probe_dump(VkCommandBuffer, uint64_t, int, vka_img *const *, const int *, int); int vka_probe_turn(uint64_t, int); vka_cs_probe_dump(g_cb, sh[5]->key, 1, pims, pbs, pn); vka_probe_turn(sh[5]->key, 1); }
    g_dispatches++;
}
void vka_draw_stats(uint64_t *draws, uint64_t *skipped, uint64_t *dispatches, uint64_t *pipelines) {
    if (draws) *draws = g_draws; if (skipped) *skipped = g_skipped; if (dispatches) *dispatches = g_dispatches; if (pipelines) *pipelines = 0;
}

// Lichii: the teardown (vka_dev.c vka_destroy): the descriptor pools (their sets with them), the shading-rate attachments and pipeline (the static spans go with
// vka_shader.c's blocks, the placeholders with the images)
void vka_draw_destroy(void) {
    fsc_reset();
    for (int s = 0; s < NSLOT; s++) for (int i = 0; i < g_ndp[s]; i++) vk.vkDestroyDescriptorPool(V.dev, g_dp[s][i], NULL);
    for (int s = 0; s < NSLOT; s++) {
        if (g_vrs_att[s].view) vk.vkDestroyImageView(V.dev, g_vrs_att[s].view, NULL);
        if (g_vrs_att[s].img) vk.vkDestroyImage(V.dev, g_vrs_att[s].img, NULL);
        if (g_vrs_att[s].mem) vk.vkFreeMemory(V.dev, g_vrs_att[s].mem, NULL);
    }
    if (g_vrs.pipe) vk.vkDestroyPipeline(V.dev, g_vrs.pipe, NULL);
    if (g_vrs.layout) vk.vkDestroyPipelineLayout(V.dev, g_vrs.layout, NULL);
    if (g_vrs.dsl) vk.vkDestroyDescriptorSetLayout(V.dev, g_vrs.dsl, NULL);
}
#endif
