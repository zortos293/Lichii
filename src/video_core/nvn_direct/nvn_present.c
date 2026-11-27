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
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bananative/dm/gfx/vk_android/vka.h"
#include "nvn_direct.h"
#include "nvn_direct_front.h"

// Presents filled and not yet taken (the game's thread takes each right after it presents: at
// most one is outstanding per presenting thread).
enum { NPRES = 8 };
typedef struct {
    uint64_t ticket, fence;
    VkSemaphore semaphore;   // the GPU signal of fence (vka_fence_signal), taken when filled: VK_NULL_HANDLE none
    uint64_t semaphore_value;
    uint64_t serial;
    VkFormat format;   // the presenting texture's (UNORM for sRGB); its image may have been made in another (learned view formats)
    int32_t crop[4];
    int has_crop;
} pres_t;
static pres_t g_pres[NPRES];
static uint64_t g_reserved, g_filled;
// The presenting queue's last submission, by ticket: the composite waits for that fence alone (its
// queue's earlier work and the syncs it waited for on the GPU come before it), not the fences of
// other queues before it. The last submission of any queue may be another queue's that waits for
// an event the game signals only in its next frame (Mario Kart World's second queue): the next
// acquire then waited for it, and the composite up to 1 s.
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
        if (logged++ < 300) VLOG("present %llu: frame-path pipeline creation %.1f ms", (unsigned long long)tk, d / 1e6);
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
    {   // DM_VK_RTDUMP=<present>:<dir> (vka_tex.c vka_tex_rt_dump): on the replay worker, where the present stands in replay order, nothing more submitted meanwhile;
        // at the first present from that one on at which the GPU has done everything handed to it (none is waited for), tried for 600 presents
        static const char* dump = (const char*)1;
        static uint64_t at;
        static int done;
        if (dump == (const char*)1) dump = getenv("DM_VK_RTDUMP");
        char* colon = NULL;
        if (dump && !done && !at) at = strtoull(dump, &colon, 10);
        if (dump && !done && at && tk >= at) {
            int vka_tex_rt_dump(const char*);
            const char* dir = strchr(dump, ':');
            if (!dir || tk >= at + 600 || vka_tex_rt_dump(dir + 1)) {
                done = 1;
                if (!dir || tk >= at + 600) VRED("DM_VK_RTDUMP: the GPU was never idle at a present from %llu to %llu: nothing read back", (unsigned long long)at, (unsigned long long)tk);
                else if (tk > at) VLOG("DM_VK_RTDUMP: read back at present %llu (asked for %llu: the GPU was busy at the presents before)", (unsigned long long)tk, (unsigned long long)at);
            }
        }
    }
    vka_perf_frame(period_s);
    frame_path_creation(tk);
    // The presenting queue's last submission (handed to the queue: the worker fills presents in
    // order with the submissions) and what the GPU signals there, taken now: the fence's slot in
    // vka_dev.c's ring may be another's by the time Lichii composites it.
    pthread_mutex_lock(&g_mu);
    const uint64_t fence = vka_ok() ? g_queue_fence[tk % NPRES] : 0;
    pthread_mutex_unlock(&g_mu);
    VkSemaphore semaphore = VK_NULL_HANDLE;
    uint64_t semaphore_value = 0;
    if (fence && !vka_fence_signal(fence, &semaphore, &semaphore_value)) semaphore = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_mu);
    pres_t* p = &g_pres[tk % NPRES];
    p->ticket = tk;
    p->serial = t->serial;
    vka_fmt f;
    p->format = vka_fmt_of(t->d.format, &f) ? f.vk : VK_FORMAT_UNDEFINED;
    p->fence = fence;
    p->semaphore = semaphore;
    p->semaphore_value = semaphore_value;
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
// waits for the previous composite: GPU::WaitForComposite). Holding it keeps the image, not its
// contents: the game's next acquire of the image waits (nvn_direct_present_composite_pending)
// while Lichii's composite of it is queued (queued: from the present until Lichii's GPU thread
// comes to it), being recorded (sampling) or submitted at composite_tick (Lichii's scheduler
// tick) and the GPU not past it.
enum { NHELD = 8 };
typedef struct {
    uint64_t id;
    vka_img* im;
    nvn_direct_frame frame;
    uint64_t composite_tick;
    int queued, sampling;
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
    const uint64_t tk = t_ticket;
    if (!tk) return 0;
    t_ticket = 0;
    pthread_mutex_lock(&g_mu);
    while (g_filled < tk && !vka_stopping()) pthread_cond_wait(&g_cv, &g_mu);
    const pres_t p = g_pres[tk % NPRES];
    pthread_mutex_unlock(&g_mu);
    if (vka_stopping()) return 0;   // the emulator shuts down (nvn_direct_stop): the worker may never fill it
    if (!vka_ok() || p.ticket != tk) return 0;
    if (p.fence && !p.semaphore) {
        // No GPU work signals the submission that drew it (it ran on the CPU: no command buffer,
        // a failed vkQueueSubmit or a hang; or it was more than vka_dev.c's FQ_RING fences
        // before the present): nothing Lichii's queue could wait on for its writes. Not shown.
        static _Atomic int n;
        if (atomic_fetch_add(&n, 1) < 8) VRED("present %llu: fence %llu has no GPU signal: dropped", (unsigned long long)tk, (unsigned long long)p.fence);
        return 0;
    }
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
    h->composite_tick = 0;
    h->queued = 1;   // (Lichii's present queues its composite next: core/nvn_hook.cpp PresentDirect)
    h->sampling = 0;
    h->frame = (nvn_direct_frame){.id = id, .image = im->img, .view = view, .format = format, .width = im->scaled ? im->d.width : im->w, .height = im->scaled ? im->d.height : im->h,
                                  .scaled_width = im->w, .scaled_height = im->h, .interval = out->interval, .fence = p.fence,
                                  .semaphore = p.semaphore, .semaphore_value = p.semaphore_value};
    if (p.has_crop) memcpy(h->frame.crop, p.crop, sizeof p.crop);
    *out = h->frame;
    pthread_mutex_unlock(&g_held_mu);
    return 1;
}

