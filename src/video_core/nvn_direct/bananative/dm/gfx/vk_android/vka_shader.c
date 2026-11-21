// From Banana Native (github.com/hugorosario/BananaNative, Android port of github.com/seedds/nxnative by seedds), MIT: see LICENSE in src/video_core/nvn_direct/bananative.
// Shaders of the Vulkan replay: library entries (android/SHADERS.md) as VkShaderModules, and NVN programs resolved to one shader per stage. Modules are made at a
// program's first draw (the submit thread), not at nvnProgramSetShaders.
// Lichii: upstream's library file (the pack's shaders/v-<DMSH_VK_CHAIN>/dmsh_vk.lib) is not loaded, and so neither is its subgroup plan. Every shader comes from Lichii's recompiler (nvn_recompile.c), each translation wrapped as a one-entry library in memory, so the
// library's ABI, modules and binding records below serve it unchanged
#if defined(__ANDROID__)
#include "vka.h"
#include "../nvn/nvn_mtl.h"
#include "../nvn/dmsh_chain.h"
#include "../nvn/dmsh_chain_vk.h"
#include "vka_spvpatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

// ---------- Static memory: buffers that live as long as the process ----------
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *ptr; VkDeviceSize size, used; } stblk;
static stblk g_st;
static stblk *g_st_old; static int g_nst_old;   // Lichii: the blocks before g_st, for the teardown (vka_shader_destroy)
int vka_static(VkDeviceSize n, VkDeviceSize align, vka_span *out) {
    if (!align) align = 16;
    pthread_mutex_lock(&g_mu);
    VkDeviceSize o = (g_st.used + align - 1) / align * align;
    if (!g_st.buf || o + n > g_st.size) {
        stblk b = {0}; b.size = n > (4u << 20) ? (n + 0xFFFF) & ~(VkDeviceSize)0xFFFF : 4u << 20;
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = b.size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
        VkMemoryRequirements mr; void *p = NULL; uint32_t t = UINT32_MAX;
        if (!vk.vkCreateBuffer(V.dev, &bi, NULL, &b.buf)) {
            vk.vkGetBufferMemoryRequirements(V.dev, b.buf, &mr);
            t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
            VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
            if (t == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &b.mem) || vk.vkBindBufferMemory(V.dev, b.buf, b.mem, 0) || vk.vkMapMemory(V.dev, b.mem, 0, VK_WHOLE_SIZE, 0, &p)) p = NULL;
        }
        if (!p) { pthread_mutex_unlock(&g_mu); VRED("static block of %llu bytes not allocated", (unsigned long long)b.size); return 0; }
        if (g_st.buf) { stblk *l = realloc(g_st_old, (size_t)(g_nst_old + 1) * sizeof *l); if (l) { g_st_old = l; g_st_old[g_nst_old++] = g_st; } }
        b.ptr = p; g_st = b; o = 0;   // the old block stays (its buffers are still bound by shaders)
    }
    g_st.used = o + n; *out = (vka_span){g_st.buf, g_st.ptr + o, o};
    pthread_mutex_unlock(&g_mu);
    return 1;
}

// ---------- Shaders by key ----------
typedef struct { uint64_t key; vka_shader *sh; const vka_shlib *lib; } shent;
static shent *g_sh; static size_t g_shcap, g_shn;
static size_t hslot(uint64_t k, size_t cap) { return (size_t)((k * 0x9E3779B97F4A7C15ull) >> 17) & (cap - 1); }
static shent *sh_find(uint64_t key, const vka_shlib *L) {
    if (!g_shcap) return NULL;
    for (size_t i = hslot(key, g_shcap);; i = (i + 1) & (g_shcap - 1)) { if (!g_sh[i].sh) return NULL; if (g_sh[i].key == key && g_sh[i].lib == L) return &g_sh[i]; }
}
static void sh_put(uint64_t key, const vka_shlib *L, vka_shader *s) {
    if (2 * (g_shn + 1) > g_shcap) {
        size_t nc = g_shcap ? 2 * g_shcap : 1024; shent *n = calloc(nc, sizeof *n);
        for (size_t i = 0; i < g_shcap; i++) if (g_sh[i].sh) { size_t j = hslot(g_sh[i].key, nc); while (n[j].sh) j = (j + 1) & (nc - 1); n[j] = g_sh[i]; }
        free(g_sh); g_sh = n; g_shcap = nc;
    }
    size_t j = hslot(key, g_shcap); while (g_sh[j].sh) j = (j + 1) & (g_shcap - 1);
    g_sh[j] = (shent){key, s, L}; g_shn++;
}

