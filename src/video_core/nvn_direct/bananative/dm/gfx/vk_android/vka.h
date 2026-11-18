// From Banana Native (github.com/hugorosario/BananaNative, Android port of github.com/seedds/nxnative by seedds), MIT: see LICENSE in src/video_core/nvn_direct/bananative.
// Internal interface of the NVN → Vulkan replay (dm/gfx/vk_android): the device (vka_dev.c), placed guest memory and pools (vka_mem.c), textures and descriptor tables (vka_tex.c),
// the command replay (vka_exec.c) and the present (vka_present.c). They stand in for nvn_mtl.m behind the mtl_* interface hle_nvn.c calls (nvn_mtl.h, nvn_obj.h).
// Vulkan comes from the headers in dm/third_party (newer than the NDK's 1.3.275: VK_EXT_map_memory_placed, VK_KHR_present_wait2); every entry point is loaded through vkGetInstanceProcAddr
#pragma once
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan_core.h>
#include <android/native_window.h>
#include <vulkan/vulkan_android.h>
#include <pthread.h>
#include <stdatomic.h>
#include "../nvn/nvn_obj.h"
#include "../../host/dm.h"
#include "nvn_direct.h"   // Lichii: NVN_DIRECT_MAX_QUEUES

#define VLOG(...) LOG("[vk] " __VA_ARGS__)
#define VRED(...) LOG("[RED] vk: " __VA_ARGS__)
#define VRED_ONCE(...) do { static _Atomic int once_; if (!atomic_exchange(&once_, 1)) VRED(__VA_ARGS__); } while (0)

#define VKA_GLOBAL_FNS(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties) X(vkEnumerateInstanceVersion)
#define VKA_INSTANCE_FNS(X) \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures) X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceImageFormatProperties) X(vkGetPhysicalDeviceSparseImageFormatProperties) \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) \
    X(vkCreateAndroidSurfaceKHR) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define VKA_DEVICE_FNS(X) \
    X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkQueueBindSparse) X(vkGetImageSparseMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkMapMemory2KHR) X(vkUnmapMemory2KHR) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkGetBufferDeviceAddress) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkGetImageMemoryRequirements2) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue) X(vkSignalSemaphore) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) X(vkCmdUpdateBuffer) X(vkCmdFillBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
    X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdResolveImage) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdClearAttachments) \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) \
    X(vkCreateDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) \
    X(vkGetRefreshCycleDurationGOOGLE) X(vkGetPastPresentationTimingGOOGLE) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkDestroyPipeline) X(vkCreatePipelineCache) X(vkGetPipelineCacheData) \
    X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkResetDescriptorPool) X(vkCreateBufferView) X(vkDestroyBufferView) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers2) X(vkCmdBindIndexBuffer) \
    X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdDispatch) X(vkCmdDispatchIndirect) X(vkCmdDispatchBase) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetPrimitiveTopology) \
    X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetDepthBoundsTestEnable) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) X(vkCmdSetDepthBias) X(vkCmdSetDepthBiasEnable) \
    X(vkCmdSetRasterizerDiscardEnable) X(vkCmdSetPrimitiveRestartEnable) X(vkCmdSetBlendConstants) X(vkCmdSetLineWidth) \
    X(vkCmdPushDescriptorSetKHR) X(vkCmdSetVertexInputEXT) X(vkCmdPushConstants) X(vkCmdDrawMeshTasksEXT) \
    X(vkCmdDrawMeshTasksIndirectEXT) X(vkCmdDrawMeshTasksIndirectCountEXT) \
    X(vkCmdDrawIndirectCount) X(vkCmdDrawIndexedIndirectCount) X(vkCmdSetDepthBounds) \
    X(vkGetMemoryHostPointerPropertiesEXT) X(vkGetMemoryFdPropertiesKHR)   /* Lichii: pools imported from guest memory (vka_mem.c) */ \
    X(vkDestroyCommandPool)   /* Lichii: a frame capture copy's pool, once the GPU finished it late (vka_capture.c vcap_late_reap) */ \
    X(vkDestroyDescriptorSetLayout) X(vkDestroyDescriptorPool) X(vkDestroyPipelineCache)   /* Lichii: the teardown (vka_dev.c vka_destroy) */
#define VKA_DECL(f) PFN_##f f;
typedef struct { PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr; VKA_GLOBAL_FNS(VKA_DECL) VKA_INSTANCE_FNS(VKA_DECL) VKA_DEVICE_FNS(VKA_DECL) } vka_fns;
extern vka_fns vk;   // a missing optional entry point is NULL

// ---------- Switches read again while the game runs (vka_ctl.c) ----------
// VKA_ENV("DM_VK_<NAME>", def) is a DM_VK_* switch's value as a number (strtod; unset or empty = def), cached per call site: one relaxed load and a compare while nothing changed.
// A live "env KEY=VALUE" control line (android/TESTING.md "Live control") bumps vka_env_gen at a frame boundary and every call site reads its switch again on its next call.
// Read switches through it, not getenv: a switch that cannot change while running (it sizes a device object, a cache) goes into vka_ctl.c's restart-only table
// A call site can run on two threads at once (the game's and the replay worker's): v is atomic (no torn read), and only one thread at a time refreshes a cache, and only to a
// newer generation (vka_env_slow), so a thread that read the value before a change cannot store it over a newer one while gen says current
typedef struct { _Atomic uint32_t gen; _Atomic double v; } vka_envc;
extern _Atomic uint32_t vka_env_gen;
double vka_env_slow(vka_envc *c, const char *key, double def);
static inline double vka_env_c(vka_envc *c, const char *key, double def) {
    return atomic_load_explicit(&c->gen, memory_order_acquire) == atomic_load_explicit(&vka_env_gen, memory_order_relaxed) ? atomic_load_explicit(&c->v, memory_order_relaxed)
                                                                                                                         : vka_env_slow(c, key, def);
}
#define VKA_ENVF(key, def) ({ static vka_envc c_; vka_env_c(&c_, key, def); })
#define VKA_ENV(key, def) ((int)VKA_ENVF(key, def))
int vka_env_str(const char *key, char *out, size_t n);   // a string switch as live control sees it (1 = set); not cached: for switches read rarely
void vka_floor_span(uint64_t frame, uint64_t ns);   // vka_exec.c: a submission of frame ended ns after the frame began (the adaptive floor)
void vka_floor_stats(char *b, size_t n);
void vka_floor_commit(int counted);   // DM_VK_GPU_FLOOR_AUTO=2: a submission counted by vka_floor_tail_add handed to the queue / done
void vka_floor_done(int counted);
void vka_gpu_floor(void);               // mtl_sub_begin: DM_VK_GPU_FLOOR's GPU clock floor on the queue (vka_dev.c)
void vka_ctl_sub(void);                 // mtl_sub_begin: live switches queued since the last frame are applied
void vka_ctl_frame(uint64_t f);         // mtl_set_frame: per-frame counters for "capture" and "stats"
void vka_exec_counts(uint64_t *ops, uint64_t *subs, uint64_t *rec_ns);   // vka_exec.c: clears / copies, submissions, recording time so far

