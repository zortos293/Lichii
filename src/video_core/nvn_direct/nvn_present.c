// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Presents in direct mode: what upstream's vka_present.c did (left out there),
// handing the presented image to Lichii's presenter instead of a swapchain of the replay's own.
// The game's present takes a ticket (vka_present_reserve, on the game's thread, from mtl_present);
// the replay worker fills it once every submission before it was handed to the queue
// (vka_present_fill: the image and the fence that finishes drawing it); Lichii's present on the
// game's thread waits for the fill and takes a reference to the image (nvn_direct_present_take),
// and Lichii's GPU thread shows it (nvn_direct_present_frame from RasterizerVulkan's
// AccelerateDisplay), so screenshots, HDR conversion and the composite are Lichii's own.

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "bananative/dm/gfx/vk_android/vka.h"
#include "nvn_direct.h"
#include "nvn_direct_front.h"

// Presents filled and not yet taken (the game's thread takes each right after it presents: at
// most one is outstanding per presenting thread).
enum { NPRES = 8 };
typedef struct {
    uint64_t ticket, fence;
    uint64_t serial;
    VkFormat format;   // the presenting texture's (UNORM for sRGB); its image may have been made in another (learned view formats)
    int32_t crop[4];
    int has_crop;
} pres_t;
static pres_t g_pres[NPRES];
static uint64_t g_reserved, g_filled;
// The presenting queue's last submission, by ticket: the present waits for it
// and every fence before it. The last submission of any queue may be another queue's that waits for an event the game
// signals only in its next frame (Mario Kart World's second queue): the next acquire then waited
// for it, and the composite up to 1 s.
_Thread_local uint64_t nvn_direct_present_queue_fence;
static uint64_t g_queue_fence[NPRES];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

uint64_t vka_present_reserve(void) {
    pthread_mutex_lock(&g_mu);
    const uint64_t tk = ++g_reserved;
    g_queue_fence[tk % NPRES] = nvn_direct_present_queue_fence;
    pthread_mutex_unlock(&g_mu);
    return tk;
}