static const char *stage_name(int s) { static const char *n[] = {"vertex", "tess control", "tess evaluation", "geometry", "fragment", "compute", "task", "mesh"}; return s >= 0 && s < 8 ? n[s] : "?"; }
// What the replay cannot run yet, decided once per shader: refused shaders keep their entry, so the draws that use them are skipped and counted
static const char *refusal(const vka_shlib *L, const dmsh_vk_entry *e) {
    if (e->flags & (DMSH_VK_F_NV_PASSTHROUGH | DMSH_VK_F_NV_VIEWPORT)) return "NV-only geometry passthrough / viewport mask (a library built before the gs_passthrough answer)";
    if (e->flags & DMSH_VK_F_INTERLOCK) return "fragment shader interlock";
    if (e->stage == DMSH_VK_STAGE_GS && !V.geom) return "a geometry shader on a device without geometryShader";
    if ((e->stage == DMSH_VK_STAGE_TCS || e->stage == DMSH_VK_STAGE_TES) && !V.tess) return "a tessellation shader on a device without tessellationShader";
    // NVN2 task and mesh programs, and the pixel shaders that read their per-primitive outputs (valid only in a mesh pipeline): only with VK_EXT_mesh_shader (vka_dev.c)
    if ((e->stage == DMSH_VK_STAGE_TS || e->stage == DMSH_VK_STAGE_MS || (e->flags & DMSH_VK_F_MESH_PRIM)) && !V.mesh) return "a task / mesh shader (or a pixel shader of their per-primitive outputs) on a device without VK_EXT_mesh_shader (or DM_VK_MESH=0)";
    if (e->stage == DMSH_VK_STAGE_TS && !V.task) return "a task shader on a device with mesh shaders but without taskShader";
    if (e->stage == DMSH_VK_STAGE_TS || e->stage == DMSH_VK_STAGE_MS) {   // the workgroup (the entry's local) within the device's limit
        uint64_t inv = (uint64_t)e->local[0] * e->local[1] * e->local[2];
        if (!inv || inv > (e->stage == DMSH_VK_STAGE_TS ? V.task_inv : V.mesh_inv)) return "a task / mesh workgroup larger than the device's (maxTaskWorkGroupInvocations / maxMeshWorkGroupInvocations)";
    }
    const dmsh_vk_bind *b = vka_shlib_binds(L, e);
    for (unsigned i = 0; i < e->bind_count; i++) if (b[i].kind == DMSH_VK_KIND_UNKNOWN) return "a binding the build could not place";
    return NULL;
}
// Lichii: a pixel shader's set-0 resources in their own set (VKA_SET_FS), in every module made of it
static void fs_set(const dmsh_vk_entry *e, uint32_t *w, size_t nb) { if (e->stage == DMSH_VK_STAGE_FS) vka_spv_set_remap(w, nb / 4, DMSH_VK_SET_DRAW, VKA_SET_FS); }
static _Thread_local size_t t_last_bytes;   // the SPIR-V size of this thread's last spv_module
static VkShaderModule spv_module(const vka_shlib *L, const dmsh_vk_entry *e, const dmsh_vk_module *m, uint64_t key, const char **why, char *err, size_t errn) {
    size_t nb = 0; uint32_t *w = vka_shlib_spirv(L, m, &nb, err, errn);
    if (!w) { *why = err; return VK_NULL_HANDLE; }
    t_last_bytes = nb;
    fs_set(e, w, nb);
    {   // DM_VK_SPV_DUMP=<dir>: each module as <key>.<variant>.spv
        const char *dir = getenv("DM_VK_SPV_DUMP");
        if (dir && *dir) {
            char path[1024]; snprintf(path, sizeof path, "%s/%016llx.%u.spv", dir, (unsigned long long)key, m->variant);
            FILE *f = fopen(path, "wb"); if (f) { fwrite(w, 1, nb, f); fclose(f); }
        }
    }
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nb, .pCode = w};
    VkShaderModule mod = VK_NULL_HANDLE; VkResult r = vk.vkCreateShaderModule(V.dev, &ci, NULL, &mod);
    free(w);
    if (r) { snprintf(err, errn, "vkCreateShaderModule %d (variant %u of %016llx)", r, m->variant, (unsigned long long)key); *why = err; return VK_NULL_HANDLE; }
    return mod;
}
VkShaderModule vka_shader_module(vka_shader *s, int variant) {
    if (variant < 0 || variant > 3) return VK_NULL_HANDLE;
    if (s->mods[variant]) return s->mods[variant];
    const dmsh_vk_module *m = vka_shlib_module(s->lib, s->e, variant);
    if (!m) return VK_NULL_HANDLE;
    const char *why = NULL; char err[256];
    VkShaderModule mod = spv_module(s->lib, s->e, m, s->key, &why, err, sizeof err);
    if (!mod) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("shader %016llx variant %d: %s", (unsigned long long)s->key, variant, why); return VK_NULL_HANDLE; }
    pthread_mutex_lock(&g_mu);
    if (s->mods[variant]) { pthread_mutex_unlock(&g_mu); vk.vkDestroyShaderModule(V.dev, mod, NULL); return s->mods[variant]; }
    s->mods[variant] = mod;
    pthread_mutex_unlock(&g_mu);
    return mod;
}
// A geometry shader that writes memory, with those writes made by one run per primitive (vka_spv_gs_claim: the Adreno 840 runs a geometry shader several times for each
// primitive, each run writing memory). out NULL: only counts them. Made from the base module after the one-invocation subgroup rewrite
static int gs_words(vka_shader *s, uint32_t **out, size_t *on, char *err, size_t errn) {
    const dmsh_vk_module *m = vka_shlib_module(s->lib, s->e, DMSH_VK_MOD_BASE);
    size_t nb = 0; uint32_t *w = m ? vka_shlib_spirv(s->lib, m, &nb, err, errn) : NULL;
    if (!w) return -1;
    int r = vka_spv_gs_claim(w, nb / 4, VKA_GS_CLAIM_BINDING, out, on, err, errn);
    free(w); return r;
}
VkShaderModule vka_shader_gs_module(vka_shader *s) {
    if (s->gs_fx <= 0) return VK_NULL_HANDLE;
    if (s->gs_claim) return s->gs_claim;
    uint32_t *o = NULL; size_t on = 0; char err[256] = ""; VkShaderModule mod = VK_NULL_HANDLE;
    if (gs_words(s, &o, &on, err, sizeof err) > 0 && o) {
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = on * 4, .pCode = o};
        VkResult r = vk.vkCreateShaderModule(V.dev, &ci, NULL, &mod); if (r) { snprintf(err, sizeof err, "vkCreateShaderModule %d", r); mod = VK_NULL_HANDLE; }
    }
    free(o);
    if (!mod) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("geometry shader %016llx: module with the claim not made: %s", (unsigned long long)s->key, err); return VK_NULL_HANDLE; }
    pthread_mutex_lock(&g_mu);
    if (s->gs_claim) { pthread_mutex_unlock(&g_mu); vk.vkDestroyShaderModule(V.dev, mod, NULL); return s->gs_claim; }
    s->gs_claim = mod;
    pthread_mutex_unlock(&g_mu);
    return mod;
}
// The base vertex module with the inputs of umask / smask retyped (vka_spvpatch.c): made once per pair of masks, kept for the process like the base module
struct vka_vinmod { struct vka_vinmod *next; uint32_t u, s; VkShaderModule mod; };
VkShaderModule vka_shader_vin_module(vka_shader *s, uint32_t umask, uint32_t smask) {
    pthread_mutex_lock(&g_mu);
    for (struct vka_vinmod *v = s->vin; v; v = v->next) if (v->u == umask && v->s == smask) { VkShaderModule m = v->mod; pthread_mutex_unlock(&g_mu); return m; }
    pthread_mutex_unlock(&g_mu);
    VkShaderModule mod = VK_NULL_HANDLE; char err[256] = ""; int own = 0;   // own: made here (the base module of the fallback is the shader's, never destroyed)
    const dmsh_vk_module *m = vka_shlib_module(s->lib, s->e, DMSH_VK_MOD_BASE);
    size_t nb = 0; uint32_t *w = m ? vka_shlib_spirv(s->lib, m, &nb, err, sizeof err) : NULL;
    size_t on = 0; uint32_t *o = w ? vka_spv_vin_retype(w, nb / 4, umask, smask, &on, err, sizeof err) : NULL;
    if (o) { VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = on * 4, .pCode = o};
             VkResult r = vk.vkCreateShaderModule(V.dev, &ci, NULL, &mod); if (r) { snprintf(err, sizeof err, "vkCreateShaderModule %d", r); mod = VK_NULL_HANDLE; } else own = 1; }
    free(w); free(o);
    if (!mod) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("vertex shader %016llx: inputs %#x (uint) / %#x (int) not retyped: %s; the base module reads them as floats", (unsigned long long)s->key, umask, smask, err);
                mod = vka_shader_module(s, DMSH_VK_MOD_BASE); }
    // Two threads (the frame path and a prewarm or optimizer thread) may have made it at once: the first one in the list wins, the other's module is destroyed
    pthread_mutex_lock(&g_mu);
    for (struct vka_vinmod *o = s->vin; o; o = o->next) if (o->u == umask && o->s == smask) {
        VkShaderModule w = o->mod; pthread_mutex_unlock(&g_mu);
        if (own && mod != w) vk.vkDestroyShaderModule(V.dev, mod, NULL);   // (no pipeline was made from it: it never left this function)
        return w; }
    struct vka_vinmod *v = malloc(sizeof *v); *v = (struct vka_vinmod){s->vin, umask, smask, mod}; s->vin = v;
    pthread_mutex_unlock(&g_mu);
    return mod;
}
// Fritchii: the base module's Input / Output locations, read once
uint32_t vka_shader_io(vka_shader *s, int out) {
    pthread_mutex_lock(&g_mu); int known = s->io_known; uint32_t r = out ? s->io_out : s->io_in; pthread_mutex_unlock(&g_mu);
    if (known) return r;
    char err[256] = ""; const dmsh_vk_module *m = vka_shlib_module(s->lib, s->e, DMSH_VK_MOD_BASE);
    size_t nb = 0; uint32_t *w = m ? vka_shlib_spirv(s->lib, m, &nb, err, sizeof err) : NULL;
    uint32_t in = w ? vka_spv_io_mask(w, nb / 4, 1) : 0, ou = w ? vka_spv_io_mask(w, nb / 4, 3) : ~0u;
    free(w);
    pthread_mutex_lock(&g_mu); s->io_in = in; s->io_out = ou; s->io_known = 1; pthread_mutex_unlock(&g_mu);
    return out ? ou : in;
}
// Fritchii: the base vertex module with (0, 0, 0, 1) outputs at the locations of mask, which a pixel shader it is drawn with reads and it never writes (vka_pso.c pstate_fill)
VkShaderModule vka_shader_defout_module(vka_shader *s, uint32_t mask) {
    pthread_mutex_lock(&g_mu);
    for (struct vka_vinmod *v = s->defout; v; v = v->next) if (v->u == mask) { VkShaderModule m = v->mod; pthread_mutex_unlock(&g_mu); return m; }
    pthread_mutex_unlock(&g_mu);
    VkShaderModule mod = VK_NULL_HANDLE; char err[256] = ""; int own = 0;
    const dmsh_vk_module *m = vka_shlib_module(s->lib, s->e, DMSH_VK_MOD_BASE);
    size_t nb = 0; uint32_t *w = m ? vka_shlib_spirv(s->lib, m, &nb, err, sizeof err) : NULL;
    size_t on = 0; uint32_t *o = w ? vka_spv_add_default_outputs(w, nb / 4, mask, &on, err, sizeof err) : NULL;
    if (o) { VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = on * 4, .pCode = o};
             VkResult r = vk.vkCreateShaderModule(V.dev, &ci, NULL, &mod); if (r) { snprintf(err, sizeof err, "vkCreateShaderModule %d", r); mod = VK_NULL_HANDLE; } else own = 1; }
    free(w); free(o);
    if (!mod) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("vertex shader %016llx: default outputs %#x not added: %s; the pixel shader reads them undefined", (unsigned long long)s->key, mask, err);
                mod = vka_shader_module(s, DMSH_VK_MOD_BASE); }
    else { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16) VLOG("vertex shader %016llx: outputs %#x its pixel shader reads and it does not write added as (0, 0, 0, 1)", (unsigned long long)s->key, mask); }
    pthread_mutex_lock(&g_mu);
    for (struct vka_vinmod *x = s->defout; x; x = x->next) if (x->u == mask) {
        VkShaderModule w2 = x->mod; pthread_mutex_unlock(&g_mu);
        if (own && mod != w2) vk.vkDestroyShaderModule(V.dev, mod, NULL);
        return w2; }
    struct vka_vinmod *v = malloc(sizeof *v); *v = (struct vka_vinmod){s->defout, mask, 0, mod}; s->defout = v;
    pthread_mutex_unlock(&g_mu);
    return mod;
}
// The base pixel module with the float outputs of umask / smask retyped for integer attachments (vka_spvpatch.c vka_spv_fout_retype): made once per pair of masks, as the vertex ones
VkShaderModule vka_shader_fout_module(vka_shader *s, uint32_t umask, uint32_t smask) {
    pthread_mutex_lock(&g_mu);
    for (struct vka_vinmod *v = s->fout; v; v = v->next) if (v->u == umask && v->s == smask) { VkShaderModule m = v->mod; pthread_mutex_unlock(&g_mu); return m; }
    pthread_mutex_unlock(&g_mu);
    VkShaderModule mod = VK_NULL_HANDLE; char err[256] = ""; int own = 0;
    const dmsh_vk_module *m = vka_shlib_module(s->lib, s->e, DMSH_VK_MOD_BASE);
    size_t nb = 0; uint32_t *w = m ? vka_shlib_spirv(s->lib, m, &nb, err, sizeof err) : NULL;
    size_t on = 0; uint32_t *o = w ? vka_spv_fout_retype(w, nb / 4, umask, smask, &on, err, sizeof err) : NULL;
    if (o) fs_set(s->e, o, on * 4);
    if (o) { VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = on * 4, .pCode = o};
             VkResult r = vk.vkCreateShaderModule(V.dev, &ci, NULL, &mod); if (r) { snprintf(err, sizeof err, "vkCreateShaderModule %d", r); mod = VK_NULL_HANDLE; } else own = 1; }
    free(w); free(o);
    if (!mod) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("pixel shader %016llx: outputs %#x (uint) / %#x (int) not retyped for their integer targets: %s; the base module, whose float outputs those targets do not take", (unsigned long long)s->key, umask, smask, err);
                mod = vka_shader_module(s, DMSH_VK_MOD_BASE); }
    else { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VLOG("pixel shader %016llx: float outputs %#x (uint) / %#x (int) retyped for their integer targets (their bits written as they are)", (unsigned long long)s->key, umask, smask); }
    pthread_mutex_lock(&g_mu);
    for (struct vka_vinmod *x = s->fout; x; x = x->next) if (x->u == umask && x->s == smask) {
        VkShaderModule had = x->mod; pthread_mutex_unlock(&g_mu);
        if (own && mod != had) vk.vkDestroyShaderModule(V.dev, mod, NULL);
        return had; }
    struct vka_vinmod *v = malloc(sizeof *v);
    if (v) { *v = (struct vka_vinmod){s->fout, umask, smask, mod}; s->fout = v; }
    pthread_mutex_unlock(&g_mu);
    return mod;
}
int vka_shader_fout_retyped(vka_shader *s, uint32_t umask, uint32_t smask) {
    VkShaderModule m = vka_shader_fout_module(s, umask, smask);
    return m && m != vka_shader_module(s, DMSH_VK_MOD_BASE);
}
// Lichii: a shader of library L made only for a stage library built for the caches (nvn_recompile.c's speculative translations): not in the shader
// table, its base module only, no c1 bytes; given back with vka_shader_scratch_free. NULL: refused
vka_shader *vka_shader_scratch(const vka_shlib *L, uint64_t key) {
    const dmsh_vk_entry *e = vka_shlib_find(L, key);
    const dmsh_vk_module *m = e && !refusal(L, e) ? vka_shlib_module(L, e, DMSH_VK_MOD_BASE) : NULL;
    if (!m) return NULL;
    vka_shader *s = calloc(1, sizeof *s); if (!s) return NULL;
    s->key = key; s->lib = L; s->e = e; s->b = vka_shlib_binds(L, e);
    const char *why = NULL; char err[256] = "";
    s->mods[DMSH_VK_MOD_BASE] = spv_module(L, e, m, key, &why, err, sizeof err);
    if (!s->mods[DMSH_VK_MOD_BASE]) { free(s); return NULL; }
    s->base_words = (uint32_t)(t_last_bytes / 4);
    return s;
}
void vka_shader_scratch_free(vka_shader *s) {
    if (!s) return;
    for (int i = 0; i < 4; i++) if (s->mods[i]) vk.vkDestroyShaderModule(V.dev, s->mods[i], NULL);
    for (struct vka_vinmod *l[2] = {s->vin, s->fout}, **p = l; p < l + 2; p++)
        for (struct vka_vinmod *v = *p, *n; v; v = n) { n = v->next; if (v->mod != s->mods[DMSH_VK_MOD_BASE]) vk.vkDestroyShaderModule(V.dev, v->mod, NULL); free(v); }
    free(s);
}
vka_shader *vka_shader_make(const vka_shlib *L, uint64_t key) {
    pthread_mutex_lock(&g_mu);
    shent *h = sh_find(key, L);
    if (h) { vka_shader *s = h->sh; pthread_mutex_unlock(&g_mu); return s->bad ? NULL : s; }
    pthread_mutex_unlock(&g_mu);
    vka_shader *s = calloc(1, sizeof *s); s->key = key; s->lib = L;
    const dmsh_vk_entry *e = vka_shlib_find(L, key);
    const char *why = NULL; char err[256] = "";
    if (!e) why = "not in the shader library";
    else {
        s->e = e; s->b = vka_shlib_binds(L, e);
        why = refusal(L, e);
        const dmsh_vk_module *m = why ? NULL : vka_shlib_module(L, e, DMSH_VK_MOD_BASE);
        if (!why && !m) why = "no base module";
        if (m) { s->mods[DMSH_VK_MOD_BASE] = spv_module(L, e, m, key, &why, err, sizeof err); s->base_words = (uint32_t)(t_last_bytes / 4); }
        if (!why && e->stage == DMSH_VK_STAGE_GS && VKA_ENV("DM_VK_GS_CLAIM", 1)) {   // memory writes: made once per primitive (vka_spv_gs_claim)
            char e2[256] = ""; s->gs_fx = gs_words(s, NULL, NULL, e2, sizeof e2);
            if (s->gs_fx < 0) VRED("geometry shader %016llx: memory writes not put under a claim (%s): drawn as it is, its writes repeated by the device's runs of each primitive", (unsigned long long)key, e2);
            else if (s->gs_fx) VLOG("geometry shader %016llx: %d memory writes, made by one run per primitive (a claim word per primitive at set 0 binding %d)", (unsigned long long)key, s->gs_fx, VKA_GS_CLAIM_BINDING);
        }
        if (!why && e->c1_len) {   // the c1 bytes: a uniform buffer of their own, made once
            if (vka_static(e->c1_len, V.ub_align, &s->c1)) { memcpy(s->c1.ptr, vka_shlib_c1(L, e), e->c1_len); s->c1_len = e->c1_len; }
            else why = "no static memory for its c1 bytes";
        }
    }
    if (why) {
        s->bad = 1;
        static _Atomic int n; if (atomic_fetch_add(&n, 1) < 64) VRED("shader %016llx (%s): %s; its draws are skipped", (unsigned long long)key, e ? stage_name(e->stage) : "?", why);
    }
    pthread_mutex_lock(&g_mu);
    h = sh_find(key, L);
    if (h) { if (s->mods[0]) vk.vkDestroyShaderModule(V.dev, s->mods[0], NULL); free(s); s = h->sh; }   // another thread made it meanwhile
    else sh_put(key, L, s);
    pthread_mutex_unlock(&g_mu);
    return s->bad ? NULL : s;
}