// ---------- Device (vka_dev.c) ----------
typedef struct {
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qf;
    VkPhysicalDeviceProperties props; VkPhysicalDeviceMemoryProperties memp;
    int placed, placed_range, unmap_reserve, bda, uab, display_timing, has_android_surface;
    // Lichii: memory pools are guest memory imported with VK_EXT_external_memory_host (vka_mem.c import_seg), not placed maps; host_import_types: the memory types an
    // import may take (vkGetMemoryHostPointerPropertiesEXT of a guest page), placed_align then the import alignment (minImportedHostPointerAlignment)
    int host_import; uint32_t host_import_types;
    // Lichii: udmabuf: the driver refuses host-pointer imports of Lichii's shared memfd memory (amdgpu), so the pools are udmabufs of the memfd's pages
    // (udmabuf_fd: /dev/udmabuf) imported with VK_EXT_external_memory_dma_buf, at most udmabuf_max bytes each (udmabuf's size_limit_mb) (vka_mem.c vka_mem_import_choose)
    int udmabuf, udmabuf_fd; VkDeviceSize udmabuf_max;
    void *perf_hint;                                // vkQueueSetPerfHintQCOM (VK_QCOM_queue_perf_hint), NULL without it: vka_gpu_floor
    // The draw path's device facts: push descriptors (and how many a set may hold), vertex input dynamic state, robustness2's null descriptors, 8-bit indices,
    // instance-rate divisors, logic operations, dual-source blending, geometry and tessellation stages, depth clamp, fill mode lines, wide lines
    int push_desc, vin_dyn, null_desc, idx8, divisor, logic_op, dual_src, geom, tess, depth_clamp, fill_lines, depth_bias_clamp, multi_vp, a2c, depth_bounds, draw_indirect_count, multi_draw_indirect;
    // NVN2 task and mesh programs (VK_EXT_mesh_shader: mesh = meshShader, task = taskShader too; DM_VK_MESH=0 leaves it off): their shaders are refused without it (vka_shader.c refusal)
    // and their draws skipped. mesh_inv / task_inv: the largest workgroup (maxMeshWorkGroupInvocations / maxTaskWorkGroupInvocations); mesh_out_v / mesh_out_p: the most vertices /
    // primitives a mesh workgroup may output; task_payload: maxTaskPayloadSize; task_groups / mesh_groups: maxTaskWorkGroupCount / maxMeshWorkGroupCount (a draw's counts, per dimension)
    int mesh, task; uint32_t mesh_inv, task_inv, mesh_out_v, mesh_out_p, task_payload, task_groups[3], mesh_groups[3];
    int gpl;                                        // graphics pipeline libraries with fast linking (VK_EXT_graphics_pipeline_library): a pipeline the cache lacks is linked from stage libraries at once, the optimized one made in the background (vka_pso.c)
    uint32_t max_push, max_vin_bindings, subgroup_size, subgroup_min, subgroup_max, subgroup_stages;   // subgroup_stages: VkShaderStageFlags with subgroup operations
    uint32_t subgroup_req_stages;                   // VkShaderStageFlags a pipeline may pin the subgroup size in (requiredSubgroupSizeStages; 0 without subgroupSizeControl)
    // The driver (VkPhysicalDeviceDriverProperties.driverID: VK_DRIVER_ID_QUALCOMM_PROPRIETARY, VK_DRIVER_ID_MESA_TURNIP, ...). A per-driver default keys on this and on
    // feature queries, never on the driver's name; each such default keeps its DM_VK_* switch (android/README.md "Custom GPU drivers")
    int driver_id;
    VkDeviceSize ub_align, sb_align, ub_range, sb_range;
    VkDeviceSize placed_align, max_alloc;
    uint32_t guest_type, local_type, upload_type;   // guest RAM (DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT|HOST_CACHED, or the nearest), images, upload arenas
    VkSemaphore tl;                                 // the timeline: value n = submission n has completed
    // NVN queues as Vulkan queues: qs[0] = q; with multi, each submission signals its queue's timeline qtl[i] and the completion thread
    // signals tl in fence order once the queue's value is reached (submissions on different queues complete in any order; tl stays in order for everything that waits on it)
    VkQueue qs[NVN_DIRECT_MAX_QUEUES]; VkSemaphore qtl[NVN_DIRECT_MAX_QUEUES]; uint64_t qseq[NVN_DIRECT_MAX_QUEUES]; int nq, multi;
    pthread_mutex_t qmu;                            // the queue is shared by the submit and present threads
    int sparse_res;                                 // sparse residency images can be made (vka_sparse.c): binding on the queue, 2D residency, standard block shape, strict non-resident reads
    uint64_t tl_queued;                             // the highest timeline value a vkQueueSubmit so far signals (under qmu): what a later submission on the queue may wait for
    // Lichii, each 0 without the device extension Lichii enabled: conservative rasterization (cons_max: maxExtraPrimitiveOverestimationSize), the last
    // vertex as the provoking one, NV viewport swizzles, NV exclusive scissors, a fragment shading rate attachment (VRS) of 16x16-pixel texels, debug utils labels
    int cons, provoking_last, vp_swizzle, excl_scissor, vrs; float cons_max;
    PFN_vkCmdSetExclusiveScissorNV set_excl_scissor;
    PFN_vkCmdBeginDebugUtilsLabelEXT label_begin; PFN_vkCmdEndDebugUtilsLabelEXT label_end; PFN_vkCmdInsertDebugUtilsLabelEXT label_insert;
} vka_dev;
extern vka_dev V;
// Custom GPU drivers (vka_driver.c): DM_VK_DRIVER=<folder with meta.json, or a .so> loads that driver through libadrenotools (restart only); unset, empty or "system" = the
// system libvulkan.so. vka_driver_open returns the libvulkan handle (NULL = use the system one); vka_driver_tag is a file-name-safe name of the custom driver in use, NULL for the system's
void *vka_driver_open(void);
const char *vka_driver_tag(void);
int vka_init(void);   // 1 = the device is up (idempotent, any thread); 0 = no Vulkan: every op logs and does nothing
int vka_ok(void);
uint32_t vka_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid);
void vka_mem_import_choose(int dmabuf);   // Lichii: with host_import, whether pools are host-pointer imports or udmabufs (vka_mem.c); dmabuf: VK_EXT_external_memory_dma_buf is there
VkResult vka_submit_q(int q, VkCommandBuffer cb, uint64_t signal, uint32_t nwait, const VkSemaphore *wait, const uint64_t *wait_vals, const VkPipelineStageFlags *stages,
                      uint32_t nsig, const VkSemaphore *sig_sems, const uint64_t *sig_vals);   // on queue q, with extra timeline signals; vka_submit = queue 0
uint64_t vka_up_last(void);   // the upload timeline's last batch value (a submission on another queue waits for it)
VkSemaphore vka_up_timeline(void);
VkResult vka_submit(VkCommandBuffer cb, uint64_t signal, uint32_t nwait, const VkSemaphore *wait, const uint64_t *wait_vals, const VkPipelineStageFlags *stages, VkSemaphore bin_signal);