void vka_perf_frame(double period_s);
void mtl_create_ns(uint64_t* lib_ns, uint64_t* pso_ns); // vka_pso.c
void vka_fsc_stats(uint64_t* hit, uint64_t* miss); // vka_draw.c
void vka_skip_report(void); // vka_draw.c
void vka_rt_size_report(void); // vka_draw.c
void vka_copy_drop_stats(uint64_t* dropped, uint64_t* kept); // vka_exec.c
// Pipeline creation on the frame path (the replay thread's compiles, links and
// cache lookups, vka_pso.c g_create_ns) between two presents, when it is over a 60 Hz frame: the
// stalls a cold pipeline cache costs, frame by frame (the first 300), and every 1800 presents a sum.
static void frame_path_creation(uint64_t tk) {
    static uint64_t last_ns, over16, over50, over_ns;
    static int logged;
    uint64_t lib_ns, ns;
    mtl_create_ns(&lib_ns, &ns);
    const uint64_t d = ns - last_ns;
    last_ns = ns;
    if (d > 16666667ull) {
        over16++;
        over50 += d > 50000000ull;
        over_ns += d;
        extern _Atomic uint64_t vka_cat_ns[5]; static uint64_t lc[5]; uint64_t c[5]; for (int i = 0; i < 5; i++) { c[i] = atomic_load(&vka_cat_ns[i]); }
        if (logged++ < 300) VLOG("present %llu: frame-path pipeline creation %.1f ms (probe %.1f, link %.1f, whole %.1f, compute %.1f; waits for other threads' builds %.1f)", (unsigned long long)tk, d / 1e6,
                                 (c[0] - lc[0]) / 1e6, (c[1] - lc[1]) / 1e6, (c[2] - lc[2]) / 1e6, (c[3] - lc[3]) / 1e6, (c[4] - lc[4]) / 1e6);
        for (int i = 0; i < 5; i++) lc[i] = c[i];
    }
    if (tk % 1800 == 0) {
        uint64_t hit, miss;
        vka_fsc_stats(&hit, &miss);
        VLOG("presents up to %llu: %llu frames with over 16.7 ms of frame-path pipeline creation (%llu over 50 ms), %.1f s in them; pixel shader sets %llu reused, %llu written",
             (unsigned long long)tk, (unsigned long long)over16, (unsigned long long)over50, over_ns / 1e9, (unsigned long long)hit, (unsigned long long)miss);
        {   // what the frame-path creation above is made of: shaders new to DM_TEMP, libraries compiled rather than read from the caches
            unsigned made, kept;
            uint64_t slow, ahead;
            vka_rc_counts(&made, &kept);
            vka_lib_counts(&slow, &ahead);
            VLOG("presents up to %llu: %u shaders made (%u translated now, %u kept from earlier runs); %llu graphics pipeline libraries compiled outside the ahead threads (over 20 ms), %llu made ahead",
                 (unsigned long long)tk, made, made - kept, kept, (unsigned long long)slow, (unsigned long long)ahead);
        }
        vka_skip_report();
        vka_rt_size_report();
        { uint64_t dr, kp; vka_copy_drop_stats(&dr, &kp); VLOG("copies into left-out levels dropped so far %llu, into kept levels of such textures %llu", (unsigned long long)dr, (unsigned long long)kp); }
    }
}
uint64_t vka_present_filled(void) { return g_filled; }   // (the replay thread, which fills them)
void vka_present_fill(uint64_t tk, const tex_t* t, double period_s, const int crop[4]) {
    vka_perf_frame(period_s);
    frame_path_creation(tk);
    pthread_mutex_lock(&g_mu);
    pres_t* p = &g_pres[tk % NPRES];
    p->ticket = tk;
    p->serial = t->serial;
    vka_fmt f;
    p->format = vka_fmt_of(t->d.format, &f) ? f.vk : VK_FORMAT_UNDEFINED;
    // The presenting queue's last submission (handed to the queue: the worker fills presents in
    // order with the submissions).
    p->fence = vka_ok() ? g_queue_fence[tk % NPRES] : 0;
    p->has_crop = crop && crop[2] > 0 && crop[3] > 0;
    if (p->has_crop) memcpy(p->crop, crop, sizeof p->crop);
    if (tk > g_filled) g_filled = tk;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

// The emulator shuts down (nvn_direct_stop): the waits for a fill see vka_stopping.
void nvn_direct_present_wake(void) {
    pthread_mutex_lock(&g_mu);
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

uint64_t vka_present_inline(const tex_t* t, double period_s, const int crop[4]) {
    const uint64_t tk = vka_present_reserve();
    vka_present_fill(tk, t, period_s, crop);
    return tk;
}

void mtl_present_flush(void) {
    pthread_mutex_lock(&g_mu);
    while (g_filled < g_reserved && !vka_stopping()) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
}

// The images handed to Lichii, by frame id: each held (vka_tex_acquire) with a view of its own
// until NHELD later frames were taken, long after Lichii's composite of it ran (the game's present
// waits for the previous composite: GPU::WaitForComposite).
enum { NHELD = 8 };
typedef struct {
    uint64_t id;
    vka_img* im;
    nvn_direct_frame frame;
} held_t;
static held_t g_held[NHELD];
static uint64_t g_frames;
static pthread_mutex_t g_held_mu = PTHREAD_MUTEX_INITIALIZER;

static _Thread_local uint64_t t_ticket;
static _Thread_local uint32_t t_interval;
void nvn_direct_present_note(uint64_t ticket, uint32_t interval) {
    t_ticket = ticket;
    t_interval = interval;
}

static VkImageView frame_view(const vka_img* im, VkFormat format) {
    // Sampled only: the image's other uses (storage) need not hold for the view's format
    VkImageViewUsageCreateInfo vu = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &vu, .image = im->img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView v = VK_NULL_HANDLE;
    return vk.vkCreateImageView(V.dev, &vi, NULL, &v) ? VK_NULL_HANDLE : v;
}

int nvn_direct_present_take(nvn_direct_frame* out) {
    memset(out, 0, sizeof *out);
    out->interval = t_interval ? t_interval : 1;
    // Fritchii: the image of the present before this one (FRITCHII_NVN_PRESENT_LAG, default 1; 0: this one). Waiting for this present's own
    // ticket held the game until the replay worker had recorded its whole frame (8.9 ms of each 20 ms frame in Ocarina of Time): with the
    // previous one, normally filled already, the game records its next frame meanwhile, at a frame of latency
    static int lag = -1; if (lag < 0) { const char *e = getenv("FRITCHII_NVN_PRESENT_LAG"); lag = e ? atoi(e) != 0 : 1; }
    static uint64_t pending;
    uint64_t tk = t_ticket;
    t_ticket = 0;
    if (lag) { const uint64_t now = tk; tk = pending; pending = now; }
    if (!tk) return 0;
    pthread_mutex_lock(&g_mu);
    while (g_filled < tk && !vka_stopping()) pthread_cond_wait(&g_cv, &g_mu);
    const pres_t p = g_pres[tk % NPRES];
    pthread_mutex_unlock(&g_mu);
    if (vka_stopping()) return 0;   // the emulator shuts down (nvn_direct_stop): the worker may never fill it
    { static int sync = -1; if (sync < 0) { const char *e = getenv("FRITCHII_NVN_SYNC_PRESENT"); sync = e ? atoi(e) : 0; }   // 1: the game waits for the taken frame's GPU work at its present; 2: for the one before it
      static uint64_t prev_fence;
      if (sync == 1 && p.fence) vka_wait_done(p.fence, 2000000000ull);
      if (sync == 2 && prev_fence) vka_wait_done(prev_fence, 2000000000ull);
      prev_fence = p.fence; }
    if (!vka_ok() || p.ticket != tk) return 0;
    vka_img* im = vka_tex_acquire(p.serial);
    if (!im) return 0;   // the replay never drew or uploaded it (a boot frame): nothing to show
    int ok = im->inited && im->aspect == VK_IMAGE_ASPECT_COLOR_BIT && im->samples == 1 && (im->usage & VK_IMAGE_USAGE_SAMPLED_BIT);
    // In the presenting texture's format: a texture of another format sharing the image wrote it in its own (an RGB10A2 window over an RGBA8 image)
    VkFormat format = p.format;
    if (ok && format != im->f.vk && !(format && vka_img_lists(im, format))) {
        VRED_ONCE("window texture serial %llu: its image (VkFormat %d) cannot be viewed in the texture's VkFormat %d: shown in the image's", (unsigned long long)p.serial, im->f.vk, format);
        format = im->f.vk;
    }
    VkImageView view = ok ? frame_view(im, format) : VK_NULL_HANDLE;
    if (!view) {
        VRED_ONCE("window texture serial %llu (format %#x) cannot be shown (initialised %d, aspect %#x, samples %u, usage %#x)", (unsigned long long)im->serial, im->d.format,
                  im->inited, im->aspect, im->samples, im->usage);
        vka_tex_drop(im);
        return 0;
    }
    pthread_mutex_lock(&g_held_mu);
    const uint64_t id = ++g_frames;
    held_t* h = &g_held[id % NHELD];
    if (h->im) {
        vk.vkDestroyImageView(V.dev, h->frame.view, NULL);
        vka_tex_drop(h->im);
    }
    h->id = id;
    h->im = im;
    h->frame = (nvn_direct_frame){.id = id, .image = im->img, .view = view, .format = format, .width = im->scaled ? im->d.width : im->w, .height = im->scaled ? im->d.height : im->h,
                                  .scaled_width = im->w, .scaled_height = im->h, .interval = out->interval, .fence = p.fence};
    if (p.has_crop) memcpy(h->frame.crop, p.crop, sizeof p.crop);
    *out = h->frame;
    pthread_mutex_unlock(&g_held_mu);
    return 1;
}

int nvn_direct_present_frame(uint64_t id, nvn_direct_frame* out) {
    pthread_mutex_lock(&g_held_mu);
    // A frame dropped already (Lichii composited it late): the newest one held instead.
    const held_t* h = &g_held[id % NHELD];
    if (h->id != id || !h->im) h = &g_held[g_frames % NHELD];
    const int ok = h->im != NULL;
    if (ok) *out = h->frame;
    pthread_mutex_unlock(&g_held_mu);
    if (!ok) return 0;
    // Drawn on the replay's queues: Lichii samples it once the GPU is past its fence (bounded:
    // a frame whose GPU work waits on the CPU is shown as far as it got).
    if (out->fence && !vka_wait_done(out->fence, 1000000000ull)) {
        static _Atomic int n;
        if (atomic_fetch_add(&n, 1) < 8) VRED("present: frame %llu's fence %llu not reached in 1 s: shown as it is", (unsigned long long)out->id, (unsigned long long)out->fence);
    }
    return 1;
}

// The teardown (vka_dev.c vka_destroy): the frames still held, their views and references
void nvn_present_destroy(void) {
    for (int i = 0; i < NHELD; i++) if (g_held[i].im) {
        vk.vkDestroyImageView(V.dev, g_held[i].frame.view, NULL);
        vka_tex_drop(g_held[i].im);
    }
}