// A shader by key (the prewarm threads, vka_pso.c): Lichii's recompiled shaders, those translated so far (nvn_recompile.c); NULL = not translated or refused
vka_shader *vka_shader_by_key(uint64_t key, int gl_depth) { return vka_rc_by_key(key, gl_depth); }

// Lichii: the keys program stages asked for that the recompiler could not place, each counted once and logged (the first 256) with how many there are so far
static pthread_mutex_t g_miss_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t *g_miss; static size_t g_miss_cap, g_miss_n;
static void missing_key(uint64_t key, unsigned stage) {
    pthread_mutex_lock(&g_miss_mu);
    if (2 * (g_miss_n + 1) > g_miss_cap) {
        size_t nc = g_miss_cap ? 2 * g_miss_cap : 256; uint64_t *n = calloc(nc, sizeof *n);
        for (size_t i = 0; i < g_miss_cap; i++) if (g_miss[i]) { size_t j = hslot(g_miss[i], nc); while (n[j]) j = (j + 1) & (nc - 1); n[j] = g_miss[i]; }
        free(g_miss); g_miss = n; g_miss_cap = nc;
    }
    uint64_t k = key ? key : 1;   // (0 marks an empty slot)
    size_t j = hslot(k, g_miss_cap); while (g_miss[j] && g_miss[j] != k) j = (j + 1) & (g_miss_cap - 1);
    int fresh = !g_miss[j]; if (fresh) { g_miss[j] = k; g_miss_n++; }
    size_t n = g_miss_n;
    pthread_mutex_unlock(&g_miss_mu);
    if (fresh && n <= 256) VRED("shader %016llx (program stage %u) has no control section kept for the recompiler: its draws are skipped (%zu keys missing so far)", (unsigned long long)key, stage, n);
}