// The fence ledger: fence n is "done" once the GPU passed it and its CPU-side effects (events, counters) have run
uint64_t vka_fence_next(void);                      // reserve the next fence value (submit thread)
uint64_t vka_fence_last(void);                      // the last fence reserved (its submission may still be recording on another thread)
uint64_t vka_fence_queued(void);                    // the last fence handed to the queue: the most a submission (the present's) may wait for without a wait-before-signal on its own queue
int vka_fence_queue(uint64_t fence, int *q, VkSemaphore *sem, uint64_t *val);   // Lichii (V.multi): the Vulkan queue a fence's submission went to and its timeline value there; 0: none known
// The replay (vka_exec.c), entered by the replay worker (vka_worker.c) or inline by the mtl_* calls of the submitting thread
void vka_rp_sub_begin(void);
void vka_rp_sub_wait(uint64_t fence);                // Lichii, after vka_rp_sub_begin: the submission's first part waits for the submission of that fence when it went to another Vulkan queue
void vka_rp_exec(const uint8_t *handle, uint64_t fence);   // fence: the submission's, reserved before it (0 = taken at its commit)
uint64_t vka_rp_sub_commit(uint64_t reserved);
void vka_rp_queue_activate(uint64_t serial);
void vka_rp_queue_forget(uint64_t serial);
void vka_rp_event(volatile uint32_t *val, int mode, uint32_t v);   // a CPU signal, in replay order (the worker's FIFO)
void vka_rp_set_queue_origin(int on);
int vka_worker_on(void);                              // DM_VK_WORKER (default 1): submissions are replayed on the replay worker thread
uint64_t vka_present_reserve(void);
void vka_present_fill(uint64_t tk, const tex_t *t, double period_s, const int crop[4]);
uint64_t vka_present_inline(const tex_t *t, double period_s, const int crop[4]);
uint64_t vka_hang_ns(void);                         // how long a fence may make no progress before it is a hang (DM_VK_HANG_S, default 6 s)
int vka_hung(void);
// Lichii: the emulator shuts down (nvn_direct_stop / nvn_direct_park in nvn_shutdown.c)
void vka_up_decoded(void);                          // vka_dev.c (Lichii): an ASTC decode went into the open upload batch
void vka_heap_line(char *o, size_t n);               // vka_dev.c (Lichii): the driver's use and budget per heap, MiB
int vka_dev_ext(const char *name);                  // vka_dev.c (Lichii): the host's device has the extension enabled
int vka_async_pipelines(void);                     // vka_dev.c: Lichii's asynchronous shader building setting (nvn_direct_device.async_pipelines)
void vka_pipeline_notify(int built);                // vka_dev.c: a pipeline made off the frame path starts (0) or is done (1): the host's "building shaders"
void vka_stop(void);                                // vka_dev.c: as after a hang nothing more is submitted and every fence reads as done, silently; waits return at once
int vka_stopping(void);
// Lichii: every thread the replay and the front end start is joined when the game stops (nvn_shutdown.c), so the next game starts them again: vka_thread starts fn(arg)
// (0 = started); wake, while the shutdown waits for the thread to leave, wakes its waits (NULL: it leaves by itself); each thread returns once vka_leaving()
int vka_thread(void *(*fn)(void *), void *arg, void (*wake)(void));
int vka_leaving(void);
void vka_worker_wake(void);                         // vka_worker.c: the game's waits for room in the worker's queue see the stop
int vka_worker_idle(uint64_t timeout_ns);           // the worker has replayed every job queued (1), or timed out (0)
int vka_complete_stop(uint64_t timeout_ns);         // vka_dev.c (Lichii): the completion thread has run what was submitted and left (1), or timed out (0)
void vka_bindless_init(void);                       // vka_tex.c: the bindless set and its layout (every pipeline layout holds it)
void vka_pso_start(void);                           // vka_pso.c: the pipeline cache opened, the prewarm and the compute pipelines made ahead started
int vka_pso_park(uint64_t timeout_ns);              // vka_pso.c: no pipeline built from now on, the last save made; 1 = no build was in progress at the end
int vka_queue_idle(void);                           // vkQueueWaitIdle bounded by the hang time (0 = not idle: hung or timed out)                                 // 1 = a GPU hang was declared (vka_dev.c): nothing more is submitted, waits return at once
void vka_fence_post(uint64_t fence, void *ops, int n, void (*run)(void *ops, int n));   // effects run by the completion thread when fence completes
uint64_t vka_done(void);
void vka_fence_cpu(uint64_t fence, void *ops, int n, void (*run)(void *ops, int n));   // no GPU behind it (no device, a failed submit): the effects run now and fence is done
int vka_wait_done(uint64_t fence, uint64_t timeout_ns);
int vka_wait_fence(uint64_t fence, uint64_t timeout_ns);   // fence itself done (vka_dev.c): with several queues, maybe before an earlier fence of another queue
void vka_after(uint64_t fence, void (*fn)(void *), void *arg);   // deferred destruction: fn(arg) once fence is done

// Host-visible scratch memory for one submission (uploads, deswizzled texels, readbacks); valid until the submission's fence
typedef struct { VkBuffer buf; uint8_t *ptr; VkDeviceSize off; } vka_span;
int vka_scratch(VkDeviceSize n, VkDeviceSize align, vka_span *out);
void vka_scratch_retire(uint64_t fence);            // everything handed out since the last call belongs to fence
// Upload batches (vka_dev.c): a new image's first content recorded into a command buffer of its own, whose staging retires on its own completion. Replay thread only.
// vka_up_begin: the open batch (begun when none; NULL = none, record into the submission), and vka_scratch on this thread takes the batch's staging until vka_up_end.
// vka_up_point: in an upload's loop, cb itself, or, when cb is the batch and its staging has passed the batch budget, the batch sent and the next one (NULL: none could
// be begun, the upload stops). vka_up_cur: after such a loop, the open batch when cb was a batch. vka_up_flush: the open batch to the queue (before every part or
// submission the replay hands to the queue)
extern _Thread_local int vka_scr_up;
int vka_up_on(void);
VkCommandBuffer vka_up_begin(void);
void vka_up_end(void);
VkCommandBuffer vka_up_point(VkCommandBuffer cb);
VkCommandBuffer vka_up_cur(VkCommandBuffer cb);
int vka_up_is(VkCommandBuffer cb);   // cb is an upload batch's
void vka_up_flush(void);
void vka_up_throttle(void);                         // waits while more than DM_VK_UPLOAD_INFLIGHT_MB of batch staging is in flight (no lock held)
void vka_up_stats(uint64_t *batches, uint64_t *waits, double *wait_ms);
VkCommandBuffer vka_rp_cb(void);                    // the replay's command buffer being recorded (vka_exec.c; NULL: none)
// One "memory" line each time the replay's own memory (placed pools, images, scratch) has taken DM_VK_MEMLOG MiB more (allocations counted, frees not) (default 512, 0: none) since the last one: where the memory
// went in a game that the system stops before its first frame (Mario Kart World 1.8.0 on a 16 GB phone). Values read without the owners' locks (a log line, called under them)
void vka_mem_grew(uint64_t bytes);
uint64_t vka_scratch_bytes(uint32_t *blocks);
uint64_t vka_mem_placed_nolock(void);
uint64_t vka_tex_mem_nolock(int *n);

// Sparse residency images (vka_sparse.c): memory only behind the 64 KB tiles a sparse pool's texture has mapped
typedef struct vks_img vks_img;
int vks_on(void);                                   // the device can (V.sparse_res), on NVIDIA by default (DM_VK_SPARSE_RES=0/1 overrides)
int vks_va_take(uint64_t bytes);                    // sparse address space for an image of that virtual size: 1 = taken
void vks_va_give(uint64_t bytes);
uint64_t vks_va_free(void);                         // sparse address space free now (a texture left at a coarser level tries a finer one once more is free than when it failed)
vks_img *vks_new(VkImage img, uint32_t type, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers, uint32_t tw, uint32_t th, uint32_t tiled);   // NULL: make it dense
int vks_tile(vks_img *s, uint32_t lv, uint32_t ly, uint32_t tx, uint32_t ty, int want);   // bind / unbind one tile of image level lv: 1 = bound now
int vks_tile_bound(vks_img *s, uint32_t lv, uint32_t ly, uint32_t tx, uint32_t ty);
uint32_t vks_tiled(const vks_img *s);               // image levels bound tile by tile (the later ones are bound whole)
uint64_t vks_img_bytes(vks_img *s);
VkSemaphore vks_timeline(uint64_t *last);           // the sparse timeline and the last value anything handed to the queue signals on it (Lichii)
void vks_free(vks_img *s);                          // img_destroy, before vkDestroyImage
void vka_qlock(void); void vka_qunlock(void);   // Lichii: around every submission to the replay's queues (vka_dev.c: Lichii's submit lock)
void vks_queue_locked(uint32_t *nw, VkSemaphore *ws, uint64_t *wv, VkPipelineStageFlags *wst);   // before a vkQueueSubmit of the replay or an upload batch, under V.qmu
void vks_stats(char *b, size_t n);
uint64_t vks_bytes_nolock(void);

