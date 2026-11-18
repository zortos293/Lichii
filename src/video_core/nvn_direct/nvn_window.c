// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVN windows in direct mode: what upstream's window block of hle_nvn.c did (left out there),
// without its pacing, frame statistics and present thread. A present hands the image to the
// replay's present queue (nvn_present.c); Lichii's hook shows it and does the present's
// bookkeeping and pacing (core/nvn_hook.cpp). The window and sync layouts are the front end's
// (nvn_obj.h).

#include <stdatomic.h>
#include <string.h>

#include "hle.h"
#include "nvn_protos.h"
#include "nvn_obj.h"
#include "nvn_capture.h"
#include "nvn_mtl.h"
#include "nvn_direct_front.h"

typedef struct { const char* name; void* fn; } nvn_impl;
// As hle_nvn.c's NVN(): an implementation with its signature checked against the prototype,
// listed in the front end's table (section swnvn).
#define NVN(name, ret, ...)                                                                        \
    static ret h_##name(__VA_ARGS__);                                                              \
    static const NVNT_##name chk_##name __attribute__((unused)) = h_##name;                        \
    PLAT_TABLE(swnvn) static const nvn_impl e_##name = {#name, (void*)h_##name};                   \
    static ret h_##name(__VA_ARGS__)
// NVN2's functions without a prototype (gen_dispatch.py reads their signatures here).
#define NVN_NOPROTO(name, ret, ...)                                                                \
    static ret h_##name(__VA_ARGS__);                                                              \
    PLAT_TABLE(swnvn) static const nvn_impl e_##name = {#name, (void*)h_##name};                   \
    static ret h_##name(__VA_ARGS__)

#define WB(b) ((win_builder_t*)(b))
#define WBHDR(b) ((win_hdr_t*)((uint8_t*)(b) + WIN_BUILDER_HDR))
#define CTX(t) ((const tex_t*)(t))

static _Atomic uint64_t g_frame;
uint64_t dm_nvn_frame_count(void) { return atomic_load(&g_frame); }