// ---------- Programs ----------
// By serial (a new serial at every nvnProgramSetShaders; mtl_prog_release drops it): the program's stage keys resolved to shaders, each at the NVN stage its entry names
typedef struct { uint64_t serial; vka_progsh ps; } prent;
static prent *g_pr; static size_t g_prcap, g_prn;
static prent *pr_find(uint64_t s) {
    if (!g_prcap) return NULL;
    for (size_t i = hslot(s, g_prcap);; i = (i + 1) & (g_prcap - 1)) { if (!g_pr[i].serial) return NULL; if (g_pr[i].serial == s) return &g_pr[i]; }
}
static void pr_put(uint64_t s, const vka_progsh *ps) {
    prent *e = pr_find(s); if (e) { e->ps = *ps; return; }
    if (2 * (g_prn + 1) > g_prcap) {
        size_t nc = g_prcap ? 2 * g_prcap : 1024; prent *n = calloc(nc, sizeof *n);
        for (size_t i = 0; i < g_prcap; i++) if (g_pr[i].serial && g_pr[i].serial != ~0ull) { size_t j = hslot(g_pr[i].serial, nc); while (n[j].serial) j = (j + 1) & (nc - 1); n[j] = g_pr[i]; }
        free(g_pr); g_pr = n; g_prcap = nc; size_t live = 0; for (size_t i = 0; i < nc; i++) live += n[i].serial != 0; g_prn = live;
    }
    size_t j = hslot(s, g_prcap); while (g_pr[j].serial && g_pr[j].serial != ~0ull) j = (j + 1) & (g_prcap - 1);
    if (!g_pr[j].serial) g_prn++;
    g_pr[j] = (prent){s, *ps};
}
void vka_prog_put(uint64_t serial, const vka_progsh *ps) { pthread_mutex_lock(&g_mu); pr_put(serial, ps); pthread_mutex_unlock(&g_mu); }
void mtl_prog_release(prog_t *p) {
    if (!p || !p->serial) return;
    pthread_mutex_lock(&g_mu); prent *e = pr_find(p->serial); if (e) { e->serial = ~0ull; memset(&e->ps, 0, sizeof e->ps); } pthread_mutex_unlock(&g_mu);   // a tombstone
}
void mtl_prog_prefetch(const prog_t *p) { vka_rc_keep(p); }   // modules are made at the program's first draw (vka_prog_shaders); Lichii: the recompiler's copies of the control sections