// ---------- Guest memory (vka_mem.c) ----------
// Guest RAM is anonymous memory; each NVN memory pool becomes placed Vulkan memory at its own address when it is made (mtl_pool_add) and anonymous again when it goes (mtl_pool_del):
// the CPU and GPU see the same bytes. A placed range is a segment with one VkBuffer over it
int vka_addr(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off, VkDeviceAddress *bda);   // 1 = [addr, addr + len) lies in one placed segment
int vka_mem_release(uintptr_t lo, uintptr_t hi);    // the host gives [lo, hi) back: segments there are given back first; 0 = the host releases the range itself
uint64_t vka_addr_run(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off);   // bytes from addr (at most len) in the segment that holds addr, 0 = none
int vka_pool_bda(uint64_t addr, VkDeviceAddress *bda);   // the device address of a pool byte (the table the binding side reads)
void *vka_mem_mapped(size_t n);   // no placed maps: a mapped allocation as a segment where the driver maps it (the selftest on an emulator)
int vka_mem_chunks(uint64_t *lo, uint64_t *hi, uint64_t *bda, int max);   // the placed segments by address (guest range and device address); the count
uint64_t vka_mem_placed_bytes(uint64_t *peak);     // bytes of placed Vulkan memory now (and the peak)

// ---------- Textures (vka_tex.c) ----------
typedef struct {
    VkFormat vk;           // the image's own format (8-bit sRGB formats are stored as their UNORM twin: the present and copies move raw bytes, views add the sRGB decode)
    VkFormat view;         // the format views and clears use
    uint8_t kind;          // 0 float / unorm / snorm, 1 uint, 2 sint, 3 depth, 4 stencil, 5 depth-stencil
    uint8_t srgb;          // stored as UNORM, cleared with an sRGB-encoded colour
    uint8_t bpb, bw, bh;
    uint8_t astc_bw, astc_bh;   // an ASTC texture on a device without ASTC: the storage's block size; the image is BC3 (Lichii: BC1 when opaque and never in a texture copy, vka_tex.c astc_bc1) filled on the GPU or by vka_astc_transcode (0: not such a texture)
} vka_fmt;
int vka_fmt_of(int nvn_format, vka_fmt *f);         // 0 = no Vulkan format (logged once per format)
// ASTC blocks (w x h x depth texels, bw x bh blocks, rows of blocks packed) transcoded to BC3 (rows of 4x4 blocks packed, 16 bytes each) on the CPU, for a device without
// ASTC: BC3 keeps an ASTC 4x4 texture's size (RGBA8 took 10.5 GiB of images for Mario Kart World's race and ran out of device memory). A weak hook the host may supply
// (a trace replay harness does); without it ASTC textures have no image there. 0 = done
// A texture filled from storage, level lv layer ly deswizzled into lin (pitch bytes a row, n bytes): a weak hook the host may supply to replace those bytes (a trace replay
// harness stands in for GPU-made state a trace does not hold: Mario Kart World's boot-time colour-grading LUTs)
void vka_upload_hook(const tex_t *t, uint32_t lv, uint32_t ly, uint8_t *lin, size_t pitch, size_t n) __attribute__((weak));
int vka_upload_claim(const tex_t *t) __attribute__((weak));   // 1 = the host fills this texture (vka_upload_hook): uploaded from storage even where it would start cleared
int vka_astc_transcode(const uint8_t *blocks, uint32_t w, uint32_t h, uint32_t depth, uint32_t bw, uint32_t bh, uint8_t *bc3) __attribute__((weak));
int vka_astc_gpu_on(void);   // the host decodes ASTC on the GPU (Lichii: nvn_direct_device.astc_to_bc; vka_astc_gpu records one level, vka_dev.c)
int vka_astc_opaque(const void *blocks, size_t n);   // the ASTC blocks decode without alpha (Lichii: nvn_direct_device.astc_opaque; 0 without the host's check)
int vka_mem_budget(uint64_t *usage, uint64_t *budget);   // the host's device-local memory use and budget (vka_dev.c; 0 = none given)
void vka_tex_budget(void);   // images let go of when the device-local use nears the budget (vka_tex.c; the replay thread, between submissions)
// An image's memory (vka_imgmem.c): space in a shared block (blk, at off, units of 256 bytes) or a dedicated allocation (blk NULL, off 0); size = its memory requirements' size
// z: zero-shared memory (vka_imem_bind_zero; dm is the shared allocation, freed with its last image)
typedef struct { VkDeviceMemory dm; uint64_t off, size; void *blk; uint32_t units; int z; } vka_imem;
VkResult vka_imem_bind(VkImage img, uint32_t type, const VkMemoryRequirements *mr, vka_imem *m);   // allocated and bound, or the error with nothing allocated
VkResult vka_imem_bind_zero(VkImage img, uint32_t type, const VkMemoryRequirements *mr, const void *key, uint32_t klen, vka_imem *m, int *fresh);   // bound to the zeroed allocation of its key (vka_zshare.h)
uint64_t vka_imem_zbytes(void); uint64_t vka_imem_zbytes_nolock(void);   // bytes of the zero-shared allocations
void vka_imem_zowned(void);   // a zero-shared image got memory of its own (vka_tex.c img_own): counted for the stats line
void vka_imem_free(vka_imem *m);   // after vkDestroyImage, once the GPU is done with the image
void vka_imem_stats(char *b, size_t n);
uint64_t vka_imem_block_free(uint64_t *block_bytes);   // Lichii: the image blocks' free space, their bytes
double vka_imem_block_use(const void *blk);            // the share of a block its images use
uint64_t vka_imem_allocations(void);   // VkDeviceMemory objects the images hold (blocks + dedicated)
typedef struct vka_img {
    uint64_t serial; const uint8_t *mem; nvn_texdesc d; nvn_texlayout L;
    VkImage img; vka_imem m; vka_fmt f; VkImageUsageFlags usage; VkImageAspectFlags aspect;
    uint32_t w, h, depth, layers, levels, samples; uint64_t bytes;
    int refs, inited, gpu_owned, cpu_dirty;
    int no_storage;          // remaking it with storage use was refused (img_storage_upgrade): not tried again
    int holds;               // references taken from outside the recording thread (vka_tex_acquire / vka_tex_ref: the present, frame generation, capture), which read img unlocked: the image is not remade while held (img_storage_upgrade)
    // Content from guest storage (vka_tex.c storage_mode): track = re-uploaded when the CPU writes its pages (write-protected after each upload, nvn_dirty.c),
    // stamp = the dirty-page generation taken before the last upload, span = the tracked pool range that holds the storage
    int track, wb_pending; uint64_t stamp; const void *span;
    uint64_t hash;           // DM_VK_NANSCAN: a hash of the storage at the last upload (vka_tex_id_check)
    uint64_t gw_seen; int from_mem; uint64_t head, up_fence; uint8_t *snap;   // head, up_fence: DM_VK_NANSCAN's storage watch   // the newest GPU write into guest memory its last upload included (vka_tex.c gw_t); content from guest storage (ST_TRACK / ST_FILL)
    // In a sparse virtual pool (hle_nvn.c nvn_sparse_*): the storage is read through the pool's 4 KB page table, never at mem (unmapped pages there are inaccessible);
    // vtrack = re-uploaded when a mapping in its range or a physical page behind it changes, vgen = the page table's generation at the last upload
    const void *vpool; int vtrack; uint64_t vgen;
    int vbind;               // a GPU-written sparse texture's residency image (vka_tex.c sparse_gpu): its tiles follow the mappings (sparse_bind), nothing uploaded
    // base: the texture's finer levels left out of the image (vka_tex.c sparse_base): image level 0 is the texture's level base, made only of the levels where pages are mapped.
    // Texture levels in views, render targets and copies go through vka_img_lv
    // vmin: the texture level its sampled views start at (vka_tex.c sparse_base: the finest level from which every level is wholly mapped)
    uint32_t base, vmin;
    // A sparse residency image (vka_sparse.c): memory only behind its mapped 64 KB tiles (rs_tw x rs_th texels), va bytes of the sparse address space; bfloor: the finest level it
    // may start at (levels left out to fit the address space, vka_tex.c img_create), a hint kept while vks_va_free() is less than bneed (the address space the next finer start asked for when it did not fit)
    struct vks_img *rs; uint32_t rs_tw, rs_th, bfloor; uint64_t va, bneed;
    void *views;             // the image views made of it (vka_view)
    int any_fmt;             // DM_VK_UBWC=0: MUTABLE_FORMAT without a list, views of any format
    VkFormat alt;            // the one other format its views may take (its sRGB twin; img_create's format list), or VK_FORMAT_UNDEFINED: views only in its own format
    VkFormat xfmt[4]; int nx; // further formats its format list takes: ones textures over the same memory were seen to view it in (vka_tex.c, learned view formats)
    uint8_t mode;            // its content at first use (vka_tex.c ST_ZERO / ST_FILL / ST_TRACK)
    uint8_t pool_fit;        // its memory requirements fit its storage in the pool (vka_tex_alias_stats): type, size and alignment
    // pool: the texture's pool at the image's creation, pgen its Initialize serial then (hle_nvn.c nvn_pool_live; 0: none or not live); pfin: nvn_pool_fin at the last check;
    // pdead: that pool was finalized since (vka_tex.c img_pool_dead): its storage is not read again
    const void *pool; uint64_t pgen, pfin; int pdead;
    uint8_t dcap;            // a sparse texture's dense image whose finer levels DM_VK_SPARSE_DENSE_MB left out (vka_tex.c dense_floor)
    struct vka_img *zold;    // the zero-shared image it left while a holder had it (vka_tex.c img_own): destroyed once holds drop to 0
    // Large images (DM_VK_BIGIMG_MB, vka_tex.c "Large images"): draws that bound it for sampling, other textures found over its storage (the first serials)
    uint32_t nsamp, novl; uint64_t ovl[4];
    uint64_t used;           // the budget's tick (vka_tex.c vka_tex_budget) at which a draw, copy or view last took it
    uint8_t scaled;          // Lichii: made at the render scale (vka_scale_on): w, h are the texture's scaled, d.width / d.height its own size
    uint64_t drawn[2];       // Lichii: the last two frames (presents filled, plus 1) a pass drew into it, the newest first (vka_draw.c drawn_again)
} vka_img;
// Texture level lv as a level of im (a sparse texture's image starts at level im->base): 0 when the image does not hold it
static inline int vka_img_lv(const vka_img *im, uint32_t *lv) { if (*lv < im->base) return 0; *lv -= im->base; return *lv < im->levels; }
// The layers a render target view of level lv can name: a 3D texture's slices at that level (drawn into through 2D / 2D-array views, VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT), else the array layers
static inline uint32_t vka_img_slices(const vka_img *im, uint32_t lv) { return im->d.target == NVN_TEXTURE_TARGET_3D ? (im->depth >> lv ? im->depth >> lv : 1) : im->layers; }
vka_img *vka_tex_get(const tex_t *t, VkCommandBuffer cb);   // the texture's image, made (and filled from guest memory, or cleared) on first use in cb; NULL = no image
// Lichii: the render scale, Lichii's resolution setting (nvn_direct_device scale_up / scale_shift): render targets (GPU content, 2D, attachable,
// see vka_tex.c img_create_) are made scale_up / 2^scale_shift times their size; passes over them, their viewports, scissors, clears and copies follow, and recompiled
// shaders read the size through Lichii's RescalingPass (push constants, vka_draw.c). 1, 0: no scale, nothing of it runs
extern uint32_t vka_scale_up, vka_scale_shift;
static inline int vka_scale_on(void) { return vka_scale_up != 1 || vka_scale_shift; }
static inline int32_t vka_scaled(int32_t v) { return (int32_t)(((int64_t)v * vka_scale_up) >> vka_scale_shift); }   // a coordinate
static inline uint32_t vka_scaled_size(uint32_t v) { uint32_t s = (uint32_t)(((uint64_t)v * vka_scale_up) >> vka_scale_shift); return v && !s ? 1 : s; }   // a size (Settings' ScaleUp)
// A scaled image made at the texture's own size again (its content resampled into it): for uses the scale cannot follow (CPU-visible bytes, shader images, a pass with targets of the other
// kind). 0 = it stays scaled (held by a reader outside recording, or no memory). Any thread that records; no render pass may be open in cb
int vka_tex_unscale(vka_img *im, VkCommandBuffer cb, const char *why);
int vka_tex_astc_copy(vka_img *src, vka_img *dst, VkCommandBuffer cb);   // Lichii: a texture copy between ASTC textures keeps one BC block size on both sides (vka_tex.c); 0 = skip it
uint64_t vka_tex_mem(int *n, uint64_t *gpu_written);   // bytes of image memory now: n images, of which gpu_written bytes the GPU wrote (the rest copies pool storage)
void vka_tex_alias_stats(char *b, size_t n);        // image bytes by content (cleared, filled once, tracked) and how many of them could live on their pool storage
vka_img *vka_tex_acquire(uint64_t serial);          // a reference that keeps the image alive (the present thread), dropped with vka_tex_drop
void vka_tex_drop(vka_img *im);
int vka_tex_live_refs(vka_img **out, int max);       // references to every live image (the replayer's --dump-gpu), each dropped with vka_tex_drop
vka_img *vka_tex_ref(vka_img *im);                  // one more reference to im (dropped with vka_tex_drop)
int vka_img_lists(vka_img *im, VkFormat v);         // 1 = im's format list takes views in v (a texture of another format sharing it: learned view formats)
// The GPU writes im (render target, clear, copy destination, storage image): its content is the GPU's from now on (no re-upload from storage), and a linear texture's level 0
// is copied back into its storage when the submission ends (vka_tex_writeback), where the CPU reads it as it would on the Switch
void vka_img_written_at(vka_img *im, const char *file, int line);
#define vka_img_written(im) vka_img_written_at(im, __FILE_NAME__, __LINE__)   // (the site is logged for a large image's first write, DM_VK_BIGIMG_MB)
// Before a write is recorded into im (an attachment, a clear, a copy destination, a storage image): a zero-shared image gets memory of its own, cleared (vka_tex.c img_own).
// Call it before taking a view of im for the write. Any thread that records (takes the table lock); no render pass may be open in cb unless upload batches are on
void vka_tex_own(vka_img *im, VkCommandBuffer cb);
void vka_tex_big_stats(char *b, size_t n);           // the largest images the GPU has not written, with what sampled them and what lies over their storage (DM_VK_BIGIMG_MB)
int vka_rp_is_open(void);                            // a render pass is open in the replay's command buffer (vka_draw.c)
void vka_tex_writeback(VkCommandBuffer cb);   // mtl_sub_commit, after the last pass
VkImageView vka_view(vka_img *im, VkFormat fmt, VkImageViewType type, VkImageAspectFlags aspect, uint32_t level, uint32_t nlevels, uint32_t layer, uint32_t nlayers, const VkComponentMapping *swz);
void vka_tex_prepare(VkCommandBuffer cb);           // at submit begin: dirty CPU textures re-uploaded, registered descriptors written
void vka_tex_uploads(VkCommandBuffer cb);
VkDescriptorSet vka_bindless_set(void);
VkDescriptorSetLayout vka_bindless_layout(void);
void vka_barrier_at(VkCommandBuffer cb, const char *file, int line);   // a full memory barrier (the replay orders every transfer op after the previous one)
#define vka_barrier(cb) vka_barrier_at(cb, __FILE_NAME__, __LINE__)   // the site names the step that ends there in DM_VK_GPUPROF's report (vka_gprof.c)
// DM_VK_GPUPROF (vka_gprof.c): GPU timestamps between the steps of a profiled frame
extern int vka_gp_active;
void vka_gp_mark(VkCommandBuffer cb, const char *file, int line);
void vka_gp_pass(const char *desc);
void vka_gp_part(VkCommandBuffer cb);
void vka_gp_sub_begin(VkCommandBuffer cb, uint64_t frame);
void vka_gp_commit(VkCommandBuffer cb, uint64_t fence);
void vka_gp_sub_frame(uint64_t f);
void vka_img_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to);
// The control line "screenshot" (vka_ctl.c): the present copies the window texture's crop into its command buffer, then hands the texels to the PNG writer once that submission is done
void vka_ctl_shot_record(VkCommandBuffer cb, vka_img *im, int cx, int cy, int cw, int ch);
void vka_ctl_shot_done(VkFence fence);

