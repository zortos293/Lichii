// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The emulator shutting down in direct mode. Upstream's replay lives as long as
// its process (detached threads, the GPU until exit); in Lichii the emulation stops first and the
// device goes away while the process goes on, and the next game starts the replay again.
// - nvn_direct_stop, before the guest's cores are suspended: a game thread inside a halted NVN
//   call (core/nvn_hook.cpp RunHaltedCall) waits on the host, where the kernel cannot suspend it
//   (a fence, room in the replay worker's queue, a present's fill). Those waits return now:
//   nothing more is replayed or submitted and every fence reads as done.
// - nvn_direct_park, before Lichii's device and the renderer objects the replay calls back into
//   (the ASTC decode) are destroyed: the replay worker finishes what it holds, pipeline builds
//   and speculative shader translations stop, the pipeline cache is saved a last time, the
//   completion thread runs what was submitted, and every thread of the replay and the front end
//   leaves and is joined (vka_thread). None of them uses the device or guest memory after that. A
//   thread that does not stop within its bound (stuck in the driver) ends the process instead:
//   the device must not go away under it.
// - nvn_direct_destroy, once the device is idle: the replay's Vulkan objects and its imports and
//   views of guest memory go (vka_destroy).
// - nvn_direct_reset (nvn_reset.c), once the game is gone: every static variable back to its
//   initial value.

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "bananative/dm/gfx/vk_android/vka.h"
#include "nvn_direct.h"

void nvn_direct_present_wake(void); // nvn_present.c
int vka_rc_park(uint64_t timeout_ns); // nvn_recompile.c
void vka_destroy(void); // vka_dev.c
void nvn_hle_destroy(void); // hle_nvn.c
void nvn_tracking_destroy(void); // nvn_tracking.c

static _Atomic int g_parked;

// ---------- The threads ----------
enum { NTHREAD = 64 };
typedef struct {
    pthread_t th;
    void* (*fn)(void*);
    void* arg;
    void (*wake)(void);
    _Atomic int state; // 0 free, 1 running, 2 returned (to be joined)
} thread_slot;
static thread_slot g_threads[NTHREAD];
static pthread_mutex_t g_threads_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_leaving;

int vka_leaving(void) { return atomic_load_explicit(&g_leaving, memory_order_acquire); }

static void* thread_main(void* arg) {
    thread_slot* s = arg;
    void* r = s->fn(s->arg);
    atomic_store(&s->state, 2);
    return r;
}

int vka_thread(void* (*fn)(void*), void* arg, void (*wake)(void)) {
    pthread_mutex_lock(&g_threads_mu);
    thread_slot* s = NULL;
    for (int i = 0; i < NTHREAD && !s; i++) if (!atomic_load(&g_threads[i].state)) s = &g_threads[i];
    if (!s) {
        pthread_mutex_unlock(&g_threads_mu);
        VRED("more than %d threads: one is not started", NTHREAD);
        return -1;
    }
    s->fn = fn; s->arg = arg; s->wake = wake;
    atomic_store(&s->state, 1);
    const int r = pthread_create(&s->th, NULL, thread_main, s);
    if (r) atomic_store(&s->state, 0);
    pthread_mutex_unlock(&g_threads_mu);
    return r;
}

// Every thread asked to leave, woken until it has returned (bounded), and joined; 1 = all left
static int threads_leave(uint64_t timeout_ns, int* running) {
    atomic_store(&g_leaving, 1);
    const uint64_t t0 = now_ns();
    for (;;) {
        void (*wake[NTHREAD])(void);
        int n = 0, left = 1;
        pthread_mutex_lock(&g_threads_mu);
        for (int i = 0; i < NTHREAD; i++) {
            if (atomic_load(&g_threads[i].state) != 1) continue;
            left = 0;
            if (g_threads[i].wake) wake[n++] = g_threads[i].wake;
        }
        pthread_mutex_unlock(&g_threads_mu);
        for (int i = 0; i < n; i++) wake[i](); // (outside the lock: a wake takes the thread's own)
        *running = 0;
        if (!left) for (int i = 0; i < NTHREAD; i++) *running += atomic_load(&g_threads[i].state) == 1;
        if (left || now_ns() - t0 >= timeout_ns) break;
        usleep(1000);
    }
    for (int i = 0; i < NTHREAD; i++) {
        if (atomic_load(&g_threads[i].state) != 2) continue;
        pthread_join(g_threads[i].th, NULL);
        atomic_store(&g_threads[i].state, 0);
    }
    return !*running;
}

// ---------- The Vulkan objects ----------
// The replay makes and destroys every object through the vk table: counted there (vka_census_install, from vka_init), so the
// teardown's line says what it left (vka_census_line).
enum { O_MEMORY, O_BUFFER, O_IMAGE, O_VIEW, O_SAMPLER, O_SEMAPHORE, O_FENCE, O_COMMAND_POOL, O_SET_LAYOUT, O_DESCRIPTOR_POOL, O_MODULE, O_PIPELINE_LAYOUT,
       O_PIPELINE_CACHE, O_PIPELINE, O_KINDS };
