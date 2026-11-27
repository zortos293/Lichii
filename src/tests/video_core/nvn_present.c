// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// nvn_present.c against stubs of the replay: the composite waits for the presented frame's own
// fence and is given the GPU signal of it taken at the present, and a window acquire of an image
// waits while Lichii's composite of it is queued, being recorded or not done on the GPU.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "vka.h"
#include "video_core/nvn_direct/nvn_direct.h"
#include "video_core/nvn_direct/nvn_direct_front.h"

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%d: %s\n", __LINE__, #expr); exit(1); } } while (0)

vka_fns vk;
vka_dev V;

static vka_img images[2];
static int own_fence_done, stopping, hung, hangs;
static int64_t host_tick;   // -1: the tick query fails
static uint64_t hang_ns = 3600000000000ull, cpu_only_fence;
static int ring_lost;   // vka_dev.c's ring no longer has any fence

void dm_log(const char* file, int line, const char* function, const char* format, ...) {
    (void)file;
    (void)line;
    (void)function;
    (void)format;
}
void vka_perf_frame(double period_s) { (void)period_s; }
void mtl_create_ns(uint64_t* lib_ns, uint64_t* pso_ns) { *lib_ns = *pso_ns = 0; }
void vka_fsc_stats(uint64_t* hit, uint64_t* miss) { *hit = *miss = 0; }
void vka_skip_report(void) {}
void vka_rt_size_report(void) {}
void vka_copy_drop_stats(uint64_t* dropped, uint64_t* kept) { *dropped = *kept = 0; }
void vka_rc_counts(unsigned* made, unsigned* kept) { *made = *kept = 0; }
void vka_lib_counts(uint64_t* compiled, uint64_t* ahead) { *compiled = *ahead = 0; }
int vka_fmt_of(int nvn_format, vka_fmt* f) {
    (void)nvn_format;
    f->vk = VK_FORMAT_R8G8B8A8_UNORM;
    return 1;
}
int vka_ok(void) { return 1; }
int vka_stopping(void) { return stopping; }
int vka_hung(void) { return hung; }
uint64_t vka_hang_ns(void) { return hang_ns; }
void vka_hang(const char* what) {
    (void)what;
    hangs++;
    hung = 1;
}
int vka_img_lists(vka_img* im, VkFormat v) { return im->f.vk == v; }
vka_img* vka_tex_acquire(uint64_t serial) {
    for (int i = 0; i < 2; i++) if (images[i].serial == serial) { images[i].holds++; return &images[i]; }
    return NULL;
}
void vka_tex_drop(vka_img* im) { im->holds--; }
int vka_tex_rt_dump(const char* dir) { (void)dir; return 1; }
// The fence alone: every earlier fence of the other queues (vka_wait_done) is not the composite's to wait for
int vka_wait_fence(uint64_t fence, uint64_t timeout_ns) {
    CHECK(fence && timeout_ns);
    return own_fence_done;
}
int vka_wait_done(uint64_t fence, uint64_t timeout_ns) {
    (void)fence;
    (void)timeout_ns;
    CHECK(!"the composite waited for the fences of every queue");
    return 0;
}
int vka_host_tick_done(uint64_t tick) { return host_tick < 0 ? -1 : (uint64_t)host_tick >= tick; }
// Queue submissions signal their queue's timeline (0x502) at 1000 + fence on the GPU; cpu_only_fence ran on the CPU
int vka_fence_signal(uint64_t fence, VkSemaphore* sem, uint64_t* val) {
    if (ring_lost || fence == cpu_only_fence) return 0;
    *sem = (VkSemaphore)(uintptr_t)0x502;
    *val = 1000 + fence;
    return 1;
}