VkImageView vka_tex_id_view(int id, VkCommandBuffer cb, uint8_t dim, int storage, VkImageLayout *layout);   // texture ID id as a view of shape dim (DMSH_VK_DIM_*); VK_NULL_HANDLE = none fits
int vka_tex_lazy_pending(void);   // Lichii: texture IDs whose images wait for their first use (vka_tex.c tex_lazy)
void vka_tex_lazy_make(VkCommandBuffer cb);   // their images made now, for a shader that reads the texture pool by index (outside a render pass)
void vka_tex_id_desc(int id, char *b, size_t n);   // texture ID id described for a [RED] line
void vka_tex_feedback(vka_img *const *ims, const uint32_t *levels, int n);   // the open pass's attachments and their levels (n 0: none), for vka_tex_id_view
int vka_tex_id_in_pass(int id);   // texture ID id's image is an attachment of the open pass (vka_tex_feedback's set)
int vka_tex_id_target(int id);                      // the NVN target texture ID id stands for (its view's), -1 = not registered
int vka_tex_id_pending(int id);                     // texture ID id is registered and its image is not made yet (its first use records commands)
VkImageView vka_tex_id_view_rec(int id, VkCommandBuffer cb, uint8_t dim, int storage, VkImageLayout *layout, int *scaled);   // the same on the recording thread, through a memo (DM_VK_TEX_MEMO); *scaled: its image is (vka_img.scaled)
int vka_tex_id_pending_rec(int id, uint8_t dim);
int vka_tex_id_storage_pending(int id);            // texture ID id's image exists without storage use and would be remade with it at its next storage binding (a draw ends its pass first)
VkSampler vka_samp_id_rec(int id);
VkSampler vka_samp_id(int id);                      // sampler ID id; VK_NULL_HANDLE = not registered
VkSampler vka_samp_default(void);                   // the sampler an unbound sampler slot reads with (nearest, clamp to edge)

