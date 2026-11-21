// From Banana Native (github.com/hugorosario/BananaNative, Android port of github.com/seedds/nxnative by seedds), MIT: see LICENSE in src/video_core/nvn_direct/bananative.
// DM_VK_NANSCAN=1: a diagnostic that finds the pass that first writes NaN or infinite texels. After every render pass, each float colour attachment's level / layer is
// scanned by a compute shader (selftest/nanscan.comp) that counts NaN and infinite texels and keeps the first one's position; when the submission is done the completion
// thread logs each pass with any, with its frame, its place in the submission, the attachment (serial, format, size) and the vertex / fragment shaders of its draws.
// DM_VK_NANSCAN=2 also splits long passes to find the draw: the Vulkan pass is ended (and reopened by the next draw, its attachments loaded) after every
// DM_VK_NANSCAN_STEP draws (default 64), in passes into the image DM_VK_NANSCAN_SERIAL (default any), of the game's pass whose index is in DM_VK_NANSCAN_DRAWS=<from>-<to> (default all), at most MAXSPLIT times a submission,
// and only the scans after which an attachment has more NaN / infinite texels than at its previous scan in the submission are logged, with the pass's draw range.
// Bisect with a coarse step over the whole pass, then step 1 over the range it names. Costs a dispatch per attachment per pass (per split with 2): for finding a
// cause, not for play
#if defined(__ANDROID__)
#include "vka.h"
#include "vka_selftest_spv.h"
#include <stdlib.h>
#include <string.h>

enum { NBATCH = 16, MAXR = 1024, NKEY = 8, MAXSPLIT = 256 };
typedef struct { uint32_t nan, inf, first, pad; } res_t;
typedef struct { uint64_t frame, serial; uint32_t pass, fmt, w, h, lv, layer, d0, ndraws, nkeys; uint64_t vs[NKEY], fs[NKEY]; int bind, tid; } rec_t;
typedef struct { rec_t r[MAXR]; int n; res_t *res; VkDeviceSize off; } batch_t;
static batch_t g_b[NBATCH]; static unsigned g_bi; static vka_span g_res;
static VkPipeline g_pipe, g_pipe3, g_fetch; static VkPipelineLayout g_lay; static VkDescriptorSetLayout g_dsl;
static uint32_t g_pass; static uint64_t g_pvs[NKEY], g_pfs[NKEY]; static uint32_t g_pn, g_pdraws, g_d0;   // g_pdraws: draws of the game's pass so far, g_d0: the segment's first
static uint64_t g_probe; static int g_split, g_nsplit; static uint32_t g_step = 64, g_from, g_to = UINT32_MAX; static uint64_t g_only;
static _Atomic int g_lines;