int vka_progsh_current(const vka_progsh *ps) { return !ps->depth || ps->depth == 1 + !vka_depth_zero(); }
int vka_prog_shaders(const prog_t *p, vka_progsh *out) {
    memset(out, 0, sizeof *out);
    if (!p || !p->serial) return 0;
    pthread_mutex_lock(&g_mu); prent *e = pr_find(p->serial); int hit = e && vka_progsh_current(&e->ps); if (hit) *out = e->ps; pthread_mutex_unlock(&g_mu);
    if (hit) return 1;
    // Lichii: every stage from the recompiler (nvn_recompile.c), translated with the draw's state
    // The pixel stage last: drawn after the program's mesh shader, it reads that shader's per-primitive outputs as such (vka_rc_shader prim_inputs)
    // Lichii: the pre-rasterization stages in the depth mode now (resolved again for the program after a switch: the other mode's translations stay kept)
    uint32_t prim = 0; const int gl_depth = !vka_depth_zero(); out->depth = (uint8_t)(1 + gl_depth);
    for (int pass = 0; pass < 2; pass++)
        for (unsigned i = 0; i < p->nstage && i < 6; i++) {
            uint8_t md[8];
            if (!prog_key(p, i, md)) continue;
            uint64_t key = vka_shlib_key(md); int st = vka_rc_stage(key);
            if ((st == 1) != (pass == 1)) continue;
            vka_shader *sh = vka_rc_shader(p, i, key, st == 1 ? prim : 0, gl_depth, &st);
            if (st < 0 || st >= VKA_NSTAGE) { out->bad |= VKA_BAD_MISSING; missing_key(key, i); continue; }
            if (st == 7) prim = vka_rc_prim(key);
            out->sh[st] = sh;
            if (!sh) out->bad |= 1u << st;
        }
    vka_prog_put(p->serial, out);
    return 1;
}