// ---------- Shaders (vka_shader.c) ----------
// A shader is one library entry with its module made (Lichii: each a one-entry library its recompiler made, nvn_recompile.c); it lives as long as the process
#include "vka_shlib.h"
#include "vka_bind.h"
typedef struct vka_shader {
    uint64_t key; const dmsh_vk_entry *e; const dmsh_vk_bind *b;
    const vka_shlib *lib;               // the library the entry is in (its header's translator answers; its other modules)
    VkShaderModule mods[4];             // by DMSH_VK_MOD_* variant, made when a pipeline first asks (the base when the shader is made)
    struct vka_vinmod *vin;             // vertex modules with integer-fed inputs retyped, by masks (vka_shader_vin_module)
    struct vka_vinmod *fout;            // pixel modules with float outputs retyped for integer targets, by masks (vka_shader_fout_module)
    vka_span c1; VkDeviceSize c1_len;   // the c1 bytes in static memory (none when c1_len is 0)
    int bad;                            // refused: no module, an unknown binding, an NV-only capability (logged once with its key)
    int gs_fx;                          // a geometry shader's memory writes (vka_spv_gs_claim): above 0, its draws bind a claim buffer at VKA_GS_CLAIM_BINDING
    VkShaderModule gs_claim;            // its module with those writes under the per-primitive claim, made when a pipeline first asks
    const uint8_t *nvn2_control;        // Lichii: a recompiled shader's NVN2 control section (nvn_recompile.c), NULL for the library's
    vka_span loop_guard;                // Lichii: a recompiled shader's loop guard word (static memory; none when it has no loops)
    uint16_t scale_tex_binding;         // Lichii: a recompiled shader's first texture binding and that texture's bit in the render scale's push constants
    uint8_t scale_tex_bit, scale_tex_bits;   // (nvn_direct.h nvn_direct_scale_texture_bit; scale_tex_bits 0: no textures)
} vka_shader;
// by NVN stage; bad: stages whose shader is refused (bit = NVN stage) or VKA_BAD_MISSING (their draws are skipped). Lichii: depth, recompiled shaders' depth mode
// (1 + gl_depth: the pre-rasterization stages are translated for one, vka_rc_shader; 0: any)
typedef struct { vka_shader *sh[VKA_NSTAGE]; unsigned bad; uint8_t depth; } vka_progsh;
int vka_progsh_current(const vka_progsh *ps);   // 1 = ps's shaders fit the depth mode now
#define VKA_BAD_MISSING (1u << VKA_NSTAGE)   // vka_progsh.bad: a stage's shader is not in the library (its NVN stage unknown)
int vka_prog_shaders(const prog_t *p, vka_progsh *out);      // 1 = resolved (stages the program lacks are NULL); 0 = no program
void vka_prog_put(uint64_t serial, const vka_progsh *ps);   // a program made by the host itself (the selftest), by serial
vka_shader *vka_shader_make(const vka_shlib *L, uint64_t key);
vka_shader *vka_shader_by_key(uint64_t key, int gl_depth);   // the recompiled shader of key in depth mode gl_depth (vka_rc_by_key); NULL = not translated or refused
VkShaderModule vka_shader_module(vka_shader *s, int variant);
VkShaderModule vka_shader_gs_module(vka_shader *s);   // a geometry shader's module with its memory writes made once per primitive (vka_spv_gs_claim)
#define VKA_SET_FS 2   // Lichii: a graphics pipeline's pixel shader resources: their own set (vka_pso.c layout_slow; its module remapped, vka_shader.c)
#define VKA_GS_CLAIM_BINDING (DMSH_VK_STAGE_GS * DMSH_VK_STAGE_BINDINGS + DMSH_VK_STAGE_BINDINGS - 1)   // set 0: the claim words of such a shader (the stage's last binding)
#define VKA_F_RENDER_AREA 16384   // Lichii: an entry flag of recompiled shaders past the library's DMSH_VK_F_*: reads the draw's render area (vka_draw.c, nvn_direct.h NVN_DIRECT_RENDER_AREA_OFFSET)
VkShaderModule vka_shader_fout_module(vka_shader *s, uint32_t umask, uint32_t smask);
int vka_shader_fout_retyped(vka_shader *s, uint32_t umask, uint32_t smask);   // 1 when that module is a rewritten one (not the base module it falls back to)   // the base pixel module with those float outputs as uint / int vectors of their bits (vka_spv_fout_retype), made once per masks
#define VKA_FS_RETYPE 4                  // vka_gkey.fs_variant beyond the library's DMSH_VK_MOD_*: the base pixel module with its float outputs at integer attachments retyped (vka_pso.c fout_masks)
VkShaderModule vka_shader_vin_module(vka_shader *s, uint32_t umask, uint32_t smask);   // the base module with those float inputs as uvec4 / ivec4 (vka_spv_vin_retype), made once per masks   // the module of that variant (DMSH_VK_MOD_*), made on first use; VK_NULL_HANDLE = the entry has none   // the shader of key in library L (the game's, or the selftest's own); NULL = not there or refused
// Lichii's recompiler (nvn_recompile.c): control sections kept at nvnProgramSetShaders; the draw whose state translation reads; a program stage's shader (*st its NVN
// stage, -1 without a kept control section); a shader by key; a recompiled compute shader's c[0]
void vka_rc_keep(const prog_t *p);
typedef struct vka_dstate vka_dstate;
void vka_rc_draw(const vka_dstate *d);
vka_shader *vka_rc_shader(const prog_t *p, unsigned i, uint64_t key, uint32_t prim_inputs, int gl_depth, int *st);
int vka_rc_stage(uint64_t key);         // NVN stage of a kept control section, -1 none
uint32_t vka_rc_prim(uint64_t key);     // a recompiled mesh shader's generic outputs per primitive
vka_shader *vka_rc_by_key(uint64_t key, int gl_depth);
unsigned vka_rc_stage_keys(int st, uint64_t **out);   // the kept translations' shader keys of NVN stage st (malloc'd)
void vka_rc_counts(unsigned *made, unsigned *kept);    // shaders made this run, of them kept from earlier runs
vka_shader *vka_shader_scratch(const vka_shlib *L, uint64_t key);   // a shader for a cache-only stage library: not in the shader table, given back with vka_shader_scratch_free
void vka_shader_scratch_free(vka_shader *s);
void vka_lib_ahead_shader(vka_shader *s, int part);   // a stage library (1 pre-rasterization, 2 fragment shader) in the most common state, for the caches only
void vka_lib_counts(uint64_t *compiled, uint64_t *ahead);   // graphics pipeline libraries compiled outside the ahead threads (over 20 ms), made ahead
void vka_rc_cs_c0(const vka_shader *sh, const vka_bstate *s, uint8_t *d);
int vka_static(VkDeviceSize n, VkDeviceSize align, vka_span *out);   // host-visible memory for the process's life (c1 bytes, the support and zero buffers)