static const char* const g_kind_name[O_KINDS] = {"device memory allocations", "buffers", "images", "image views", "samplers", "semaphores", "fences",
                                                 "command pools", "descriptor set layouts", "descriptor pools", "shader modules", "pipeline layouts",
                                                 "pipeline caches", "pipelines"};
static _Atomic int64_t g_live[O_KINDS], g_made[O_KINDS];
static void made(int kind) { atomic_fetch_add(&g_live[kind], 1); atomic_fetch_add(&g_made[kind], 1); }
#define CREATE(f, Info, T, kind)                                                                                   \
    static PFN_##f real_##f;                                                                                       \
    static VkResult VKAPI_CALL census_##f(VkDevice d, const Info* ci, const VkAllocationCallbacks* a, T* o) {      \
        const VkResult r = real_##f(d, ci, a, o);                                                                  \
        if (r == VK_SUCCESS) made(kind);                                                                           \
        return r;                                                                                                  \
    }
#define DESTROY(f, T, kind)                                                                                        \
    static PFN_##f real_##f;                                                                                       \
    static void VKAPI_CALL census_##f(VkDevice d, T o, const VkAllocationCallbacks* a) {                           \
        if (o) atomic_fetch_sub(&g_live[kind], 1);                                                                 \
        real_##f(d, o, a);                                                                                         \
    }
#define PIPELINES(f, Info)                                                                                         \
    static PFN_##f real_##f;                                                                                       \
    static VkResult VKAPI_CALL census_##f(VkDevice d, VkPipelineCache c, uint32_t n, const Info* ci,               \
                                          const VkAllocationCallbacks* a, VkPipeline* o) {                         \
        const VkResult r = real_##f(d, c, n, ci, a, o);                                                            \
        for (uint32_t i = 0; i < n; i++) if (o[i]) made(O_PIPELINE);                                               \
        return r;                                                                                                  \
    }
#define CENSUS(X)                                                                                                  \
    X(CREATE, vkAllocateMemory, VkMemoryAllocateInfo, VkDeviceMemory, O_MEMORY)                                    \
    X(DESTROY, vkFreeMemory, VkDeviceMemory, O_MEMORY)                                                             \
    X(CREATE, vkCreateBuffer, VkBufferCreateInfo, VkBuffer, O_BUFFER)                                              \
    X(DESTROY, vkDestroyBuffer, VkBuffer, O_BUFFER)                                                                \
    X(CREATE, vkCreateImage, VkImageCreateInfo, VkImage, O_IMAGE)                                                  \
    X(DESTROY, vkDestroyImage, VkImage, O_IMAGE)                                                                   \
    X(CREATE, vkCreateImageView, VkImageViewCreateInfo, VkImageView, O_VIEW)                                       \
    X(DESTROY, vkDestroyImageView, VkImageView, O_VIEW)                                                            \
    X(CREATE, vkCreateSampler, VkSamplerCreateInfo, VkSampler, O_SAMPLER)                                          \
    X(DESTROY, vkDestroySampler, VkSampler, O_SAMPLER)                                                             \
    X(CREATE, vkCreateSemaphore, VkSemaphoreCreateInfo, VkSemaphore, O_SEMAPHORE)                                  \
    X(DESTROY, vkDestroySemaphore, VkSemaphore, O_SEMAPHORE)                                                       \
    X(CREATE, vkCreateFence, VkFenceCreateInfo, VkFence, O_FENCE)                                                  \
    X(DESTROY, vkDestroyFence, VkFence, O_FENCE)                                                                   \
    X(CREATE, vkCreateCommandPool, VkCommandPoolCreateInfo, VkCommandPool, O_COMMAND_POOL)                         \
    X(DESTROY, vkDestroyCommandPool, VkCommandPool, O_COMMAND_POOL)                                                \
    X(CREATE, vkCreateDescriptorSetLayout, VkDescriptorSetLayoutCreateInfo, VkDescriptorSetLayout, O_SET_LAYOUT)  \
    X(DESTROY, vkDestroyDescriptorSetLayout, VkDescriptorSetLayout, O_SET_LAYOUT)                                  \
    X(CREATE, vkCreateDescriptorPool, VkDescriptorPoolCreateInfo, VkDescriptorPool, O_DESCRIPTOR_POOL)            \
    X(DESTROY, vkDestroyDescriptorPool, VkDescriptorPool, O_DESCRIPTOR_POOL)                                       \
    X(CREATE, vkCreateShaderModule, VkShaderModuleCreateInfo, VkShaderModule, O_MODULE)                            \
    X(DESTROY, vkDestroyShaderModule, VkShaderModule, O_MODULE)                                                    \
    X(CREATE, vkCreatePipelineLayout, VkPipelineLayoutCreateInfo, VkPipelineLayout, O_PIPELINE_LAYOUT)            \
    X(DESTROY, vkDestroyPipelineLayout, VkPipelineLayout, O_PIPELINE_LAYOUT)                                       \
    X(CREATE, vkCreatePipelineCache, VkPipelineCacheCreateInfo, VkPipelineCache, O_PIPELINE_CACHE)                 \
    X(DESTROY, vkDestroyPipelineCache, VkPipelineCache, O_PIPELINE_CACHE)                                          \
    X(PIPELINES, vkCreateGraphicsPipelines, VkGraphicsPipelineCreateInfo)                                          \
    X(PIPELINES, vkCreateComputePipelines, VkComputePipelineCreateInfo)                                            \
    X(DESTROY, vkDestroyPipeline, VkPipeline, O_PIPELINE)