int vka_nan_on(void) {
    static int v = -1; if (v >= 0) return v;
    const char *e = getenv("DM_VK_NANSCAN"); v = e && atoi(e) > 0 && V.push_desc ? atoi(e) : 0;
    if (v >= 2) {
        const char *s = getenv("DM_VK_NANSCAN_STEP"), *r = getenv("DM_VK_NANSCAN_DRAWS"); if (s && atoi(s) > 0) g_step = (uint32_t)atoi(s);
        const char *o = getenv("DM_VK_NANSCAN_SERIAL"); if (o) g_only = strtoull(o, NULL, 10);
        if (r) { char *x; g_from = (uint32_t)strtoul(r, &x, 10); if (*x == '-') g_to = (uint32_t)strtoul(x + 1, NULL, 10); }
        VLOG("DM_VK_NANSCAN=2: passes split every %u draws in draws %u-%u of a pass%s, at most %d splits a submission", g_step, g_from, g_to, g_only ? " into DM_VK_NANSCAN_SERIAL" : "", MAXSPLIT);
    } else if (v) VLOG("DM_VK_NANSCAN: every pass's float attachments are scanned for NaN / infinite texels");
    return v;
}
// DM_VK_NANSCAN=2, after a draw: 1 = end the Vulkan pass now for a scan (the game's pass goes on)
int vka_nan_split(vka_img *const *ims, int n) {
    uint32_t i = g_pdraws;   // the draws of the game's pass so far
    {   // Fritchii: with DM_VK_PASS_DUMP_FS, only after draws with one of those fragment shaders (so the split budget reaches them)
        static const char *fsl; static int fi; if (!fi) { fsl = getenv("DM_VK_PASS_DUMP_FS"); fi = 1; }
        if (fsl) { int hit = 0; for (uint32_t k = 0; k < g_pn && !hit; k++) { char kx[17]; snprintf(kx, sizeof kx, "%016llx", (unsigned long long)g_pfs[k]); hit = strstr(fsl, kx) != NULL; } if (!hit) return 0; }
    }
    if (i <= g_from || i > g_to || (i - g_from) % g_step) return 0;
    if (g_nsplit >= MAXSPLIT) { static _Atomic int n; if (!atomic_exchange(&n, 1)) VLOG("NaN scan: %d splits in one submission, no more in it (at draw %u of pass %u)", MAXSPLIT, i, g_pass); return 0; }
    if (g_only) { int k = 0; while (k < n && !(ims[k] && ims[k]->serial == g_only)) k++; if (k == n) return 0; }   // DM_VK_NANSCAN_SERIAL: only passes into that image
    g_nsplit++; g_split = 1; return 1;
}
static int ready(void) {
    static int st; if (st) return st > 0;
    st = -1;
    if (!vka_static((VkDeviceSize)NBATCH * MAXR * sizeof(res_t), V.sb_align, &g_res)) return 0;
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 2, .pBindings = b};
    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof SPV_NANSCAN_COMP, .pCode = SPV_NANSCAN_COMP};
    VkResult r;
    if ((r = vk.vkCreateDescriptorSetLayout(V.dev, &dl, NULL, &g_dsl)) || (r = vk.vkCreatePipelineLayout(V.dev, &li, NULL, &g_lay))) { VRED("NaN scan: %d", r); return 0; }
    for (int i = 0; i < 3; i++) {   // 2D-array views (attachments, 2D / cube inputs), 3D inputs, the ASTC check's fetch
        VkShaderModule m = VK_NULL_HANDLE;
        if (i == 1) { mi.codeSize = sizeof SPV_NANSCAN3_COMP; mi.pCode = SPV_NANSCAN3_COMP; }
        if (i == 2) { mi.codeSize = sizeof SPV_FETCH_COMP; mi.pCode = SPV_FETCH_COMP; }
        if ((r = vk.vkCreateShaderModule(V.dev, &mi, NULL, &m))) { VRED("NaN scan: %d", r); return 0; }
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}, .layout = g_lay};
        r = vk.vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, i == 2 ? &g_fetch : i ? &g_pipe3 : &g_pipe);
        vk.vkDestroyShaderModule(V.dev, m, NULL);
        if (r) { VRED("NaN scan pipeline: %d", r); return 0; }
    }
    for (int i = 0; i < NBATCH; i++) { g_b[i].res = (res_t *)(g_res.ptr + (size_t)i * MAXR * sizeof(res_t)); g_b[i].off = g_res.off + (VkDeviceSize)i * MAXR * sizeof(res_t); }
    st = 1; return 1;
}
void vka_nan_draw(uint64_t vs, uint64_t fs) {
    g_pdraws++;   // counted before the draw records: a draw skipped after this still has its index
    for (uint32_t i = 0; i < g_pn; i++) if (g_pvs[i] == vs && g_pfs[i] == fs) return;
    if (g_pn < NKEY) { g_pvs[g_pn] = vs; g_pfs[g_pn] = fs; g_pn++; }
}
uint64_t vka_frame(void);
// The ASTC check (once per run with DM_VK_NANSCAN): an 8x8 ASTC 4x4 texture of four void-extent (constant colour) blocks, sampled by the device and logged. A void-extent block's
// colour is UNORM16 when its bit 9 (dynamic range) is 0 and FP16 when it is 1 (ASTC specification, "Void-Extent Blocks")
static const uint16_t g_vcol[4][4] = {{0x1212, 0x1212, 0x1212, 0xffff}, {0x8000, 0x4000, 0xc000, 0x8000}, {0x3c00, 0x3800, 0x3400, 0x3c00}, {0xffff, 0x0000, 0xffff, 0xffff}};
static const int g_vhdr[4] = {0, 0, 1, 0};
static vka_span g_fetch_out; static int g_astc_state;   // 1 = recorded, report at the next commit
static void astc_report(void *arg) {
    (void)arg; const float *t = (const float *)g_fetch_out.ptr;
    for (int b = 0; b < 4; b++) {
        const float *v = t + 4 * ((b / 2) * 4 * 8 + (b % 2) * 4);   // the block's first texel in the 8x8 texture
        VLOG("NaN scan: ASTC check: void-extent block %d (%s, colour %04x %04x %04x %04x) samples as (%g, %g, %g, %g)", b, g_vhdr[b] ? "HDR: FP16" : "LDR: UNORM16",
             g_vcol[b][0], g_vcol[b][1], g_vcol[b][2], g_vcol[b][3], v[0], v[1], v[2], v[3]);
    }
}
static void astc_check(VkCommandBuffer cb) {
    static uint8_t blocks[4 * 16]; static tex_t t;
    for (int b = 0; b < 4; b++) {
        uint64_t lo = 0x1fc | (uint64_t)g_vhdr[b] << 9 | 3ull << 10 | 0xfffffffffffffull << 12, hi = 0;   // extent all ones: the whole texture
        for (int c = 0; c < 4; c++) hi |= (uint64_t)g_vcol[b][c] << (16 * c);
        memcpy(blocks + 16 * b, &lo, 8); memcpy(blocks + 16 * b + 8, &hi, 8);
    }
    memset(&t, 0, sizeof t);
    t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = 0x79; t.d.width = 8; t.d.height = 8; t.d.depth = 1; t.d.levels = 1; t.d.flags = NVN_TEXTURE_FLAGS_LINEAR; t.d.stride = 32;
    nvn_tex_layout(&t.d, &t.L); t.mem = blocks; t.serial = nvn_new_serial();
    vka_img *im = vka_tex_get(&t, cb);
    if (!im || !vka_static(64 * 16, V.sb_align, &g_fetch_out)) { VLOG("NaN scan: ASTC check: no test texture"); return; }
    memset(g_fetch_out.ptr, 0xff, 64 * 16);
    VkImageView v = vka_view(im, im->f.view, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1, NULL);
    if (!v) return;
    VkDescriptorImageInfo ii = {vka_samp_default(), v, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bi = {g_fetch_out.buf, g_fetch_out.off, 64 * 16};
    VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
                                  {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi}};
    uint32_t pc[4] = {8, 8, 0, 0};
    vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_fetch);
    vk.vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_lay, 0, 2, wr); vka_dyn_invalidate();   // the draws' last push is no longer bound (vka_draw.c push_same)
    vk.vkCmdPushConstants(cb, g_lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vk.vkCmdDispatch(cb, 1, 1, 1);
    vka_barrier(cb);
    g_astc_state = 1;
}
// One dispatch: level lv, layer l0 of im into the batch's next result
static int scan_one(VkCommandBuffer cb, batch_t *b, vka_img *im, uint32_t lv, uint32_t l0, uint32_t pass, int bind, int tid) {
    if (b->n == MAXR) return 0;
    int v3 = im->d.target == NVN_TEXTURE_TARGET_3D;   // a whole level of a 3D texture (inputs only)
    VkImageView v = vka_view(im, im->f.view, v3 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_ASPECT_COLOR_BIT, lv, 1, v3 ? 0 : l0, 1, NULL);
    if (!v) return 0;
    uint32_t w = im->w >> lv ? im->w >> lv : 1, h = im->h >> lv ? im->h >> lv : 1, dz = v3 ? (im->depth >> lv ? im->depth >> lv : 1) : 1;
    int k = b->n++;
    b->res[k] = (res_t){0, 0, 0xffffffffu, 0};
    b->r[k] = (rec_t){vka_frame(), im->serial, pass, (uint32_t)im->d.format, w, h, lv, l0, g_d0, g_pdraws, g_pn, {0}, {0}, bind, tid};
    memcpy(b->r[k].vs, g_pvs, sizeof g_pvs); memcpy(b->r[k].fs, g_pfs, sizeof g_pfs);
    VkDescriptorImageInfo ii = {vka_samp_default(), v, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bi = {g_res.buf, b->off + (VkDeviceSize)k * sizeof(res_t), sizeof(res_t)};
    VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
                                  {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi}};
    uint32_t pc[4] = {w, h, v3 ? dz : 0, 0};
    vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, v3 ? g_pipe3 : g_pipe);
    vk.vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_lay, 0, 2, wr); vka_dyn_invalidate();   // the draws' last push is no longer bound (vka_draw.c push_same)
    vk.vkCmdPushConstants(cb, g_lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vk.vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, dz);
    return 1;
}
// DM_VK_NANSCAN_FS=<fragment shader key> (with DM_VK_NANSCAN=2): the first draw of each frame with that fragment shader has its sampled float textures scanned before it
// (every level and layer, outside the pass): which input holds NaN / infinite texels. 1 = this draw is one (the caller ends the pass and calls vka_nan_inputs)
int vka_nan_probe(uint64_t fs) {
    static uint64_t last = ~0ull; static int init; uint64_t key = g_probe;
    if (!init) { init = 1; const char *e = getenv("DM_VK_NANSCAN_FS"); if (e) g_probe = key = strtoull(e, NULL, 16); }
    uint64_t vka_present_filled(void); const uint64_t pf = vka_present_filled();   // (Fritchii: per replayed frame: two can share the game's frame number)
    if (!key || fs != key || last == pf) return 0;
    last = pf; return 1;
}
static void dump_img(VkCommandBuffer cb, vka_img *im, uint32_t lv, const char *name, const char *keys); static char g_dump_dir[512]; static uint64_t g_dump_frame = ~0ull; static int g_dump_nfr = 1;
uint64_t vka_present_filled(void);
void vka_nan_inputs(VkCommandBuffer cb, vka_img *const *ims, const int *binds, const int *tids, int n) {
    batch_t *b = &g_b[g_bi % NBATCH];
    if (getenv("DM_VK_PASS_DUMP")) {   // Fritchii: with DM_VK_PASS_DUMP, the probed draw's inputs dumped (level 0) once
        static uint64_t done = ~0ull; if (g_dump_frame != ~0ull && vka_frame() >= g_dump_frame && vka_frame() < g_dump_frame + (uint64_t)g_dump_nfr && done != vka_present_filled()) { done = vka_present_filled();   // (Fritchii: each frame of DM_VK_PASS_DUMP_FRAMES) VLOG("pass dump: inputs taken in frame %llu (capture frame %llu)", (unsigned long long)vka_frame(), (unsigned long long)g_dump_frame); vka_barrier(cb);
            for (int i = 0; i < n; i++) { char nm[48], ks[64]; snprintf(nm, sizeof nm, "f%llu_p%llu_in_b%d_t%d", (unsigned long long)(vka_frame() - g_dump_frame), (unsigned long long)vka_present_filled(), binds[i], tids[i]); snprintf(ks, sizeof ks, " input of %016llx", (unsigned long long)g_probe); dump_img(cb, ims[i], 0, nm, ks); }
            vka_barrier(cb); VLOG("pass dump: %d inputs of %016llx dumped", n, (unsigned long long)g_probe); }
    }
    if (!ready()) return;
    int any = 0;
    for (int i = 0; i < n; i++) {
        vka_img *im = ims[i];
        if (!im || im->f.kind != 0 || im->samples > 1 || !(im->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) continue;
        uint32_t nl = im->d.target == NVN_TEXTURE_TARGET_3D ? 1 : im->layers;
        for (uint32_t lv = 0; lv < im->levels && lv < 14; lv++) for (uint32_t l = 0; l < nl && l < 6; l++) any |= scan_one(cb, b, im, lv, l, g_pass, binds[i], tids[i]);
    }
    if (any) vka_barrier(cb);
    static _Atomic int k; if (atomic_fetch_add(&k, 1) < 2) VLOG("NaN scan: frame %llu: %d inputs of fragment shader %016llx scanned before its first draw (only those with NaN / infinite texels are logged)", (unsigned long long)vka_frame(), n, (unsigned long long)g_probe);
}
// After a pass ends (its writes made visible by the caller's barrier): one dispatch per float colour attachment
// Fritchii, DM_VK_PASS_DUMP=<dir> (with DM_VK_NANSCAN=1): when <dir>/capture.now appears, every pass of the next frame has its colour attachments copied to host memory
// and written as <dir>/<n>_p<pass>_s<serial>_<w>x<h>_vk<format>.raw, with a manifest.txt line each (the pass's vertex / fragment shaders): a frame's passes to compare
typedef struct dump_ent { struct dump_ent *next; VkBuffer buf; VkDeviceMemory mem; VkDeviceSize size; char name[160]; char keys[400]; } dump_ent;
static dump_ent *g_dump_list; static unsigned g_dump_n;
static void dump_write(void *arg) {
    for (dump_ent *e = arg, *nx; e; e = nx) {
        nx = e->next; void *p = NULL;
        if (!vk.vkMapMemory(V.dev, e->mem, 0, e->size, 0, &p)) {
            char path[800]; snprintf(path, sizeof path, "%s/%s.raw", g_dump_dir, e->name);
            FILE *f = fopen(path, "wb"); if (f) { fwrite(p, 1, e->size, f); fclose(f); }
            vk.vkUnmapMemory(V.dev, e->mem);
            snprintf(path, sizeof path, "%s/manifest.txt", g_dump_dir); f = fopen(path, "a"); if (f) { fprintf(f, "%s%s\n", e->name, e->keys); fclose(f); }
        }
        vk.vkDestroyBuffer(V.dev, e->buf, NULL); vk.vkFreeMemory(V.dev, e->mem, NULL); free(e);
    }
}
// One image level (all its layers, or a 3D level's slices) copied for the dump; compressed formats as their blocks
static void dump_img(VkCommandBuffer cb, vka_img *im, uint32_t lv, const char *name, const char *keys) {
    if (!im || im->samples > 1 || im->f.kind >= 3) return;
    uint32_t w = im->w >> lv ? im->w >> lv : 1, h = im->h >> lv ? im->h >> lv : 1, d3 = im->d.target == NVN_TEXTURE_TARGET_3D, z = d3 ? (im->depth >> lv ? im->depth >> lv : 1) : 1, nl = d3 ? 1 : im->layers;
    uint32_t bx = (w + im->f.bw - 1) / im->f.bw, by = (h + im->f.bh - 1) / im->f.bh;
    VkDeviceSize size = (VkDeviceSize)bx * by * z * nl * im->f.bpb;
    dump_ent *e = calloc(1, sizeof *e); if (!e) return;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkMemoryRequirements mr; if (vk.vkCreateBuffer(V.dev, &bi, NULL, &e->buf)) { free(e); return; }
    vk.vkGetBufferMemoryRequirements(V.dev, e->buf, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
    if (ai.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &e->mem) || vk.vkBindBufferMemory(V.dev, e->buf, e->mem, 0)) { vk.vkDestroyBuffer(V.dev, e->buf, NULL); free(e); return; }
    VkBufferImageCopy c = {0, 0, 0, {im->aspect, lv, 0, nl}, {0, 0, 0}, {w, h, z}};
    vk.vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, e->buf, 1, &c);
    e->size = size;
    snprintf(e->name, sizeof e->name, "%04u_%s_s%llu_%ux%ux%u_l%u_vk%d", g_dump_n++, name, (unsigned long long)im->serial, w, h, z * nl, lv, (int)im->f.vk);
    snprintf(e->keys, sizeof e->keys, "%s", keys);
    e->next = g_dump_list; g_dump_list = e;
}
// Fritchii: FRITCHII_CS_PROBE's keys are taken in list order: a key dumps only once the one before it in the list has dumped its after (one sequence of dispatches)
static uint64_t g_probe_done_post[64]; static int g_probe_ndone;
int vka_probe_turn(uint64_t key, int when) {   // 1: this dispatch of key dumps (when 0), or its after is recorded (when 1)
    static const char *list; static int li; if (!li) { list = getenv("FRITCHII_CS_PROBE"); li = 1; }
    if (!list || !g_dump_dir[0] || g_dump_frame == ~0ull || vka_frame() < g_dump_frame) return 0;
    char kx[17]; snprintf(kx, sizeof kx, "%016llx", (unsigned long long)key); const char *at = strstr(list, kx); if (!at) return 0;
    int idx = 0; for (const char *c = list; c < at; c++) idx += *c == ',';
    if (idx > g_probe_ndone) return 0;   // an earlier key has not finished
    if (idx < g_probe_ndone) return 0;   // done already
    if (when) g_probe_done_post[g_probe_ndone++] = key;
    return 1;
}
// Fritchii: a buffer range copied for the dump (FRITCHII_CS_PROBE's storage buffers)
void vka_cs_probe_buf(VkCommandBuffer cb, uint64_t key, int when, VkBuffer src, VkDeviceSize off, VkDeviceSize n, int binding) {
    static const char *list; static int li; if (!li) { list = getenv("FRITCHII_CS_PROBE"); li = 1; }
    if (!list || !g_dump_dir[0] || g_dump_frame == ~0ull || vka_frame() < g_dump_frame || vka_frame() > g_dump_frame + 4 || !n || n > (1u << 20)) return;
    char kx[17]; snprintf(kx, sizeof kx, "%016llx", (unsigned long long)key); if (!strstr(list, kx)) return;
    static uint64_t done[256]; static int nd; uint64_t tag = key ^ ((uint64_t)binding << 48) ^ ((uint64_t)when << 60); for (int i = 0; i < nd; i++) if (done[i] == tag) return;
    if (nd < 256) done[nd++] = tag;
    dump_ent *e = calloc(1, sizeof *e); if (!e) return;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkMemoryRequirements mr; if (vk.vkCreateBuffer(V.dev, &bi, NULL, &e->buf)) { free(e); return; }
    vk.vkGetBufferMemoryRequirements(V.dev, e->buf, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
    if (ai.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &e->mem) || vk.vkBindBufferMemory(V.dev, e->buf, e->mem, 0)) { vk.vkDestroyBuffer(V.dev, e->buf, NULL); free(e); return; }
    vka_barrier(cb);
    VkBufferCopy c = {off, 0, n}; vk.vkCmdCopyBuffer(cb, src, e->buf, 1, &c);
    vka_barrier(cb);
    e->size = n; snprintf(e->name, sizeof e->name, "%04u_cs%s_%s_buf_b%d_%llu", g_dump_n++, kx, when ? "post" : "pre", binding, (unsigned long long)n);
    e->next = g_dump_list; g_dump_list = e;
}
// Fritchii, FRITCHII_CS_PROBE=<keys> (with DM_VK_PASS_DUMP): on the capture frame, each listed compute program's images dumped before (when=0) and after (when=1)
// its first dispatch, as <n>_cs<key>_<pre|post>_b<binding>_...: the first dispatch whose output differs from the normal renderer's
void vka_cs_probe_dump(VkCommandBuffer cb, uint64_t key, int when, vka_img *const *ims, const int *binds, int n) {
    static const char *list; static int li; if (!li) { list = getenv("FRITCHII_CS_PROBE"); li = 1; }
    if (!list || !g_dump_dir[0] || g_dump_frame == ~0ull || vka_frame() < g_dump_frame || vka_frame() > g_dump_frame + 4) return;   // (vka_frame is the game's: the replay runs behind it)
    char kx[17]; snprintf(kx, sizeof kx, "%016llx", (unsigned long long)key); if (!strstr(list, kx)) return;
    static uint64_t done[64][2]; static int nd; int i = 0; for (; i < nd && done[i][0] != key; i++) {}
    if (i < nd && (done[i][1] >> when & 1)) return;
    if (i == nd && nd < 64) { done[nd][0] = key; done[nd][1] = 0; nd++; }
    if (when && !(i < 64 && (done[i][1] & 1))) return;   // (an after without its before: a later dispatch's)
    if (i < 64) done[i][1] |= 1u << when;
    vka_barrier(cb);
    for (int k = 0; k < n; k++) { char nm[80]; snprintf(nm, sizeof nm, "cs%s_%s_b%d", kx, when ? "post" : "pre", binds[k]); dump_img(cb, ims[k], 0, nm, ""); }
    vka_barrier(cb);
    if (!when && getenv("FRITCHII_CS_PROBE_CLEAR")) {   // the program's 3D images cleared to 0 before it runs: what is nonzero after is what it wrote
        for (int k = 0; k < n; k++) if (ims[k] && ims[k]->d.target == NVN_TEXTURE_TARGET_3D && ims[k]->f.kind == 0) {
            VkClearColorValue z = {{0}}; VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            vk.vkCmdClearColorImage(cb, ims[k]->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg); }
        vka_barrier(cb);
    }
}
// Fritchii, FRITCHII_PASS_TRACE=<dir> with FRITCHII_PASS_TRACE_FS=<fragment shader key>: <dir>/trace.now arms it; the next FRITCHII_PASS_TRACE_N passes
// (default 120) drawn with that pixel shader have their first target's centre (1280x720, or its middle half) copied into one preallocated host buffer, without a wait,
// then written as <dir>/p<n>.raw and info.txt (1 s later). Each frame's final image at the game's own rate, cheap enough not to change its timing (the pass dump stalls)
static struct { int on, n, want, w, h, bpp, x, y; unsigned tick; uint64_t fs; VkBuffer buf; uint8_t *ptr; VkDeviceSize each; char dir[600]; } g_tr;
static void *trace_write(void *a) {
    (void)a; struct timespec ts = {1, 0}; nanosleep(&ts, NULL);
    for (int i = 0; i < g_tr.n; i++) { char path[700]; snprintf(path, sizeof path, "%s/p%03d.raw", g_tr.dir, i); FILE *f = fopen(path, "wb"); if (f) { fwrite(g_tr.ptr + (size_t)i * g_tr.each, 1, (size_t)g_tr.each, f); fclose(f); } }
    char path[700]; snprintf(path, sizeof path, "%s/info.txt", g_tr.dir); FILE *f = fopen(path, "w"); if (f) { fprintf(f, "%d %d %d %d\n", g_tr.n, g_tr.w, g_tr.h, g_tr.bpp); fclose(f); }
    VLOG("pass trace: %d frames of %dx%d written to %s", g_tr.n, g_tr.w, g_tr.h, g_tr.dir);
    g_tr.on = 0; return NULL;
}
static void pass_trace(VkCommandBuffer cb, vka_img *im) {
    static int init; if (!init) { init = 1; const char *e = getenv("FRITCHII_PASS_TRACE"), *k = getenv("FRITCHII_PASS_TRACE_FS");
        if (e && *e && k) { snprintf(g_tr.dir, sizeof g_tr.dir, "%s", e); g_tr.fs = strtoull(k, NULL, 16); g_tr.want = (int)VKA_ENV("FRITCHII_PASS_TRACE_N", 120); } }
    if (!g_tr.dir[0] || !im || im->samples != 1 || im->f.kind != 0 || im->f.bw != 1 || im->w < (uint32_t)VKA_ENV("FRITCHII_NANSCAN_MIN_W", 0)) return;
    int hit = 0; for (uint32_t k = 0; k < g_pn && !hit; k++) hit = g_pfs[k] == g_tr.fs;
    if (!hit) return;
    if (!g_tr.on) {
        if (g_tr.tick++ % 30) return;
        char path[700]; snprintf(path, sizeof path, "%s/trace.now", g_tr.dir); FILE *f = fopen(path, "rb"); if (!f) return; fclose(f); remove(path);
        g_tr.bpp = (int)im->f.bpb;
        g_tr.w = im->w >= 2560 ? 1280 : (int)im->w / 2; g_tr.h = im->h >= 1440 ? 720 : (int)im->h / 2; g_tr.x = ((int)im->w - g_tr.w) / 2; g_tr.y = ((int)im->h - g_tr.h) / 2;
        g_tr.each = (VkDeviceSize)g_tr.w * g_tr.h * g_tr.bpp;
        if (!g_tr.buf) {
            VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = g_tr.each * (VkDeviceSize)g_tr.want, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
            VkDeviceMemory mem = VK_NULL_HANDLE; void *p = NULL;
            if (vk.vkCreateBuffer(V.dev, &bi, NULL, &g_tr.buf)) { g_tr.dir[0] = 0; return; }
            VkMemoryRequirements mr; vk.vkGetBufferMemoryRequirements(V.dev, g_tr.buf, &mr);
            uint32_t t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
            if (t == UINT32_MAX) t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
            VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
            if (t == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &mem) || vk.vkBindBufferMemory(V.dev, g_tr.buf, mem, 0) || vk.vkMapMemory(V.dev, mem, 0, VK_WHOLE_SIZE, 0, &p)) {
                VLOG("pass trace: no host buffer of %llu MiB", (unsigned long long)(g_tr.each * (VkDeviceSize)g_tr.want >> 20)); g_tr.dir[0] = 0; return; }
            g_tr.ptr = p;
        }
        g_tr.n = 0; g_tr.on = 1; VLOG("pass trace: the next %d passes of %016llx (%dx%d of %ux%u, VkFormat %d)", g_tr.want, (unsigned long long)g_tr.fs, g_tr.w, g_tr.h, im->w, im->h, im->f.vk);
    }
    if (g_tr.on != 1 || g_tr.n >= g_tr.want) return;
    vka_barrier(cb);
    VkBufferImageCopy r = {g_tr.each * (VkDeviceSize)g_tr.n, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {g_tr.x, g_tr.y, 0}, {(uint32_t)g_tr.w, (uint32_t)g_tr.h, 1}};
    vk.vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, g_tr.buf, 1, &r);
    if (++g_tr.n == g_tr.want) { g_tr.on = 2; pthread_t t; if (!pthread_create(&t, NULL, trace_write, NULL)) pthread_detach(t); }
}
static void dump_pass(VkCommandBuffer cb, vka_img *const *ims, const uint32_t *lvs, const uint32_t *l0s, int n, uint32_t pass) {
    static int on = -1; static uint32_t tick;
    if (n > 0) pass_trace(cb, ims[0]);
    if (on < 0) { const char *d = getenv("DM_VK_PASS_DUMP"); on = d && *d; if (on) snprintf(g_dump_dir, sizeof g_dump_dir, "%s", d); }
    if (!on) return;
    const uint64_t fr = vka_frame();
    if (g_dump_frame == ~0ull && !(tick++ & 63)) {
        char path[600]; snprintf(path, sizeof path, "%s/capture.now", g_dump_dir);
        FILE *f = fopen(path, "rb"); if (f) { fclose(f); remove(path); g_dump_frame = fr + 1; VLOG("pass dump: frame %llu is captured into %s", (unsigned long long)g_dump_frame, g_dump_dir); }
    }
    static int nfr = -1; if (nfr < 0) { const char *e = getenv("DM_VK_PASS_DUMP_FRAMES"); nfr = e && atoi(e) > 1 ? atoi(e) : 1; g_dump_nfr = nfr; }   // Fritchii: that many frames from the capture frame on (names start f<n>_)
    if (fr < g_dump_frame || fr >= g_dump_frame + (uint64_t)nfr) return;
    static int only = -2; if (only == -2) { const char *e = getenv("DM_VK_PASS_DUMP_PASS"); only = e ? atoi(e) : -1; }   // DM_VK_PASS_DUMP_PASS=<n>: that pass only (with DM_VK_NANSCAN=2: after each of its draws)
    if (only >= 0 && (int)pass != only) return;
    static const char *fsl; static int fi; if (!fi) { fsl = getenv("DM_VK_PASS_DUMP_FS"); fi = 1; }   // DM_VK_PASS_DUMP_FS=<keys>: only segments drawn with one of those fragment shaders
    if (fsl) { int hit = 0; for (uint32_t k = 0; k < g_pn && !hit; k++) { char kx[17]; snprintf(kx, sizeof kx, "%016llx", (unsigned long long)g_pfs[k]); hit = strstr(fsl, kx) != NULL; } if (!hit) return; }
    vka_barrier(cb);
    for (int i = 0; i < n; i++) {
        vka_img *im = ims[i];
        if (!im || im->samples > 1 || im->d.target == NVN_TEXTURE_TARGET_3D || im->f.kind >= 3 || im->f.bw != 1 || im->f.bh != 1) continue;
        if ((only >= 0 || fsl) && i > 0) continue;   // (the first attachment only: the HDR target)
        uint32_t lv = lvs[i], w = im->w >> lv ? im->w >> lv : 1, h = im->h >> lv ? im->h >> lv : 1;
        VkDeviceSize size = (VkDeviceSize)w * h * im->f.bpb;
        dump_ent *e = calloc(1, sizeof *e); if (!e) return;
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VkMemoryRequirements mr; if (vk.vkCreateBuffer(V.dev, &bi, NULL, &e->buf)) { free(e); continue; }
        vk.vkGetBufferMemoryRequirements(V.dev, e->buf, &mr);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                   .memoryTypeIndex = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
        if (ai.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &e->mem) || vk.vkBindBufferMemory(V.dev, e->buf, e->mem, 0)) { vk.vkDestroyBuffer(V.dev, e->buf, NULL); free(e); continue; }
        VkBufferImageCopy c = {0, 0, 0, {im->aspect, lv, l0s[i], 1}, {0, 0, 0}, {w, h, 1}};
        vk.vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, e->buf, 1, &c);
        e->size = size;
        snprintf(e->name, sizeof e->name, "f%llu_%04u_p%03u_d%04u_s%llu_%ux%u_vk%d", (unsigned long long)(fr - g_dump_frame), g_dump_n++, pass, g_pdraws, (unsigned long long)im->serial, w, h, (int)im->f.vk);
        uint64_t nvn_direct_guest_of(uint64_t); size_t o = (size_t)snprintf(e->keys, sizeof e->keys, " guest %#llx", (unsigned long long)nvn_direct_guest_of((uint64_t)(uintptr_t)im->mem)); for (uint32_t k = 0; k < g_pn && o < sizeof e->keys - 40; k++) o += (size_t)snprintf(e->keys + o, sizeof e->keys - o, " %016llx/%016llx", (unsigned long long)g_pvs[k], (unsigned long long)g_pfs[k]);
        e->next = g_dump_list; g_dump_list = e;
    }
    vka_barrier(cb);
}
void vka_nan_pass(VkCommandBuffer cb, vka_img *const *ims, const uint32_t *lvs, const uint32_t *l0s, int n) {
    dump_pass(cb, ims, lvs, l0s, n, g_split ? g_pass : g_pass);
    batch_t *b = &g_b[g_bi % NBATCH];
    int split = g_split; g_split = 0;
    uint32_t pass = split ? g_pass : g_pass++;
    if (!ready()) return;
    if (!g_astc_state) { g_astc_state = -1; astc_check(cb); }
    int any = 0;
    for (int i = 0; i < n; i++) {
        vka_img *im = ims[i];
        if (!im || im->f.kind != 0 || im->samples > 1 || im->d.target == NVN_TEXTURE_TARGET_3D || !(im->usage & VK_IMAGE_USAGE_SAMPLED_BIT) || b->n == MAXR) continue;
        any |= scan_one(cb, b, im, lvs[i], l0s[i], pass, -1, 0);
    }
    if (any) vka_barrier(cb);
    g_pn = 0;
    if (split) g_d0 = g_pdraws; else { g_pdraws = 0; g_d0 = 0; }
}
static void report(void *arg) {
    batch_t *b = arg;
    for (int k = 0; k < b->n; k++) {
        const res_t *r = &b->res[k]; const rec_t *c = &b->r[k];
        if (!r->nan && !r->inf) continue;
        if (c->bind >= 0) {
            if (atomic_fetch_add(&g_lines, 1) >= 200) return;
            VLOG("NaN scan: frame %llu pass %u (fragment %016llx): input binding %d texture ID %d serial %llu (format %#x %ux%u level %u layer %u): %u NaN + %u infinite texels, first at (%u, %u)",
                 (unsigned long long)c->frame, c->pass, (unsigned long long)g_probe, c->bind, c->tid, (unsigned long long)c->serial, c->fmt, c->w, c->h, c->lv, c->layer, r->nan, r->inf, r->first & 0xffff, r->first >> 16);
            continue;
        }
        if (vka_nan_on() >= 2) {   // only where the count of this attachment grew since its previous scan in this submission
            int j = k - 1; while (j >= 0 && (b->r[j].serial != c->serial || b->r[j].lv != c->lv || b->r[j].layer != c->layer)) j--;
            if (j >= 0 && b->res[j].nan + b->res[j].inf >= r->nan + r->inf) continue;
        }
        if (atomic_fetch_add(&g_lines, 1) >= 200) return;
        char keys[400] = ""; size_t o = 0;
        for (uint32_t i = 0; i < c->nkeys && o < sizeof keys - 40; i++) o += (size_t)snprintf(keys + o, sizeof keys - o, " %016llx/%016llx", (unsigned long long)c->vs[i], (unsigned long long)c->fs[i]);
        VLOG("NaN scan: frame %llu pass %u: %u NaN + %u infinite texels in serial %llu (format %#x %ux%u level %u layer %u), first at (%u, %u); draws %u-%u of the pass, vertex/fragment%s%s",
             (unsigned long long)c->frame, c->pass, r->nan, r->inf, (unsigned long long)c->serial, c->fmt, c->w, c->h, c->lv, c->layer, r->first & 0xffff, r->first >> 16, c->d0, c->ndraws, keys, c->nkeys == NKEY ? " …" : "");
    }
}
void vka_nan_commit(uint64_t fence) {   // the submission's batch is read when fence is done; the next submission starts a new batch
    if (g_dump_list) { vka_after(fence, dump_write, g_dump_list); g_dump_list = NULL; }
    batch_t *b = &g_b[g_bi % NBATCH];
    if (b->n) vka_after(fence, report, b);
    if (g_astc_state == 1) { g_astc_state = 2; vka_after(fence, astc_report, NULL); }
    g_bi++; g_b[g_bi % NBATCH].n = 0; g_pass = 0; g_nsplit = 0;
}

// Lichii: the teardown (vka_dev.c vka_destroy)
void vka_nan_destroy(void) {
    VkPipeline p[3] = {g_pipe, g_pipe3, g_fetch};
    for (int i = 0; i < 3; i++) if (p[i]) vk.vkDestroyPipeline(V.dev, p[i], NULL);
    if (g_lay) vk.vkDestroyPipelineLayout(V.dev, g_lay, NULL);
    if (g_dsl) vk.vkDestroyDescriptorSetLayout(V.dev, g_dsl, NULL);
}
#endif