// ---------- Pipelines (vka_pso.c) ----------
typedef struct {   // what a graphics pipeline is made from beyond its shaders and layout: zero-filled, then compared bytewise
    uint32_t cfmt[8], dfmt, sfmt;     // attachment formats (VK_FORMAT_UNDEFINED = none)
    uint8_t ncolor, samples, topo, poly_mode, depth_clamp, a2c, logic_en, logic_op, gl_depth, fs_variant, alpha_func, gs_fx;   // fs_variant: DMSH_VK_MOD_*; alpha_func: NVN_ALPHA_FUNC_* or 0; gs_fx: 1 = the geometry module with the claim (vka_shader_gs_module)
    uint32_t vin_u, vin_s;            // float vertex inputs fed UINT / SINT formats: retyped in the vertex module (vka_spvpatch.c), by location
    struct { uint8_t en, src, dst, op, srca, dsta, opa, mask; } att[8];
    // Lichii: the sample mask (of the key's samples), the patch's control points (0: no tessellation), conservative rasterization (cons: on, dilate:
    // quarter pixels), the last vertex provoking, VK_NV_viewport_swizzle's swizzles of viewport 0 (0: none), the exclusive scissor's dynamic state, a shading rate attachment
    uint16_t sample_mask; uint8_t patch, cons, dilate, provoking_last, excl, vrs; uint8_t vp_sw[4];
    uint32_t vin_static;              // no vertex input dynamic state: the attributes and bindings below are part of the pipeline
    struct { uint32_t fmt, offset; uint8_t binding, used, pad[2]; } va[32];   // by location
    struct { uint32_t stride, divisor; uint8_t used, pad[3]; } vb[33];         // by binding (stream; 32 = the zero buffer of inputs no attribute feeds)
} vka_gkey;
typedef struct {
    VkPipelineLayout layout; VkDescriptorSetLayout set0; int push; uint32_t ndesc;
    // Lichii, graphics: the pixel shader's own set (VKA_SET_FS, allocated), and the independent-sets layouts its stage libraries are made with
    VkDescriptorSetLayout set_fs; uint32_t ndesc_fs; VkPipelineLayout pre_layout, fs_layout;
} vka_layout;
int vka_layout_of(vka_shader *const sh[VKA_NSTAGE], int compute, vka_layout *out);   // the set-0 layout of these stages' records with the pools' set 1
VkPipeline vka_gfx_pipeline(vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k, int wait);   // wait: never left to the asynchronous threads (vka_pso.c)
VkPipeline vka_cs_pipeline(vka_shader *cs, const vka_layout *lay);
void vka_pso_save(int force);         // the VkPipelineCache and the prewarm list to the app's files (DM_TEMP): due DM_VK_PCACHE_SAVE_S after the last save, on the saver thread; force = now, on the caller's
void vka_pso_waits(uint64_t *save_ns, uint64_t *pw_wait_ns);   // cumulative: time saving the cache (saver thread), the frame path's waits for a prewarm thread making the same pipeline
double vka_gpu_span_ms(uint64_t f);   // frame f's submissions from first part's start to last part's end (vka_exec.c); -1 = not in yet