#define DEFINE(how, ...) how(__VA_ARGS__)
CENSUS(DEFINE)
#define HOOK(how, f, ...) real_##f = vk.f; if (real_##f) vk.f = census_##f;
void vka_census_install(void) { CENSUS(HOOK) }

void vka_census_line(void) {
    char left[512] = "", made_[768] = "";
    size_t nl = 0, nm = 0;
    for (int k = 0; k < O_KINDS; k++) {
        const int64_t l = atomic_load(&g_live[k]), m = atomic_load(&g_made[k]);
        if (l && nl < sizeof left) nl += (size_t)snprintf(left + nl, sizeof left - nl, "%s %lld %s", nl ? "," : "", (long long)l, g_kind_name[k]);
        if (m && nm < sizeof made_) nm += (size_t)snprintf(made_ + nm, sizeof made_ - nm, "%s %lld %s", nm ? "," : "", (long long)m, g_kind_name[k]);
    }
    if (nl) VRED("the teardown left Vulkan objects:%s (made in this run:%s)", left, made_);
    else VLOG("the teardown left no Vulkan objects (made in this run:%s)", nm ? made_ : " none");
}

// The shutdown should reach nvn_direct_park within seconds of the stop: if it does not, a line
// says so.
static void* watchdog_main(void* arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "vk-shutdown");
    for (int i = 0; i < 50; i++) {
        if (atomic_load(&g_parked)) return NULL;
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }
    VRED("shutdown: the device not released 5 s after the stop (Lichii's shutdown waits elsewhere)");
    return NULL;
}

void nvn_direct_stop(void) {
    if (vka_stopping()) return;
    vka_stop();
    vka_worker_wake();
    nvn_direct_present_wake();
    VLOG("the emulator stops: nothing more is replayed or submitted, every fence reads as done");
    vka_thread(watchdog_main, NULL, NULL);
}

void nvn_direct_park(void) {
    nvn_direct_stop();
    atomic_store(&g_parked, 1);
    // Bounded: a thread stuck in the driver must not keep the emulator from shutting down. The
    // device's own wait for idle follows (RendererVulkan's destructor).
    const uint64_t bound = 3000000000ull;
    const uint64_t t0 = now_ns();
    const int worker = vka_worker_idle(bound);
    const int builds = vka_pso_park(bound);
    const int translations = vka_rc_park(bound);
    const int completion = worker && builds && translations && vka_complete_stop(bound);
    int running = 0;
    const int left = completion && threads_leave(bound, &running);
    const double ms = (now_ns() - t0) / 1e6;
    if (left) {
        VLOG("parked for the shutdown in %.0f ms: the replay worker is idle, no pipeline is being built or shader translated, the pipeline cache is saved, every thread has left", ms);
        return;
    }
    VRED("parked for the shutdown in %.0f ms: the replay worker %s, pipeline builds %s, speculative translations %s, the completion thread %s, %d threads %s; the device is still in use: the process exits", ms,
         worker ? "is idle" : "is still replaying", builds ? "stopped" : "still running", translations ? "stopped" : "still running",
         !(worker && builds && translations) ? "was not stopped" : completion ? "has left" : "is still running", running, completion ? "still running" : "not asked to leave");
    // Not abort or exit: the replay's own fatal exit (dm_host_shutdown's abort) has been seen to hang
    // the process, and exit handlers and destructors would destroy the device under those threads.
    _exit(1);
}

// nvn_reset.c, before the reset: a boot that ended without nvn_direct_park (it failed before its
// renderer adopted the device) may have left the stop's watchdog. 1 = no thread is left
int nvn_direct_threads_left(void) {
    atomic_store(&g_parked, 1);
    int running = 0;
    return threads_leave(3000000000ull, &running);
}

void nvn_direct_destroy(void) {
    const uint64_t t0 = now_ns();
    VLOG("the device is idle: the replay's objects go");
    if (vka_ok()) vka_destroy();
    nvn_hle_destroy();
    nvn_tracking_destroy();
    VLOG("the replay's objects and the front end's mappings destroyed in %.0f ms", (now_ns() - t0) / 1e6);
}