NVN(nvnWindowBuilderSetDefaults, void, NVNwindowBuilder* b) {
    NVNdevice* dev = WB(b)->dev;
    memset(b, 0, sizeof(win_builder_t));
    WB(b)->dev = dev;
    WB(b)->interval = 1;
    if (dm_sdk_ounce()) {
        static const win_hdr_t defaults = {{0.64f, 0.33f, 0.30f, 0.60f, 0.15f, 0.06f, 0.3127f, 0.3290f, 0.03f, 1000.f}, {200.f, 60.f}};
        *WBHDR(b) = defaults;
    }
}
NVN(nvnWindowBuilderSetDevice, void, NVNwindowBuilder* b, NVNdevice* d) { WB(b)->dev = d; }
NVN(nvnWindowBuilderSetNativeWindow, void, NVNwindowBuilder* b, NVNnativeWindow w) { WB(b)->native = w; }
// The builder keeps the game's array, as the driver does, and reads it at WindowInitialize and
// GetTexture: its entries are the game's addresses.
NVN_NOPROTO(nvnWindowBuilderSetTextures, void, NVNwindowBuilder* b, int n, const void* t) {
    if (n < 0 || n > 4) {
        LOG("[RED] WindowBuilderSetTextures count %d", n);
        n = n < 0 ? 0 : 4;
    }
    WB(b)->ntex = n;
    WB(b)->tex = t;
}
static NVNtexture* builder_texture(const win_builder_t* b, int i) {
    return (NVNtexture*)(uintptr_t)nvn_direct_host_of(((const uint64_t*)b->tex)[i]);
}
NVN(nvnWindowBuilderSetPresentInterval, void, NVNwindowBuilder* b, int v) { WB(b)->interval = v; }
NVN(nvnWindowInitialize, NVNboolean, NVNwindow* w, const NVNwindowBuilder* b) {
    const win_builder_t* s = (const win_builder_t*)b;
    win_t* o = (win_t*)w;
    memset(o, 0, sizeof *o);
    o->magic = MAGIC('W', 'I', 'N', 'D');
    o->native = s->native;
    o->ntex = s->ntex;
    o->interval = s->interval;
    o->active = s->active ? s->active : s->ntex;
    for (int i = 0; i < s->ntex; i++) o->tex[i] = builder_texture(s, i);
    if (dm_sdk_ounce()) o->hdr = *(const win_hdr_t*)((const uint8_t*)b + WIN_BUILDER_HDR);
    LOG("[nvn] WindowInitialize swap chain %d textures, interval %d, first %dx%d format %#x", o->ntex, o->interval,
        o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.width : 0, o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.height : 0,
        o->ntex && o->tex[0] ? CTX(o->tex[0])->d.format : 0);
    return 1;
}
NVN(nvnWindowGetPresentInterval, int, const NVNwindow* w) { return ((const win_t*)w)->interval; }
NVN(nvnWindowSetPresentInterval, void, NVNwindow* w, int v) {
    if (((win_t*)w)->interval != v) LOG("[nvn] the game sets present interval %d -> %d", ((win_t*)w)->interval, v);
    ((win_t*)w)->interval = v;
}
NVN(nvnWindowSetCrop, void, NVNwindow* w, int x, int y, int ww, int hh) {
    win_t* o = (win_t*)w;
    const int v[4] = {x, y, ww, hh};
    memcpy(o->crop, v, sizeof v);
}
// Before any SetCrop it is the whole first texture, as NVN starts it.
NVN(nvnWindowGetCrop, void, const NVNwindow* w, NVNrectangle* r) {
    const win_t* o = (const win_t*)w;
    if (o->crop[2] > 0 && o->crop[3] > 0) {
        r->x = o->crop[0]; r->y = o->crop[1]; r->width = o->crop[2]; r->height = o->crop[3];
        return;
    }
    r->x = r->y = 0;
    r->width = o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.width : 0;
    r->height = o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.height : 0;
}
// The images go round in order; an acquire's sync is reached when the window's last present is:
// its queue's last submission and every fence before it (mtl_wait_acq), as on the console, where
// the image is free once the frame after it is on screen. The last submission of
// any queue, as upstream's default mtl_acquire_fence, may be another queue's that waits for an
// event the game signals only after this acquire's wait (Mario Kart World's second queue: a GPU
// hang). Only the presenting queue's own fence was too early: the game then rewrote data the
// other queues still read (device lost at the title).
NVN(nvnWindowAcquireTexture, NVNwindowAcquireTextureResult, NVNwindow* w, NVNsync* s, int* idx) {
    win_t* o = (win_t*)w;
    *idx = o->ntex ? o->next++ % o->ntex : 0;
    if (s) {
        ((sync_t*)s)->fenced = 2;
        ((sync_t*)s)->fence = o->last_pfence;
    }
    return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
NVN(nvnQueueAcquireTexture, NVNqueueAcquireTextureResult, NVNqueue* q, NVNwindow* w, int* idx) {
    (void)q;
    return h_nvnWindowAcquireTexture(w, NULL, idx) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS
               ? (NVNqueueAcquireTextureResult)0
               : (NVNqueueAcquireTextureResult)1;
}
// The image goes to the replay's present queue (mtl_present: after every submission before it);
// Lichii's present on this thread then shows it (nvn_direct_present_take, core/nvn_hook.cpp).
NVN(nvnQueuePresentTexture, void, NVNqueue* q, NVNwindow* w, int idx) {
    win_t* o = (win_t*)w;
    o->presents++;
    atomic_fetch_add(&g_frame, 1);
    if (idx < 0 || idx >= o->ntex || !o->tex[idx]) return;
    o->pfence[idx] = o->last_pfence = nvn_direct_present_queue_fence = ((const queue_t*)q)->last_fence;
    const int interval = o->interval > 0 ? o->interval : 1;
    const int has_crop = o->crop[2] > 0 && o->crop[3] > 0;
    nvn_cap_present(CTX(o->tex[idx]), has_crop ? o->crop : NULL); // DM_NVN_CAPTURE
    nvn_direct_present_note(mtl_present(CTX(o->tex[idx]), interval / 60.0, has_crop ? o->crop : NULL), (uint32_t)interval);
}

// ---------- Window state and HDR metadata ----------
// What the getters return is what the driver returns (Mario Kart World's sdk, read statically):
// the value set, or SetDefaults' value. The HDR metadata and the colour space are the game's
// description of what it renders. Its driver hands a window's metadata to the system for the
// display; Lichii's presenter takes no metadata, from either renderer (nx2:vi2's layer metadata is
// not kept either): it labels a frame by its format and the HDR status the game was told
// (Service::Nx2NativeImageColorSpace), so these are kept for the getters only.
NVN_NOPROTO(nvnWindowBuilderGetDevice, NVNdevice*, const NVNwindowBuilder* b) { return WB(b)->dev; }
NVN_NOPROTO(nvnWindowBuilderGetNumTextures, int, const NVNwindowBuilder* b) { return WB(b)->ntex; }
NVN_NOPROTO(nvnWindowBuilderGetTexture, NVNtexture*, const NVNwindowBuilder* b, int i) {
    return i >= 0 && i < WB(b)->ntex ? builder_texture(WB(b), i) : NULL;
}
NVN(nvnWindowBuilderGetNativeWindow, NVNnativeWindow, const NVNwindowBuilder* b) { return WB(b)->native; }
NVN(nvnWindowBuilderGetPresentInterval, int, const NVNwindowBuilder* b) { return WB(b)->interval; }
NVN_NOPROTO(nvnWindowBuilderGetNumActiveTextures, int, const NVNwindowBuilder* b) { return WB(b)->active; }
NVN(nvnWindowGetNativeWindow, NVNnativeWindow, const NVNwindow* w) { return ((const win_t*)w)->native; }
NVN_NOPROTO(nvnWindowGetNumTextures, int, const NVNwindow* w) { return ((const win_t*)w)->ntex; }
NVN_NOPROTO(nvnWindowGetNumActiveTextures, int, const NVNwindow* w) { return ((const win_t*)w)->active; }
NVN_NOPROTO(nvnWindowBuilderSetColorSpace, void, NVNwindowBuilder* b, int space) { WB(b)->color_space = space; }
NVN_NOPROTO(nvnWindowBuilderGetColorSpace, int, const NVNwindowBuilder* b) { return WB(b)->color_space; }

static void hdr_set_display(win_hdr_t* h, float rx, float ry, float gx, float gy, float bx, float by,
                            float wx, float wy, float l0, float l1) {
    const float v[10] = {rx, ry, gx, gy, bx, by, wx, wy, l0, l1};
    memcpy(h->display, v, sizeof v);
}
static void hdr_get_display(const win_hdr_t* h, float* rx, float* ry, float* gx, float* gy, float* bx,
                            float* by, float* wx, float* wy, float* l0, float* l1) {
    float* const out[10] = {rx, ry, gx, gy, bx, by, wx, wy, l0, l1};
    for (int i = 0; i < 10; i++) *out[i] = h->display[i];
}
NVN_NOPROTO(nvnWindowBuilderSetHdrDisplayMetadata, void, NVNwindowBuilder* b, float rx, float ry, float gx,
            float gy, float bx, float by, float wx, float wy, float l0, float l1) {
    hdr_set_display(WBHDR(b), rx, ry, gx, gy, bx, by, wx, wy, l0, l1);
}
NVN_NOPROTO(nvnWindowBuilderGetHdrDisplayMetadata, void, const NVNwindowBuilder* b, float* rx, float* ry,
            float* gx, float* gy, float* bx, float* by, float* wx, float* wy, float* l0, float* l1) {
    hdr_get_display(WBHDR(b), rx, ry, gx, gy, bx, by, wx, wy, l0, l1);
}
NVN_NOPROTO(nvnWindowBuilderSetHdrContentMetadata, void, NVNwindowBuilder* b, float l0, float l1) {
    WBHDR(b)->content[0] = l0;
    WBHDR(b)->content[1] = l1;
}
NVN_NOPROTO(nvnWindowBuilderGetHdrContentMetadata, void, const NVNwindowBuilder* b, float* l0, float* l1) {
    *l0 = WBHDR(b)->content[0];
    *l1 = WBHDR(b)->content[1];
}
NVN_NOPROTO(nvnWindowSetHdrDisplayMetadata, void, NVNwindow* w, float rx, float ry, float gx, float gy,
            float bx, float by, float wx, float wy, float l0, float l1) {
    hdr_set_display(&((win_t*)w)->hdr, rx, ry, gx, gy, bx, by, wx, wy, l0, l1);
}
NVN_NOPROTO(nvnWindowGetHdrDisplayMetadata, void, const NVNwindow* w, float* rx, float* ry, float* gx,
            float* gy, float* bx, float* by, float* wx, float* wy, float* l0, float* l1) {
    hdr_get_display(&((const win_t*)w)->hdr, rx, ry, gx, gy, bx, by, wx, wy, l0, l1);
}
NVN_NOPROTO(nvnWindowSetHdrContentMetadata, void, NVNwindow* w, float l0, float l1) {
    ((win_t*)w)->hdr.content[0] = l0;
    ((win_t*)w)->hdr.content[1] = l1;
}
NVN_NOPROTO(nvnWindowGetHdrContentMetadata, void, const NVNwindow* w, float* l0, float* l1) {
    *l0 = ((const win_t*)w)->hdr.content[0];
    *l1 = ((const win_t*)w)->hdr.content[1];
}