// ---------- Draws and dispatches (vka_draw.c) ----------
// The NVN state a queue keeps between submissions, as nvn_mtl.m's S (state_reset gives NVN's defaults)
typedef struct vka_dstate {   // (named: nvn_recompile.c declares it ahead)
    vka_bstate b;
    const prog_t *bst[VKA_NSTAGE], *cprog;   // the program bound per graphics stage (NVN stage bits; [5] unused, [6] / [7] NVN2's task and mesh bits), the compute program
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    va_st attr[16]; int nattr; vs_st stream[16]; int nstream; NVNbufferRange vbuf[16];
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    p_poly_offset po; float vp[16][4]; float dr[2]; int has_vp, depth_clamp; uint8_t vp_sw[16][4]; p_depth_bounds dbounds;
    float blend_color[4], alpha_ref;
    p_prim_restart restart; int rast_discard, tf_on;
    // Lichii: the command-buffer state of hle_nvn.c's "Command-buffer state" section
    int patch; float tess_inner[2], tess_outer[4]; uint32_t sample_mask;
    int cons, cons_mode, subpixel_bias[2], provoking; float dilate;
    int excl_on, excl_n; int32_t excl[16][4];
    p_vrs_image vrs; uint8_t vrs_pal[16][16];
    int inited;
} vka_dstate;
void vka_dstate_reset(vka_dstate *d);
int vka_state_op(vka_dstate *d, uint32_t op, const void *a);   // 1 = a state record, taken
// The render pass the draws of a submission go into: opened at a draw, closed by anything that is not a draw or state (vka_rp_end), and by a new SET_RT
void vka_draw_sub_begin(VkCommandBuffer cb, unsigned slot);    // the slot's per-draw descriptor sets are free again (its last fence is done)
void vka_rp_end(void);
int vka_rp_split_ok(const p_set_rt *rt, int has_rt);
void vka_draw_cb_switch(VkCommandBuffer cb);
void vka_dyn_invalidate(void);                       // the draws' dynamic state is set again in full at the next draw (another pipeline was bound)
void vka_dlss_eval(VkCommandBuffer cb, const p_dlss *c);   // Lichii: OP_DLSS, a spatial upscale in place of NVIDIA's network (vka_dlss.c)
int vka_pass_keep(void);                             // DM_VK_PASS_KEEP=0: a SET_RT ends the pass, as before
int vka_rp_open_for(const p_set_rt *rt, int has_rt);   // a pass is open over these targets
int vka_rp_game_barrier(uint32_t bits);                // a game barrier (OP_BARRIER): 1 = taken inside the open pass (DM_VK_BAR_KEEP), 0 = the caller ends the pass and records a full barrier
void vka_rp_bar_stats(char *b, size_t n, double frames);
void vka_rp_prep_clear(const p_set_rt *rt, int has_rt); // before a clear of these targets: a pass or pending clears of other targets end
// A clear of a bound target: everything the clear outside a pass needs (vka_clear_outside, vka_exec.c), and the value for the attachment inside one
typedef struct { int ci; vka_img *im; VkFormat fmt; VkImageAspectFlags asp; uint32_t lv, l0, nl; VkRect2D rc; int whole; VkClearValue out, in; } vka_pclear;
void vka_clear_outside(const vka_pclear *c);         // the clear on the image, outside any pass, then a barrier
// A clear of bound target c->ci (< 0: depth / stencil) into the render pass over those targets: an open one takes it as vkCmdClearAttachments; with none open it waits for
// the next draw's pass (loadOp CLEAR), or is done outside when anything else comes first (vka_rp_end). 0 = not taken: the caller ends the pass and clears outside
int vka_rp_clear(const p_set_rt *rt, int has_rt, const vka_dstate *d, const vka_pclear *c);
void vka_draw(vka_dstate *d, const p_set_rt *rt, int has_rt, const p_scissor *sc, int has_sc, const p_draw *dw, const p_draw_idx *di);
void vka_dispatch(vka_dstate *d, const p_dispatch *dp);
// Whether the game is a Switch 2 one (NVN2: its compute reads the group counts at c0 + DMSH_C0_DISPATCH_NVN2): dm_sdk_ounce() unless set (the replayer sets it from the capture)
int vka_nvn2(void);
void vka_set_nvn2(int on);
void vka_draw_force(int on);   // draws on whatever the memory (the selftest, whose memory may be a mapped allocation)
// DM_VK_NANSCAN (vka_nanscan.c): NaN / infinite texels in each pass's float attachments, logged with the pass's shaders
int vka_nan_on(void);
// The command encoder thread (vka_enc.c, DM_VK_ENC): the replay thread's vkCmd* calls into its command buffer, made on a thread of their own
void vka_enc_install(void);                 // vka_dev.c: the vk table's vkCmd* entries go through the encoder
void vka_enc_start(VkCommandBuffer cb);     // after vkBeginCommandBuffer on the replay thread: its calls into cb are deferred (when DM_VK_ENC is on)
void vka_enc_stop(void);                    // before vkEndCommandBuffer / a submit / a pool use: every deferred call made, none deferred from now on
void vka_enc_sync(void);                    // every deferred call made (a direct call that must land in order follows)
int vka_enc_on(void);
int vka_enc_call(void (*fn)(const void *), const void *arg, size_t n);   // fn on the encoder thread in order (1), or 0: encoder off

void vka_enc_switch(VkCommandBuffer cb);    // recording goes on in cb (queued reset / begin)
void vka_enc_stats(char *b, size_t n, double frames);   // the replay statistics' line (empty when unused)
int vka_nan_split(vka_img *const *ims, int n);
int vka_nan_probe(uint64_t fs);
void vka_nan_inputs(VkCommandBuffer cb, vka_img *const *ims, const int *binds, const int *tids, int n);
vka_img *vka_tex_id_img(int id, VkCommandBuffer cb);
void vka_tex_id_dump(int id);
void vka_tex_id_check(int id);                      // DM_VK_NANSCAN: has texture ID id's storage changed since its upload, and does the tracker know
// GPU writes into guest memory, as recorded (vka_tex.c gw_t): textures over them are uploaded with their bytes, or again once they are done
void vka_tex_gw_copy(uint64_t src, uint64_t dst, uint64_t n);
void vka_tex_gw_fill(uint64_t dst, uint64_t n, uint32_t value);
void vka_tex_gw_update(uint64_t dst, uint64_t n, const void *data);
void vka_tex_gw_opaque(uint64_t dst, uint64_t n);   // bytes the host does not know at recording (shader stores, texture → buffer copies)
int vka_tex_gml_take(VkDescriptorBufferInfo *bi);  // a raw global memory write log for one draw or dispatch, read when its submission is done (0 = none free)
void vka_tex_gw_commit(uint64_t fence);
void vka_tex_watch_dispatch(uint64_t key);         // DM_VK_NANSCAN: compute shaders by submission, for the storage watch             // the writes recorded since the last commit belong to this submission
void vka_nan_draw(uint64_t vs, uint64_t fs);
void vka_nan_pass(VkCommandBuffer cb, vka_img *const *ims, const uint32_t *lvs, const uint32_t *l0s, int n);
void vka_nan_commit(uint64_t fence);
void vka_draw_stats(uint64_t *draws, uint64_t *skipped, uint64_t *dispatches, uint64_t *pipelines);

// ---------- Present (vka_present.c) ----------
int vka_origin_ll(void);
int vka_queue_origin_ll(void);   // the submitting queue's raster origin (nvnQueueBuilder flags): the winding flips with it
int vka_depth_zero(void);        // nvnDeviceSetDepthMode NEAR_IS_ZERO (else GL's near = -w)
void vka_present_init(void);
// The overlay's counts (vka_fps.c): each image the present thread handed to the swapchain (shown), the game's own or a generated one; frame generation's state
void vka_fps_note(int shown, int generated);
void vka_fps_fg(int state, int mult);
// Frame generation (vka_framegen.c; LSFG in lsfg/, GPL-3.0-or-later): for the game frame about to be presented (its window texture, crop, period and timeline value), the images
// to present before it (gen, at most VKA_FG_MAX, the present takes its own references), how long each image of this frame should stay (hold, seconds; 0 = the game's period)
// and the output period for the window's frame rate (0 = frame generation off). Present thread only
enum { VKA_FG_MAX = 3 };
int vka_fg_frame(uint64_t serial, const int *crop, double period, uint64_t fence, vka_img **gen, double *hold, double *out_period);
uint64_t vka_refresh_ns(void);   // vka_present.c: the panel's refresh period in ns from display timing, 0 when not known (present thread)