const vka_mesh_fb *vka_prog_mesh_fb(const prog_t *p) {   // Fritchii (vka.h)
    if (!p || !p->serial) return NULL;
    const int gl_depth = !vka_depth_zero();
    for (unsigned i = 0; i < p->nstage && i < 6; i++) {
        uint8_t md[8];
        if (!prog_key(p, i, md)) continue;
        uint64_t key = vka_shlib_key(md);
        if (vka_rc_stage(key) == 7) return vka_rc_mesh_fb(p, i, key, gl_depth);
    }
    return NULL;
}
// Lichii: the teardown (vka_dev.c vka_destroy, after the pipelines): every shader's modules, the static blocks, the tables
void vka_shader_destroy(void) {
    for (size_t i = 0; i < g_shcap; i++) if (g_sh[i].sh) {
        vka_shader *s = g_sh[i].sh;
        if (s->gs_claim) vk.vkDestroyShaderModule(V.dev, s->gs_claim, NULL);
        vka_shader_scratch_free(s);
    }
    free(g_sh); free(g_miss);
    for (int i = 0; i <= g_nst_old; i++) {
        const stblk *b = i < g_nst_old ? &g_st_old[i] : &g_st;
        if (b->buf) { vk.vkDestroyBuffer(V.dev, b->buf, NULL); vk.vkFreeMemory(V.dev, b->mem, NULL); }
    }
    free(g_st_old);
}
#endif