// A composite is done with frame id: submitted at tick (0: it did not sample the frame).
static void composite_done(uint64_t id, uint64_t tick) {
    pthread_mutex_lock(&g_held_mu);
    held_t* h = &g_held[id % NHELD];
    if (h->id == id && h->im) {
        h->sampling = 0;
        if (tick > h->composite_tick) h->composite_tick = tick;
    }
    pthread_mutex_unlock(&g_held_mu);
}

int nvn_direct_present_frame(uint64_t id, nvn_direct_frame* out) {
    pthread_mutex_lock(&g_held_mu);
    // This present's composite request is no longer queued (only its own: presents from several
    // threads take frame ids in another order than they request their composites).
    held_t* h = &g_held[id % NHELD];
    if (h->id == id) h->queued = 0;
    // A frame dropped already (Lichii composited it late): the newest one held instead.
    if (h->id != id || !h->im) h = &g_held[g_frames % NHELD];
    const int ok = h->im != NULL && !vka_stopping() && !vka_hung();
    if (ok) {
        *out = h->frame;
        h->sampling = 1;
    }
    pthread_mutex_unlock(&g_held_mu);
    if (!ok) return 0;
    // Drawn on the replay's queues: Lichii samples it once the GPU is past its fence.
    if (out->fence && !vka_wait_fence(out->fence, 1000000000ull)) {
        static _Atomic int n;
        if (atomic_fetch_add(&n, 1) < 8) VRED("present: frame %llu's fence %llu not reached in 1 s: dropped", (unsigned long long)out->id, (unsigned long long)out->fence);
        composite_done(out->id, 0);
        return 0;
    }
    if (vka_stopping() || vka_hung()) {
        composite_done(out->id, 0);
        return 0;
    }
    // out->semaphore: the composite's submission also waits for it (its queue's own timeline, not
    // every queue's), which makes the image's writes visible to Lichii's queue (the CPU wait above
    // alone does not); its value is reached already.
    return 1;
}

void nvn_direct_present_composited(uint64_t id, uint64_t tick) { composite_done(id, tick); }

// Since when Lichii's tick could not be read (0: it could): the image stays the composite's
// meanwhile, and after the hang time the replay stops as after a GPU hang.
static _Atomic uint64_t g_tick_fail_ns;
static int tick_done(uint64_t tick) {
    const int r = vka_host_tick_done(tick);
    if (r >= 0) {
        atomic_store(&g_tick_fail_ns, 0);
        return r;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    uint64_t since = 0;
    if (!atomic_compare_exchange_strong(&g_tick_fail_ns, &since, now) && now - since >= vka_hang_ns())
        vka_hang("Lichii's GPU tick could not be read: its composites' images are never free again");
    return 0;
}

// The image of texture serial (a window's next one) is still read by a composite the GPU is not
// past. A host-side check only: the acquire halts the guest thread meanwhile (nvn_dispatch.c), so
// nothing that writes the image is submitted before.
int nvn_direct_present_composite_pending(uint64_t serial) {
    if (vka_stopping() || vka_hung() || !vka_ok()) return 0;
    vka_img* im = vka_tex_acquire(serial);
    if (!im) return 0;
    uint64_t tick = 0;
    int owned = 0;
    pthread_mutex_lock(&g_held_mu);
    for (int i = 0; i < NHELD; i++) if (g_held[i].im == im) {
        owned |= g_held[i].queued || g_held[i].sampling;
        if (g_held[i].composite_tick > tick) tick = g_held[i].composite_tick;
    }
    pthread_mutex_unlock(&g_held_mu);
    vka_tex_drop(im);
    return owned || (tick && !tick_done(tick));
}

// The teardown (vka_dev.c vka_destroy): the frames still held, their views and references
void nvn_present_destroy(void) {
    for (int i = 0; i < NHELD; i++) if (g_held[i].im) {
        vk.vkDestroyImageView(V.dev, g_held[i].frame.view, NULL);
        vka_tex_drop(g_held[i].im);
    }
}