static VKAPI_ATTR VkResult VKAPI_CALL create_view(VkDevice dev, const VkImageViewCreateInfo* info, const VkAllocationCallbacks* alloc, VkImageView* view) {
    (void)dev;
    (void)info;
    (void)alloc;
    static uintptr_t next = 1;
    *view = (VkImageView)next++;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_view(VkDevice dev, VkImageView view, const VkAllocationCallbacks* alloc) {
    (void)dev;
    (void)view;
    (void)alloc;
}

// The game presents texture serial from a queue whose last submission is fence; Lichii takes the frame.
static int present(uint64_t serial, uint64_t fence, nvn_direct_frame* frame) {
    nvn_direct_present_queue_fence = fence;
    const uint64_t tk = vka_present_reserve();
    tex_t t = {0};
    t.serial = serial;
    vka_present_fill(tk, &t, 1 / 60.0, NULL);
    nvn_direct_present_note(tk, 1);
    return nvn_direct_present_take(frame);
}

int main(void) {
    vk.vkCreateImageView = create_view;
    vk.vkDestroyImageView = destroy_view;
    for (int i = 0; i < 2; i++) {
        images[i] = (vka_img){.serial = 1 + i, .img = (VkImage)(uintptr_t)(0x100 + i), .f = {.vk = VK_FORMAT_R8G8B8A8_UNORM}, .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
                              .aspect = VK_IMAGE_ASPECT_COLOR_BIT, .w = 64, .h = 64, .samples = 1, .inited = 1};
    }
    nvn_direct_frame first, shown;

    // Presented and its composite queued, no tick yet (the game acquired both images ahead): its
    // image is the compositor's
    CHECK(present(1, 10, &first) && first.fence == 10);
    CHECK(first.semaphore == (VkSemaphore)(uintptr_t)0x502 && first.semaphore_value == 1010);   // its queue's GPU signal, taken at the present
    CHECK(nvn_direct_present_composite_pending(1));
    CHECK(!nvn_direct_present_composite_pending(2));   // another window image
    CHECK(!nvn_direct_present_composite_pending(3));   // a texture without an image
    // Not past its own fence in 1 s: dropped, not sampled, and the image is no longer the compositor's
    CHECK(!nvn_direct_present_frame(first.id, &shown));
    CHECK(!nvn_direct_present_composite_pending(1));
    own_fence_done = 1;
    // vka_dev.c's ring lost the fence meanwhile: the signal taken at the present stands
    ring_lost = 1;
    CHECK(nvn_direct_present_frame(first.id, &shown) && shown.id == first.id);
    CHECK(shown.semaphore == (VkSemaphore)(uintptr_t)0x502 && shown.semaphore_value == 1010);
    ring_lost = 0;
    // The image is the compositor's while the composite is recorded, then until its tick is done
    CHECK(nvn_direct_present_composite_pending(1));
    nvn_direct_present_composited(shown.id, 7);
    host_tick = 6;
    CHECK(nvn_direct_present_composite_pending(1));
    CHECK(!nvn_direct_present_composite_pending(2));
    // An earlier tick reported late leaves the later one
    nvn_direct_present_composited(shown.id, 5);
    CHECK(nvn_direct_present_composite_pending(1));
    // A failed tick query does not free it, nor stop the replay before the hang time
    host_tick = -1;
    for (int i = 0; i < 4; i++) CHECK(nvn_direct_present_composite_pending(1));
    CHECK(!hangs);
    // A query that succeeds starts the count again; one that keeps failing for the hang time stops the replay as a GPU hang, which frees the acquire
    host_tick = 6;
    CHECK(nvn_direct_present_composite_pending(1));
    host_tick = -1;
    hang_ns = 0;
    CHECK(nvn_direct_present_composite_pending(1) && !hangs);   // (the first failure since)
    CHECK(nvn_direct_present_composite_pending(1) && hangs == 1);
    CHECK(!nvn_direct_present_composite_pending(1) && hangs == 1);   // hung: nothing waits
    hung = 0;
    hang_ns = 3600000000000ull;
    host_tick = 7;
    CHECK(!nvn_direct_present_composite_pending(1));

    // Two presenting threads: the second takes the later frame id and requests its composite
    // first; Lichii's composite of it leaves the first frame's queued composite the compositor's
    nvn_direct_frame a, b;
    CHECK(present(1, 11, &a) && present(2, 12, &b) && b.id > a.id);
    CHECK(nvn_direct_present_frame(b.id, &shown) && shown.id == b.id);
    nvn_direct_present_composited(shown.id, 8);
    host_tick = 8;
    CHECK(!nvn_direct_present_composite_pending(2));
    CHECK(nvn_direct_present_composite_pending(1));
    CHECK(nvn_direct_present_frame(a.id, &shown) && shown.id == a.id);
    CHECK(nvn_direct_present_composite_pending(1));
    nvn_direct_present_composited(shown.id, 9);
    CHECK(nvn_direct_present_composite_pending(1));
    host_tick = 9;
    CHECK(!nvn_direct_present_composite_pending(1));

    // The submission that drew it ran on the CPU (no command buffer, a failed submission): no GPU
    // signal to hand Lichii's queue, so not shown, and nothing for an acquire to wait for
    nvn_direct_frame cpu;
    cpu_only_fence = 13;
    CHECK(!present(2, 13, &cpu));
    CHECK(!nvn_direct_present_composite_pending(2));

    // A queued composite at shutdown: the acquire no longer waits
    nvn_direct_frame last;
    CHECK(present(2, 14, &last));
    CHECK(nvn_direct_present_composite_pending(2));
    stopping = 1;
    CHECK(!nvn_direct_present_composite_pending(2));
    CHECK(images[0].holds == 2 && images[1].holds == 2);   // the held frames' references only
    puts("nvn_present_tests: ok");
    return 0;
}
