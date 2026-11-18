// From Banana Native (github.com/hugorosario/BananaNative, Android port of github.com/seedds/nxnative by seedds), MIT: see LICENSE in src/video_core/nvn_direct/bananative.
// The Vulkan device of the replay: instance, the one queue, the timeline that fences submissions, the completion thread that runs their CPU-side effects, and scratch memory.
// Feature and extension choices follow the probe of android/spike (results/honor-pnm-n49.md): placed memory maps, buffer device addresses, update-after-bind arrays, display timing
#if defined(__ANDROID__)
#include "vka.h"
#include "../../../../nvn_direct.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

vka_fns vk;
static int g_state;   // 0 not tried, 1 up, -1 failed
static pthread_mutex_t g_init_mu = PTHREAD_MUTEX_INITIALIZER;
vka_dev V = {.qmu = PTHREAD_MUTEX_INITIALIZER};
// Lichii: the device handed over (nvn_direct_adopt_device), and the lock every submission to its queues holds (Lichii's Scheduler::submit_mutex: Lichii submits to the
// last of these queues too); V.qmu without one
static nvn_direct_device g_adopt; static int g_adopted;
static uint64_t g_uwait_ns;   // waits for upload batches; cumulative
void vka_qlock(void) { if (g_adopt.queue_lock) g_adopt.queue_lock(g_adopt.queue_context); else pthread_mutex_lock(&V.qmu); }
void vka_qunlock(void) { if (g_adopt.queue_unlock) g_adopt.queue_unlock(g_adopt.queue_context); else pthread_mutex_unlock(&V.qmu); }
int vka_async_pipelines(void) { return g_adopt.async_pipelines; }
uint32_t vka_scale_up = 1, vka_scale_shift;
int nvn_direct_adopt_device(const nvn_direct_device *d) {
    // (the array of extension names is the caller's, for the call: the replay keeps its own copy; the names live as long as the device)
    const char **ext = malloc((d->extension_count ? d->extension_count : 1) * sizeof *ext); if (!ext) return 0;
    if (d->extension_count) memcpy(ext, d->extensions, d->extension_count * sizeof *ext);
    pthread_mutex_lock(&g_init_mu); if (!g_adopted && !g_state) { g_adopt = *d; g_adopt.extensions = ext; ext = NULL; g_adopted = 1; } pthread_mutex_unlock(&g_init_mu);
    free(ext);
    VLOG("asynchronous pipelines (draws skipped until their pipeline is made): %s (asynchronous shader building setting)", g_adopt.async_pipelines ? "on" : "off");
    if (g_adopt.scale_up && (g_adopt.scale_up != 1 || g_adopt.scale_shift)) {   // (Lichii's recompiler reads the scale)
        vka_scale_up = g_adopt.scale_up; vka_scale_shift = g_adopt.scale_shift;
        VLOG("render scale %u/%u", g_adopt.scale_up, 1u << g_adopt.scale_shift);
    }
    char h[256]; uint64_t u = 0, b = 0;
    if (g_adopt.memory_usage) { V.pd = d->physical; vka_heap_line(h, sizeof h); u = g_adopt.memory_usage(g_adopt.memory_context); VLOG("VRAM before the replay starts: host use %.0f MiB; driver %s", u / 1048576.0, h); }
    int ok = vka_init();
    if (ok && g_adopt.memory_usage) { vka_heap_line(h, sizeof h); u = g_adopt.memory_usage(g_adopt.memory_context); (void)b; VLOG("VRAM after the replay started: host use %.0f MiB; driver %s", u / 1048576.0, h); }
    return ok;
}


uint32_t vka_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    for (uint32_t i = 0; i < V.memp.memoryTypeCount; i++)
        if ((bits >> i & 1) && (V.memp.memoryTypes[i].propertyFlags & want) == want && !(V.memp.memoryTypes[i].propertyFlags & avoid)) return i;
    return UINT32_MAX;
}

int vka_dev_ext(const char *name) { for (uint32_t i = 0; i < g_adopt.extension_count; i++) if (!strcmp(g_adopt.extensions[i], name)) return 1; return 0; }   // (Lichii: the host enabled it)
static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) { for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1; return 0; }

static void *completion_main(void *arg);
static pthread_t g_complete_th; static int g_complete_up; static _Atomic int g_complete_stop, g_complete_left;   // Lichii: vka_complete_stop
static int init_once(void) {
    // Lichii: the replay runs on Lichii's Vulkan device (nvn_direct_adopt_device): its instance, physical device, device and the graphics family's queues; it makes none of
    // its own. Without a device handed over there is no replay device and submissions run their CPU-side effects only (the null backend)
    if (!g_adopted) return 0;
    vk.vkGetInstanceProcAddr = g_adopt.get_instance_proc_addr;
#define G(f) vk.f = (PFN_##f)vk.vkGetInstanceProcAddr(NULL, #f);
    VKA_GLOBAL_FNS(G)
#undef G
    V.inst = g_adopt.instance; VkResult r;
#define I(f) vk.f = (PFN_##f)vk.vkGetInstanceProcAddr(V.inst, #f);
    VKA_INSTANCE_FNS(I)
#undef I
    V.pd = g_adopt.physical;
    vk.vkGetPhysicalDeviceProperties(V.pd, &V.props); vk.vkGetPhysicalDeviceMemoryProperties(V.pd, &V.memp);
    if (V.props.apiVersion < VK_API_VERSION_1_3) { VRED("device %s is Vulkan %u.%u: the replay needs 1.3", V.props.deviceName, VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion)); return 0; }
    uint32_t nq = 0; vk.vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof *qp); vk.vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &nq, qp);
    V.qf = g_adopt.queue_family;   // Lichii: its graphics family
    { VkPhysicalDeviceFeatures pf; vk.vkGetPhysicalDeviceFeatures(V.pd, &pf); const VkPhysicalDeviceSparseProperties *sp = &V.props.sparseProperties;   // sparse residency: for sparse NVN
      VLOG("sparse: binding %d, residency buffer %d image2D %d image3D %d aliased %d, standard 2D block shape %d, non-resident strict %d, address space %llu MiB, sparse binding on queue family %u: %s; "
           "buffer / image granularity %llu",   // pools' textures (docs/reference/nvn-objects.md "Sparse texture arrays made at their full size")
           pf.sparseBinding, pf.sparseResidencyBuffer, pf.sparseResidencyImage2D, pf.sparseResidencyImage3D, pf.sparseResidencyAliased, sp->residencyStandard2DBlockShape, sp->residencyNonResidentStrict,
           (unsigned long long)(V.props.limits.sparseAddressSpaceSize >> 20), V.qf, V.qf < nq && (qp[V.qf].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) ? "yes" : "no",
           (unsigned long long)V.props.limits.bufferImageGranularity);
      // Sparse residency images (vka_sparse.c) need it all: binding on the replay's queue, 2D residency, the standard block shape (NVN's 64 KB tiles) and strict non-resident reads
      // (an unbound tile reads 0, as an unmapped NVN page). The features are enabled with every other supported one (vkGetPhysicalDeviceFeatures2 below)
      V.sparse_res = pf.sparseBinding && pf.sparseResidencyImage2D && sp->residencyStandard2DBlockShape && sp->residencyNonResidentStrict && V.qf < nq && (qp[V.qf].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT); }
    uint32_t fam_queues = g_adopt.queue_count;   // Lichii: the queues it made and handed over
    free(qp);
    if (V.qf == UINT32_MAX) { VRED("no graphics + compute queue family"); return 0; }

    // Lichii: the extensions Lichii's device enabled (it enables what the replay needs in direct mode: vulkan_device.cpp), not every one the device supports
    uint32_t nde = g_adopt.extension_count;
    VkExtensionProperties *de = calloc(nde ? nde : 1, sizeof *de); for (uint32_t i = 0; i < nde; i++) snprintf(de[i].extensionName, sizeof de[i].extensionName, "%s", g_adopt.extensions[i]);
    const char *dext[16]; uint32_t ndext = 0;
    { char q[700]; size_t qn = 0; q[0] = 0;   // the vendor's own extensions (tile properties, render pass transforms ...): what a tiler-specific path could use
      for (uint32_t i = 0; i < nde && qn < sizeof q; i++) if (strstr(de[i].extensionName, "_QCOM_")) qn += (size_t)snprintf(q + qn, sizeof q - qn, " %s", de[i].extensionName + 3);
      if (qn) VLOG("device extensions of the vendor:%s", q); }
    int swap = has_ext(de, nde, VK_KHR_SWAPCHAIN_EXTENSION_NAME), map2 = has_ext(de, nde, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME), placed = map2 && has_ext(de, nde, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME);
    V.display_timing = swap && has_ext(de, nde, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
    if (swap) dext[ndext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (map2) dext[ndext++] = VK_KHR_MAP_MEMORY_2_EXTENSION_NAME;
    if (placed) dext[ndext++] = VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME;
    if (V.display_timing) dext[ndext++] = VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME;
    // The draw path (vka_draw.c): each one optional, with a fallback when absent
    int x_push = has_ext(de, nde, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME), x_vin = has_ext(de, nde, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    const char *x_rob = has_ext(de, nde, VK_KHR_ROBUSTNESS_2_EXTENSION_NAME) ? VK_KHR_ROBUSTNESS_2_EXTENSION_NAME : has_ext(de, nde, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) ? VK_EXT_ROBUSTNESS_2_EXTENSION_NAME : NULL;
    const char *x_i8 = has_ext(de, nde, VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME) ? VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME : has_ext(de, nde, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME) ? VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME : NULL;
    const char *x_div = has_ext(de, nde, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) ? VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME : has_ext(de, nde, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) ? VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME : NULL;
    if (x_push) dext[ndext++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    if (x_vin) dext[ndext++] = VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME;
    if (x_rob) dext[ndext++] = x_rob;
    if (x_i8) dext[ndext++] = x_i8;
    if (x_div) dext[ndext++] = x_div;
    int x_gpl = has_ext(de, nde, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) && has_ext(de, nde, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
    if (x_gpl) { dext[ndext++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME; dext[ndext++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME; }
    int mesh_ext = has_ext(de, nde, VK_EXT_MESH_SHADER_EXTENSION_NAME); const char *mesh_env = getenv("DM_VK_MESH");   // restart only: the device's extensions
    int x_mesh = mesh_ext && !(mesh_env && !strcmp(mesh_env, "0"));
    if (x_mesh) dext[ndext++] = VK_EXT_MESH_SHADER_EXTENSION_NAME;
    int x_ph = has_ext(de, nde, "VK_QCOM_queue_perf_hint");
    int x_host = has_ext(de, nde, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);   // Lichii: memory pools are imported guest memory (vka_mem.c import_seg)
    int x_dmabuf = has_ext(de, nde, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) && has_ext(de, nde, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);   // Lichii: or as udmabufs of it (vka_mem_import_choose)
    if (x_ph) dext[ndext++] = "VK_QCOM_queue_perf_hint";
    free(de);

    // Every feature the device has, but robustness and capture-replay (they cost and nothing needs them); the shader side takes what it needs from the same device
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT fmp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT fvin = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesKHR frob = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR};
    VkPhysicalDeviceIndexTypeUint8Features fi8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES};
    VkPhysicalDeviceVertexAttributeDivisorFeatures fdiv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT fgpl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    VkPhysicalDeviceMeshShaderFeaturesEXT fmesh = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    void *chain = placed ? &fmp : NULL;
#define LINK(on, st) if (on) { (st).pNext = chain; chain = &(st); }
    struct { VkStructureType sType; void *pNext; VkBool32 queuePerfHint; } fph = {(VkStructureType)1000302001};   // VkPhysicalDeviceQueuePerfHintFeaturesQCOM (not in the NDK's headers yet)
    LINK(x_vin, fvin) LINK(x_rob, frob) LINK(x_i8, fi8) LINK(x_div, fdiv) LINK(x_ph, fph) LINK(x_gpl, fgpl) LINK(x_mesh, fmesh)
#undef LINK
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = chain};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
    vk.vkGetPhysicalDeviceFeatures2(V.pd, &f2);
    f2.features.robustBufferAccess = VK_FALSE; f13.robustImageAccess = VK_FALSE;
    f12.bufferDeviceAddressCaptureReplay = VK_FALSE; f12.bufferDeviceAddressMultiDevice = VK_FALSE;
    frob.robustBufferAccess2 = VK_FALSE; frob.robustImageAccess2 = VK_FALSE;   // only nullDescriptor: unbound slots read a null descriptor
    // Mesh shading: the task and mesh stages only (multiview, shading-rate and query support each need more of the device than the replay enables or uses)
    fmesh.multiviewMeshShader = VK_FALSE; fmesh.primitiveFragmentShadingRateMeshShader = VK_FALSE; fmesh.meshShaderQueries = VK_FALSE;
    if (x_mesh && !fmesh.meshShader) fmesh.taskShader = VK_FALSE;   // (task shading is of no use without mesh shading)
    V.push_desc = x_push; V.vin_dyn = x_vin && fvin.vertexInputDynamicState; V.null_desc = x_rob && frob.nullDescriptor; V.idx8 = x_i8 && fi8.indexTypeUint8;
    V.divisor = x_div && fdiv.vertexAttributeInstanceRateDivisor; V.logic_op = f2.features.logicOp; V.dual_src = f2.features.dualSrcBlend; V.geom = f2.features.geometryShader;
    V.tess = f2.features.tessellationShader; V.depth_clamp = f2.features.depthClamp; V.fill_lines = f2.features.fillModeNonSolid; V.depth_bias_clamp = f2.features.depthBiasClamp; V.depth_bounds = f2.features.depthBounds;
    V.multi_vp = f2.features.multiViewport; V.a2c = 1; V.mesh = x_mesh && fmesh.meshShader; V.task = V.mesh && fmesh.taskShader;
    if (!f12.timelineSemaphore || !f13.dynamicRendering) { VRED("the device lacks timeline semaphores (%d) or dynamic rendering (%d)", f12.timelineSemaphore, f13.dynamicRendering); return 0; }
    V.bda = f12.bufferDeviceAddress; V.uab = f12.descriptorBindingSampledImageUpdateAfterBind && f12.descriptorBindingPartiallyBound && f12.runtimeDescriptorArray;
    V.placed = placed && fmp.memoryMapPlaced; V.placed_range = placed && fmp.memoryMapRangePlaced; V.unmap_reserve = placed && fmp.memoryUnmapReserve;
    float prio = 1.f;
    // One Vulkan queue per NVN queue. Lichii: every queue of the family Lichii made, up to NVN_DIRECT_MAX_QUEUES (Ocarina of Time keeps six NVN queues)
    V.nq = (int)(fam_queues < NVN_DIRECT_MAX_QUEUES ? fam_queues : NVN_DIRECT_MAX_QUEUES); if (V.nq < 1) V.nq = 1;
    float prios[4] = {1.f, 1.f, 1.f, 1.f}; (void)prio;
    (void)prios; (void)dext; (void)ndext;
    V.dev = g_adopt.device;   // Lichii's device (see above)
#define D(f) vk.f = (PFN_##f)vk.vkGetDeviceProcAddr(V.dev, #f);
    VKA_DEVICE_FNS(D)
#undef D
    if (!vk.vkMapMemory2KHR) vk.vkMapMemory2KHR = (PFN_vkMapMemory2KHR)vk.vkGetDeviceProcAddr(V.dev, "vkMapMemory2");   // core in 1.4
    if (!vk.vkUnmapMemory2KHR) vk.vkUnmapMemory2KHR = (PFN_vkUnmapMemory2KHR)vk.vkGetDeviceProcAddr(V.dev, "vkUnmapMemory2");
    if (!vk.vkMapMemory2KHR || !vk.vkUnmapMemory2KHR) V.placed = V.placed_range = 0;
    { void vka_census_install(void); vka_census_install(); }   // Lichii: the objects made and destroyed are counted (nvn_shutdown.c)
    {   // Lichii: the state of hle_nvn.c's "Command-buffer state" section that needs an extension, where Lichii's device enabled it (vulkan_device.cpp)
        VkPhysicalDeviceProvokingVertexFeaturesEXT fpv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
        VkPhysicalDeviceExclusiveScissorFeaturesNV fex = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXCLUSIVE_SCISSOR_FEATURES_NV, .pNext = &fpv};
        VkPhysicalDeviceFragmentShadingRateFeaturesKHR ffsr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR, .pNext = &fex};
        VkPhysicalDeviceFeatures2 qf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &ffsr};
        vk.vkGetPhysicalDeviceFeatures2(V.pd, &qf);
        VkPhysicalDeviceConservativeRasterizationPropertiesEXT pcr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONSERVATIVE_RASTERIZATION_PROPERTIES_EXT};
        VkPhysicalDeviceFragmentShadingRatePropertiesKHR pfsr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR, .pNext = &pcr};
        VkPhysicalDeviceProperties2 qp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pfsr};
        vk.vkGetPhysicalDeviceProperties2(V.pd, &qp);
        V.cons = vka_dev_ext(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME); V.cons_max = pcr.maxExtraPrimitiveOverestimationSize;
        V.provoking_last = vka_dev_ext(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME) && fpv.provokingVertexLast;
        V.vp_swizzle = vka_dev_ext(VK_NV_VIEWPORT_SWIZZLE_EXTENSION_NAME);
        if (vka_dev_ext(VK_NV_SCISSOR_EXCLUSIVE_EXTENSION_NAME) && fex.exclusiveScissor) V.set_excl_scissor = (PFN_vkCmdSetExclusiveScissorNV)vk.vkGetDeviceProcAddr(V.dev, "vkCmdSetExclusiveScissorNV");
        V.excl_scissor = V.set_excl_scissor != NULL;
        // VRS: an attachment of NVN's 16x16-pixel tiles, written as R8_UINT storage by the pass that makes it (vka_draw.c vrs_fill)
        VkFormatProperties r8; vk.vkGetPhysicalDeviceFormatProperties(V.pd, VK_FORMAT_R8_UINT, &r8);
        const VkFormatFeatureFlags r8_need = VK_FORMAT_FEATURE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        const VkExtent2D t0 = pfsr.minFragmentShadingRateAttachmentTexelSize, t1 = pfsr.maxFragmentShadingRateAttachmentTexelSize;
        V.vrs = vka_dev_ext(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) && ffsr.attachmentFragmentShadingRate && (r8.optimalTilingFeatures & r8_need) == r8_need
                && t0.width <= 16 && t1.width >= 16 && t0.height <= 16 && t1.height >= 16;
        if (g_adopt.debug_labels) {
            V.label_begin = (PFN_vkCmdBeginDebugUtilsLabelEXT)vk.vkGetDeviceProcAddr(V.dev, "vkCmdBeginDebugUtilsLabelEXT");
            V.label_end = (PFN_vkCmdEndDebugUtilsLabelEXT)vk.vkGetDeviceProcAddr(V.dev, "vkCmdEndDebugUtilsLabelEXT");
            V.label_insert = (PFN_vkCmdInsertDebugUtilsLabelEXT)vk.vkGetDeviceProcAddr(V.dev, "vkCmdInsertDebugUtilsLabelEXT");
            if (!V.label_begin || !V.label_end || !V.label_insert) V.label_begin = NULL;
        }
        VLOG("command-buffer state: conservative rasterization %s (dilation up to %.2f), last provoking vertex %s, viewport swizzles %s, exclusive scissors %s, shading rate attachment %s "
             "(texels %ux%u-%ux%u), debug labels %s", V.cons ? "yes" : "no", V.cons_max, V.provoking_last ? "yes" : "no", V.vp_swizzle ? "yes" : "no", V.excl_scissor ? "yes" : "no",
             V.vrs ? "yes" : "no", t0.width, t0.height, t1.width, t1.height, V.label_begin ? "yes" : "no");
    }
    // Lichii: the queues it handed over; the last one is Lichii's own graphics queue, the host's here (V.q). With fewer than three everything goes to that one
    for (int i = 0; i < V.nq; i++) V.qs[i] = g_adopt.queues[fam_queues - (uint32_t)V.nq + (uint32_t)i];
    V.q = g_adopt.queues[fam_queues - 1];
    // With several, the last queue is the host's (upload batches, sparse binds, the present: V.q) and the NVN queues share the others: an upload a part waits for must not
    // sit behind another NVN queue's part that waits for an event (MKW's first window frame deadlocked so with the uploads on the first NVN queue)
    V.multi = V.nq > 2;
    if (V.multi) V.q = V.qs[V.nq - 1]; else { V.nq = 1; V.qs[0] = V.q; }
    if (x_ph && fph.queuePerfHint) V.perf_hint = (void *)vk.vkGetDeviceProcAddr(V.dev, "vkQueueSetPerfHintQCOM");
    VLOG("GPU clock floor (VK_QCOM_queue_perf_hint): %s", V.perf_hint ? "available (DM_VK_GPU_FLOOR)" : x_ph ? "extension without the feature" : "not on this device");

    VkPhysicalDeviceMapMemoryPlacedPropertiesEXT pmp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT};
    VkPhysicalDeviceVulkan13Properties p13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, .pNext = placed ? &pmp : NULL};
    VkPhysicalDeviceVulkan11Properties p11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, .pNext = &p13};
    VkPhysicalDevicePushDescriptorPropertiesKHR ppd = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR, .pNext = &p11};
    VkPhysicalDeviceMaintenance3Properties pm3 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES, .pNext = V.push_desc ? (void *)&ppd : (void *)&p11};
    VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT pgpl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT, .pNext = &pm3};
    VkPhysicalDeviceDriverProperties pdr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, .pNext = x_gpl ? (void *)&pgpl : (void *)&pm3};
    VkPhysicalDeviceMeshShaderPropertiesEXT pmesh = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT, .pNext = &pdr};
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT pemh = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT, .pNext = x_mesh ? (void *)&pmesh : (void *)&pdr};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = x_host ? (void *)&pemh : x_mesh ? (void *)&pmesh : (void *)&pdr};
    vk.vkGetPhysicalDeviceProperties2(V.pd, &p2);
    VLOG("driver: %s (%s), driver ID %d, conformance %u.%u.%u.%u, driver version %#x; %s", pdr.driverName, pdr.driverInfo, pdr.driverID, pdr.conformanceVersion.major,
         pdr.conformanceVersion.minor, pdr.conformanceVersion.subminor, pdr.conformanceVersion.patch, V.props.driverVersion, vka_driver_tag() ? "custom (DM_VK_DRIVER)" : "the system's");
    V.driver_id = (int)pdr.driverID;
    { const char *e = getenv("DM_VK_GPL"); int want = !(e && !strcmp(e, "0"));   // restart only: the libraries are made with the device's pipelines
      V.gpl = want && x_gpl && fgpl.graphicsPipelineLibrary && pgpl.graphicsPipelineLibraryFastLinking;
      VLOG("graphics pipeline libraries: %s", V.gpl ? "on (a pipeline the cache lacks is linked from stage libraries, the optimized one is made in the background; DM_VK_GPL=0 off)"
           : !x_gpl ? "not on this device" : !fgpl.graphicsPipelineLibrary ? "extension without the feature" : !pgpl.graphicsPipelineLibraryFastLinking ? "no fast linking" : "off (DM_VK_GPL=0)"); }
    V.placed_align = pmp.minPlacedMemoryMapAlignment ? pmp.minPlacedMemoryMapAlignment : 4096; V.max_alloc = pm3.maxMemoryAllocationSize;
    // Lichii: without placed maps (a desktop driver) the pools are guest memory imported as host allocations, at the import alignment
    V.host_import = !V.placed && x_host && vk.vkGetMemoryHostPointerPropertiesEXT && pemh.minImportedHostPointerAlignment;
    if (V.host_import) V.placed_align = pemh.minImportedHostPointerAlignment > 4096 ? pemh.minImportedHostPointerAlignment : 4096;
    if (V.host_import) vka_mem_import_choose(x_dmabuf);
    V.max_push = V.push_desc ? ppd.maxPushDescriptors : 0;
    if (V.mesh && !vk.vkCmdDrawMeshTasksEXT) V.mesh = V.task = 0;
    V.draw_indirect_count = f12.drawIndirectCount && vk.vkCmdDrawIndirectCount && vk.vkCmdDrawIndexedIndirectCount;
    V.multi_draw_indirect = f2.features.multiDrawIndirect;
    if (V.mesh) { V.mesh_inv = pmesh.maxMeshWorkGroupInvocations; V.task_inv = pmesh.maxTaskWorkGroupInvocations; V.mesh_out_v = pmesh.maxMeshOutputVertices;
                  V.mesh_out_p = pmesh.maxMeshOutputPrimitives; V.task_payload = pmesh.maxTaskPayloadSize;
                  memcpy(V.task_groups, pmesh.maxTaskWorkGroupCount, sizeof V.task_groups); memcpy(V.mesh_groups, pmesh.maxMeshWorkGroupCount, sizeof V.mesh_groups); }
    VLOG("mesh shading (NVN2 task and mesh programs): %s", V.task ? "on" : V.mesh ? "mesh shaders only (no taskShader)" : !mesh_ext ? "not on this device" : !x_mesh ? "off (DM_VK_MESH=0)" : "extension without the mesh stage");
    if (V.mesh) VLOG("mesh shading limits: workgroups mesh %u / task %u invocations, mesh output %u vertices / %u primitives / %u components / %u bytes, task payload %u bytes, mesh shared %u bytes",
                     V.mesh_inv, V.task_inv, V.mesh_out_v, V.mesh_out_p, pmesh.maxMeshOutputComponents, pmesh.maxMeshOutputMemorySize, V.task_payload, pmesh.maxMeshSharedMemorySize);
    VLOG("subgroups: size %u, stages %#x (geometry %s, fragment %s, vertex %s), operations %#x, quad operations in all stages %s, required size stages %#x", p11.subgroupSize, p11.subgroupSupportedStages,
         p11.subgroupSupportedStages & VK_SHADER_STAGE_GEOMETRY_BIT ? "yes" : "NO", p11.subgroupSupportedStages & VK_SHADER_STAGE_FRAGMENT_BIT ? "yes" : "NO", p11.subgroupSupportedStages & VK_SHADER_STAGE_VERTEX_BIT ? "yes" : "NO",
         p11.subgroupSupportedOperations, p11.subgroupQuadOperationsInAllStages ? "yes" : "no", p13.requiredSubgroupSizeStages);
    V.subgroup_stages = p11.subgroupSupportedStages;
    V.subgroup_req_stages = f13.subgroupSizeControl ? p13.requiredSubgroupSizeStages : 0;
    V.subgroup_size = p11.subgroupSize; V.subgroup_min = p13.minSubgroupSize ? p13.minSubgroupSize : p11.subgroupSize; V.subgroup_max = p13.maxSubgroupSize ? p13.maxSubgroupSize : p11.subgroupSize;
    if (!vk.vkCmdPushDescriptorSetKHR) vk.vkCmdPushDescriptorSetKHR = (PFN_vkCmdPushDescriptorSetKHR)vk.vkGetDeviceProcAddr(V.dev, "vkCmdPushDescriptorSet");   // core in 1.4
    if (!vk.vkCmdPushDescriptorSetKHR) V.push_desc = 0, V.max_push = 0;
    if (!vk.vkCmdSetVertexInputEXT) V.vin_dyn = 0;
    vka_enc_install();
    const VkPhysicalDeviceLimits *lim = &V.props.limits;
    V.ub_align = lim->minUniformBufferOffsetAlignment ? lim->minUniformBufferOffsetAlignment : 256; V.sb_align = lim->minStorageBufferOffsetAlignment ? lim->minStorageBufferOffsetAlignment : 256;
    V.ub_range = lim->maxUniformBufferRange; V.sb_range = lim->maxStorageBufferRange; V.max_vin_bindings = lim->maxVertexInputBindings;

    VkMemoryPropertyFlags HV = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    V.guest_type = vka_mem_type(~0u, HV | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
    if (V.guest_type == UINT32_MAX) V.guest_type = vka_mem_type(~0u, HV | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
    if (V.guest_type == UINT32_MAX) V.guest_type = vka_mem_type(~0u, HV, 0);
    V.upload_type = V.guest_type;
    V.local_type = vka_mem_type(~0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);

    VkSemaphoreTypeCreateInfo st = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    if ((r = vk.vkCreateSemaphore(V.dev, &sci, NULL, &V.tl))) { VRED("timeline semaphore %d", r); return 0; }
    for (int i = 0; V.multi && i < V.nq; i++) if ((r = vk.vkCreateSemaphore(V.dev, &sci, NULL, &V.qtl[i]))) { VRED("queue timeline %d: %d: one queue", i, r); V.multi = 0; V.nq = 1; V.q = V.qs[0]; }
    VLOG("queues: %d of the family's (%s)", V.nq, V.multi ? "NVN queues on all but the last, the host's work on the last; waits between them on the GPU" : "one FIFO for every NVN queue");
    if (pthread_create(&g_complete_th, NULL, completion_main, NULL)) { VRED("cannot start the completion thread"); return 0; }
    g_complete_up = 1;
    VLOG("device %s, Vulkan %u.%u.%u, driver %#x; placed maps %s (range %s, alignment %llu), host memory import %s, buffer device address %s, update-after-bind %s, display timing %s; guest memory type %u (%#x), max allocation %llu MiB, at most %u allocations",
         V.props.deviceName, VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion), VK_API_VERSION_PATCH(V.props.apiVersion), V.props.driverVersion,
         V.placed ? "yes" : "no", V.placed_range ? "yes" : "no", (unsigned long long)V.placed_align, V.host_import ? "yes" : "no", V.bda ? "yes" : "no", V.uab ? "yes" : "no", V.display_timing ? "yes" : "no",
         V.guest_type, V.guest_type < 32 ? V.memp.memoryTypes[V.guest_type].propertyFlags : 0, (unsigned long long)(V.max_alloc >> 20), V.props.limits.maxMemoryAllocationCount);   // (images take one allocation each: Mario Kart World ran out of them)
    VLOG("draw path: push descriptors %s (%u), vertex input dynamic state %s, null descriptors %s, 8-bit indices %s, instance divisors %s, logic op %s, dual source %s, geometry %s, tessellation %s; "
         "uniform alignment %llu (range %llu), storage alignment %llu, subgroups %u (%u-%u)",
         V.push_desc ? "yes" : "no", V.max_push, V.vin_dyn ? "yes" : "no", V.null_desc ? "yes" : "no", V.idx8 ? "yes" : "no", V.divisor ? "yes" : "no", V.logic_op ? "yes" : "no",
         V.dual_src ? "yes" : "no", V.geom ? "yes" : "no", V.tess ? "yes" : "no", (unsigned long long)V.ub_align, (unsigned long long)V.ub_range, (unsigned long long)V.sb_align, V.subgroup_size, V.subgroup_min, V.subgroup_max);
    if (!V.placed && !V.host_import) VRED("no VK_EXT_map_memory_placed or VK_EXT_external_memory_host: the GPU sees no memory pool");
    if (!swap) VRED("no VK_KHR_swapchain: nothing is presented");
    vka_pso_start();   // Lichii: the pipeline cache, the prewarm and the compute pipelines made ahead from the start, not from the first pipeline
    return 1;
}
int vka_init(void) {
    if (__atomic_load_n(&g_state, __ATOMIC_ACQUIRE)) return g_state > 0;
    pthread_mutex_lock(&g_init_mu);
    if (!g_state) __atomic_store_n(&g_state, init_once() ? 1 : -1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_init_mu);
    return g_state > 0;
}
int vka_ok(void) { return __atomic_load_n(&g_state, __ATOMIC_ACQUIRE) > 0; }

// Fence n's queue and value on its timeline (multi): the completion thread waits for that, then signals V.tl = n. q < 0: no GPU work signals it (the host takes it as reached)
enum { FQ_RING = 4096 };
static struct { uint64_t fence, qv; int q; } g_fq[FQ_RING];
static uint64_t sub_ex(int q, VkCommandBuffer cb, uint64_t signal, uint32_t nwait, const VkSemaphore *wait, const uint64_t *wait_vals, const VkPipelineStageFlags *stages,
                       uint32_t nsig, const VkSemaphore *sig_sems, const uint64_t *sig_vals, VkSemaphore bin_signal, VkResult *res);
VkResult vka_submit(VkCommandBuffer cb, uint64_t signal, uint32_t nwait, const VkSemaphore *wait, const uint64_t *wait_vals, const VkPipelineStageFlags *stages, VkSemaphore bin_signal) {
    VkResult r; sub_ex(-1, cb, signal, nwait, wait, wait_vals, stages, 0, NULL, NULL, bin_signal, &r); return r;
}
VkResult vka_submit_q(int q, VkCommandBuffer cb, uint64_t signal, uint32_t nwait, const VkSemaphore *wait, const uint64_t *wait_vals, const VkPipelineStageFlags *stages,
                      uint32_t nsig, const VkSemaphore *sig_sems, const uint64_t *sig_vals) {
    VkResult r; sub_ex(q, cb, signal, nwait, wait, wait_vals, stages, nsig, sig_sems, sig_vals, VK_NULL_HANDLE, &r); return r;
}
static uint64_t sub_ex(int q, VkCommandBuffer cb, uint64_t signal, uint32_t nwait, const VkSemaphore *wait, const uint64_t *wait_vals, const VkPipelineStageFlags *stages,
                       uint32_t nsig, const VkSemaphore *sig_sems, const uint64_t *sig_vals, VkSemaphore bin_signal, VkResult *res) {
    int host = q < 0 || !V.multi; if (q < 0 || q >= V.nq - 1 || !V.multi) q = 0;   // (host: the host queue V.q; with signal, its fences are recorded under queue 0's slot... none signal: the replay's go through queues 0..nq-2)
    VkSemaphore sig[24]; uint64_t sv[24]; uint32_t ns = 0;
    vka_qlock();
    uint64_t qv = 0;
    if (signal && !V.multi) { sig[ns] = V.tl; sv[ns++] = signal; }
    else if (signal) { qv = ++V.qseq[q]; sig[ns] = V.qtl[q]; sv[ns++] = qv; }
    if (bin_signal) { sig[ns] = bin_signal; sv[ns++] = 0; }
    for (uint32_t i = 0; i < nsig && ns < 24; i++) { sig[ns] = sig_sems[i]; sv[ns++] = sig_vals[i]; }
    // The waits, one for the upload batches on queue 0 (a submission on another queue), and one more for the sparse binds queued before it (vks_queue_locked)
    VkSemaphore ws[20]; uint64_t wv[20]; VkPipelineStageFlags wst[20];
    if (nwait > 18) { VRED_ONCE("vka_submit: %u waits, at most 18 taken: the rest are dropped", nwait); nwait = 18; }
    for (uint32_t i = 0; i < nwait; i++) { ws[i] = wait[i]; wv[i] = wait_vals[i]; wst[i] = stages[i]; }
    if (!host && vka_up_timeline() && vka_up_last()) { ws[nwait] = vka_up_timeline(); wv[nwait] = vka_up_last(); wst[nwait++] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; }
    vks_queue_locked(&nwait, ws, wv, wst);
    VkTimelineSemaphoreSubmitInfo ti = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = nwait, .pWaitSemaphoreValues = wv, .signalSemaphoreValueCount = ns, .pSignalSemaphoreValues = sv};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ti, .waitSemaphoreCount = nwait, .pWaitSemaphores = ws, .pWaitDstStageMask = wst,
                       .commandBufferCount = cb ? 1 : 0, .pCommandBuffers = &cb, .signalSemaphoreCount = ns, .pSignalSemaphores = sig};
    VkResult r = vka_hung() ? VK_ERROR_DEVICE_LOST : vk.vkQueueSubmit(host ? V.q : V.qs[q], 1, &si, VK_NULL_HANDLE);
    if (signal && V.multi) { if (r) V.qseq[q]--; g_fq[signal % FQ_RING].fence = signal; g_fq[signal % FQ_RING].q = r ? -1 : q; g_fq[signal % FQ_RING].qv = qv; }
    if (!r && signal > V.tl_queued) __atomic_store_n(&V.tl_queued, signal, __ATOMIC_RELEASE);
    vka_qunlock();
    *res = r;
    return qv;
}
// The queue idle, bounded by the hang time: every timeline the replay's queue work signals, up to the last value handed to a queue: submissions (the present's copies wait
// for no more), upload batches and sparse binds. Lichii: no vkQueueWaitIdle, which nothing bounds and which held the queue lock
int vka_queue_idle(void) {
    if (vka_hung()) return 0;
    vka_qlock();
    const uint64_t f = vka_fence_queued(), u = vka_up_last(); const VkSemaphore ut = vka_up_timeline(); uint64_t sp = 0; const VkSemaphore st = vks_timeline(&sp);
    vka_qunlock();
    VkSemaphore s[3]; uint64_t v[3]; uint32_t n = 0;
    if (f) { s[n] = V.tl; v[n++] = f; }
    if (u && ut) { s[n] = ut; v[n++] = u; }
    if (sp && st) { s[n] = st; v[n++] = sp; }
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = n, .pSemaphores = s, .pValues = v};
    VkResult r = n ? vk.vkWaitSemaphores(V.dev, &wi, vka_hang_ns()) : VK_SUCCESS;
    if (r) { VRED("queue idle: fence %llu, upload batch %llu and sparse bind value %llu not all reached in %.1f s (%d): not waited for", (unsigned long long)f, (unsigned long long)u,
                  (unsigned long long)sp, vka_hang_ns() / 1e9, r); return 0; }
    return 1;
}
uint64_t vka_fence_queued(void) { return __atomic_load_n(&V.tl_queued, __ATOMIC_ACQUIRE); }
int vka_fence_queue(uint64_t fence, int *q, VkSemaphore *sem, uint64_t *val) {   // (the slot holds the fence until FQ_RING later ones: an older one is not known)
    if (!V.multi || !fence) return 0;
    vka_qlock();
    const int ok = g_fq[fence % FQ_RING].fence == fence && g_fq[fence % FQ_RING].q >= 0;
    if (ok) { *q = g_fq[fence % FQ_RING].q; *sem = V.qtl[*q]; *val = g_fq[fence % FQ_RING].qv; }
    vka_qunlock();
    return ok;
}

// ---------- Fences ----------
// Fence n is reserved by the submission that signals timeline value n. The completion thread waits for the timeline, then runs that submission's CPU-side effects in order
// and only then publishes n as done: a SyncWait that returns has seen the events and counters of everything before it, as nvn_mtl.m's post_complete does
enum { POST_RING = 1024 };
typedef struct { uint64_t fence; void *ops; int n; void (*run)(void *, int); } post_rec;
static post_rec g_post[POST_RING];
typedef struct after_rec { struct after_rec *next; uint64_t fence; void (*fn)(void *); void *arg; } after_rec;
static after_rec *g_after;
static uint64_t g_fence, g_submitted, g_done;
static uint64_t g_fdone[FQ_RING];   // with several queues: fences done, by slot (a fence past g_done may be done before the fences before it)
static pthread_mutex_t g_fmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fcv = PTHREAD_COND_INITIALIZER, g_scv = PTHREAD_COND_INITIALIZER;

uint64_t vka_fence_last(void) { pthread_mutex_lock(&g_fmu); uint64_t f = g_fence; pthread_mutex_unlock(&g_fmu); return f; }
uint64_t vka_fence_next(void) { pthread_mutex_lock(&g_fmu); uint64_t f = ++g_fence; pthread_mutex_unlock(&g_fmu); return f; }
void vka_fence_post(uint64_t fence, void *ops, int n, void (*run)(void *, int)) {
    pthread_mutex_lock(&g_fmu);
    while (g_post[fence % POST_RING].fence && g_post[fence % POST_RING].fence != fence) pthread_cond_wait(&g_fcv, &g_fmu);   // that slot's earlier fence has not completed yet
    g_post[fence % POST_RING] = (post_rec){fence, ops, n, run};
    if (fence > g_submitted) g_submitted = fence;
    pthread_cond_broadcast(&g_scv); pthread_mutex_unlock(&g_fmu);
}
void vka_fence_cpu(uint64_t fence, void *ops, int n, void (*run)(void *, int)) {
    if (run) run(ops, n); else free(ops);
    pthread_mutex_lock(&g_fmu);
    if (fence > g_submitted) g_submitted = fence;
    if (V.multi && !vka_hung()) g_fdone[fence % FQ_RING] = fence;   // (g_done moves past it once the fences before it are done: completion_main)
    else if (fence > g_done) __atomic_store_n(&g_done, fence, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_scv);
    pthread_cond_broadcast(&g_fcv); pthread_mutex_unlock(&g_fmu);
}
uint64_t vka_done(void) { return __atomic_load_n(&g_done, __ATOMIC_ACQUIRE); }
// Waits until fence and every fence before it are done (in fence order: what frees memory or reuses a slot needs). vka_wait_fence: until fence itself is, which with several
// queues may be before an earlier fence of another queue (a sync the game fenced on one queue)
static int wait_fence(uint64_t fence, uint64_t timeout_ns, int own) {
    if (vka_done() >= fence || vka_stopping()) return 1;   // (Lichii: vka_stop)
    if (!timeout_ns) { if (!own) return 0; pthread_mutex_lock(&g_fmu); int d = g_fdone[fence % FQ_RING] == fence; pthread_mutex_unlock(&g_fmu); return d; }
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t ns = (uint64_t)dl.tv_nsec + (timeout_ns > 3600000000000ull ? 3600000000000ull : timeout_ns);
    dl.tv_sec += (time_t)(ns / 1000000000ull); dl.tv_nsec = (long)(ns % 1000000000ull);
    pthread_mutex_lock(&g_fmu);
    while (g_done < fence && !(own && g_fdone[fence % FQ_RING] == fence) && !vka_stopping()) if (pthread_cond_timedwait(&g_fcv, &g_fmu, &dl)) break;
    int ok = g_done >= fence || (own && g_fdone[fence % FQ_RING] == fence) || vka_stopping(); pthread_mutex_unlock(&g_fmu);
    return ok;
}
int vka_wait_done(uint64_t fence, uint64_t timeout_ns) { return wait_fence(fence, timeout_ns, 0); }
int vka_wait_fence(uint64_t fence, uint64_t timeout_ns) { return wait_fence(fence, timeout_ns, 1); }
void vka_after(uint64_t fence, void (*fn)(void *), void *arg) {
    pthread_mutex_lock(&g_fmu);
    if (g_done >= fence) { pthread_mutex_unlock(&g_fmu); fn(arg); return; }
    after_rec *a = malloc(sizeof *a); *a = (after_rec){g_after, fence, fn, arg}; g_after = a;
    pthread_mutex_unlock(&g_fmu);
}
// ---------- GPU hangs ----------
// A fence that makes no progress for DM_VK_HANG_S seconds (default 6) is a hang: the GPU, or the driver's queue, will not get there. Then nothing more is submitted (mtl_sub_begin
// records no command buffer, vka_submit refuses, the present drops its frames), every fence reads as done with its CPU-side effects run, pool memory the GPU may still read is
// kept instead of freed, and the host exits (dm_host_shutdown) unless DM_VK_HANG_EXIT=0. Before this, the completion thread logged the stall every 2 s and went on waiting, a pool
// given back was freed anyway after 2 s, and the present waited on the queue: on the Adreno 840 phone the system server restarted (a soft reboot) a few seconds later
static _Atomic int g_hung;
int vka_hung(void) { return atomic_load_explicit(&g_hung, memory_order_acquire); }
uint64_t vka_hang_ns(void) {
    double s = VKA_ENVF("DM_VK_HANG_S", 6.0); if (!(s > 0.5)) s = 6.0;
    return (uint64_t)(s * 1e9);
}
// Lichii: the emulator shuts down: as after a hang nothing more is submitted and every fence reads as done (the completion thread runs the effects of those
// submitted), without the hang's report or exit; waits for fences return at once, also those the replay worker has not submitted yet, so the game's threads leave their
// halted calls and the emulator's cores stop
static _Atomic int g_stopping;
int vka_stopping(void) { return atomic_load_explicit(&g_stopping, memory_order_acquire); }
void vka_stop(void) {
    if (atomic_exchange(&g_stopping, 1)) return;
    atomic_store(&g_hung, 1);
    pthread_mutex_lock(&g_fmu); pthread_cond_broadcast(&g_scv); pthread_cond_broadcast(&g_fcv); pthread_mutex_unlock(&g_fmu);
}
static void *hang_exit_main(void *arg) { (void)arg; pthread_setname_np(pthread_self(), "vk-hang"); dm_host_shutdown("GPU hang"); return NULL; }
void vka_exec_hang_report(uint64_t fence);   // vka_exec.c: what the stuck submission held
static void hang_declare(uint64_t want, uint64_t at, double s, const char *what) {
    if (atomic_exchange(&g_hung, 1)) return;
    VRED("GPU hang: %s; fence %llu not reached in %.1f s (the timeline at %llu, %llu handed to the queue, %llu reserved): nothing more is submitted, every fence reads as done",
         what, (unsigned long long)want, s, (unsigned long long)at, (unsigned long long)vka_fence_queued(), (unsigned long long)vka_fence_last());
    vka_exec_hang_report(at + 1);
    if (!VKA_ENV("DM_VK_HANG_EXIT", 1)) { VRED("GPU hang: DM_VK_HANG_EXIT=0, the game runs on without the GPU"); return; }
    VRED("GPU hang: the host exits (DM_VK_HANG_EXIT=0 runs on without the GPU)");
    pthread_t th; if (!pthread_create(&th, NULL, hang_exit_main, NULL)) pthread_detach(th);
}

// The CPU-side effects of fence f (its slot cleared after: vka_fence_post may wait for it), outside g_fmu
static void post_run_one(uint64_t f) {
    post_rec *p = &g_post[f % POST_RING];
    if (p->fence != f) return;
    if (p->run) p->run(p->ops, p->n); else free(p->ops);
    pthread_mutex_lock(&g_fmu); p->fence = 0; pthread_cond_broadcast(&g_fcv); pthread_mutex_unlock(&g_fmu);
}
// With the NVN queues as Vulkan queues each fence completes when its own queue's timeline reaches it, and g_done follows the fences done in a row. In fence
// order, a submission that waits for an event a later submission of another queue signals held back every fence after it: Mario Kart World waited for one of its queues'
// syncs (an acquire, a queue's FenceSync) before submitting the signal another queue's earlier submission waited for, and the GPU hang detection ended the run.
// Returns the fences that completed in this pass (*pending: the first one not done yet, 0 = none)
static int multi_pass(uint64_t done, uint64_t sub, uint64_t *pending) {
    static struct { int q; uint64_t qv; } fq[POST_RING];   // (at most POST_RING fences are pending: vka_fence_post)
    uint64_t qc[NVN_DIRECT_MAX_QUEUES] = {0}; int n = 0; *pending = 0;
    if (sub - done > POST_RING) sub = done + POST_RING;
    for (int i = 0; i < V.nq; i++) vk.vkGetSemaphoreCounterValue(V.dev, V.qtl[i], &qc[i]);
    vka_qlock();
    for (uint64_t f = done + 1; f <= sub; f++) { unsigned k = (unsigned)(f - done - 1); fq[k].q = g_fq[f % FQ_RING].fence == f ? g_fq[f % FQ_RING].q : -2; fq[k].qv = g_fq[f % FQ_RING].qv; }
    vka_qunlock();
    for (uint64_t f = done + 1; f <= sub; f++) {
        pthread_mutex_lock(&g_fmu); int d = g_fdone[f % FQ_RING] == f; pthread_mutex_unlock(&g_fmu);
        if (d) continue;
        int q = fq[f - done - 1].q; uint64_t qv = fq[f - done - 1].qv;
        if (q == -2 || (q >= 0 && qc[q] < qv)) { if (!*pending) *pending = f; continue; }   // (-2: handed to the queue but not recorded yet; -1: not on the GPU)
        post_run_one(f);
        pthread_mutex_lock(&g_fmu); g_fdone[f % FQ_RING] = f; pthread_cond_broadcast(&g_fcv); pthread_mutex_unlock(&g_fmu);
        n++;
    }
    return n;
}
static void multi_wait(void) {   // until any NVN queue's timeline moves (or 0.5 s)
    VkSemaphore sems[NVN_DIRECT_MAX_QUEUES]; uint64_t vals[NVN_DIRECT_MAX_QUEUES]; uint32_t n = 0;
    for (int i = 0; i < V.nq - 1; i++) { uint64_t c = 0; vk.vkGetSemaphoreCounterValue(V.dev, V.qtl[i], &c); if (V.qseq[i] > c) { sems[n] = V.qtl[i]; vals[n++] = c + 1; } }
    if (!n) { struct timespec ts = {0, 200000}; nanosleep(&ts, NULL); return; }   // (a fence handed to the queue but not recorded yet)
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .flags = VK_SEMAPHORE_WAIT_ANY_BIT, .semaphoreCount = n, .pSemaphores = sems, .pValues = vals};
    VkResult r = vk.vkWaitSemaphores(V.dev, &wi, 500000000ull);
    if (r && r != VK_TIMEOUT) { char w[64]; snprintf(w, sizeof w, "vkWaitSemaphores %d (the device is lost)", r); hang_declare(vka_done() + 1, vka_done(), 0, w); }
}
static void *completion_main(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk-complete"); void vka_perf_thread(int nice); vka_perf_thread(-8);   // vka_perf.c
    uint64_t stall_t0 = 0, stall_at = 0; int stall_logged = 0;
    for (;;) {
        pthread_mutex_lock(&g_fmu);
        while (g_done >= g_submitted && !atomic_load(&g_complete_stop)) pthread_cond_wait(&g_scv, &g_fmu);
        if (g_done >= g_submitted) { pthread_mutex_unlock(&g_fmu); break; }   // (Lichii: vka_complete_stop, once everything submitted is run)
        // Wait for the next fence, not the last one handed to the queue: each fence is published (and its waiters woken) when the GPU reaches it. Waiting for the last one
        // held every earlier fence back until all work queued at that moment had run: Donkey Kong Bananza waits for its previous frame's last fence after queueing two
        // submissions of the next frame, so its wait ended only when those were done too. DM_VK_DONE_LAST=1 (live) waits for the last one, as before, for A/B runs
        uint64_t sub = g_submitted, done = g_done, want = VKA_ENV("DM_VK_DONE_LAST", 0) ? sub : done + 1; pthread_mutex_unlock(&g_fmu);
        uint64_t v = 0;
        if (!vka_hung() && V.multi) {
            uint64_t pending = 0; int n = multi_pass(done, sub, &pending);
            pthread_mutex_lock(&g_fmu);
            v = g_done; while (v < sub && g_fdone[(v + 1) % FQ_RING] == v + 1) v++;
            pthread_mutex_unlock(&g_fmu);
            if (v > done) { VkSemaphoreSignalInfo ss = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = V.tl, .value = v}; uint64_t cur = 0; vk.vkGetSemaphoreCounterValue(V.dev, V.tl, &cur); if (cur < v) vk.vkSignalSemaphore(V.dev, &ss); }
            uint64_t t = now_ns();
            if (n || !pending) stall_t0 = 0;
            else {   // no fence completed in this pass
                if (!stall_t0 || stall_at != pending) { stall_t0 = t; stall_at = pending; stall_logged = 0; }
                double st = (t - stall_t0) / 1e9;
                if (st >= 2 && !stall_logged) { stall_logged = 1; VRED("GPU: fence %llu not reached after %.1f s (fences done in a row up to %llu, %llu handed to the queue)", (unsigned long long)pending, st, (unsigned long long)v, (unsigned long long)vka_fence_queued());
                    void vka_exec_stall(void); vka_exec_stall(); }   // vka_exec.c: event waits nothing walked so far meets
                if (t - stall_t0 >= vka_hang_ns()) hang_declare(pending, v, st, "no progress");
                else multi_wait();
            }
        } else if (!vka_hung()) {
            VkResult r;
            VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &V.tl, .pValues = &want};
            r = vk.vkWaitSemaphores(V.dev, &wi, 500000000ull);
            vk.vkGetSemaphoreCounterValue(V.dev, V.tl, &v);
            uint64_t t = now_ns();
            if (r == VK_TIMEOUT && v <= done) {   // no progress in this wait
                if (!stall_t0 || stall_at != done) { stall_t0 = t - 500000000ull; stall_at = done; stall_logged = 0; }
                double st = (t - stall_t0) / 1e9;
                if (st >= 2 && !stall_logged) { stall_logged = 1; VRED("GPU: fence %llu not reached after %.1f s (at %llu, %llu handed to the queue)", (unsigned long long)want, st, (unsigned long long)v, (unsigned long long)vka_fence_queued()); }
                if (t - stall_t0 < vka_hang_ns()) continue;
                hang_declare(want, v, st, "no progress");
            } else if (r && r != VK_TIMEOUT) {
                char w[64]; snprintf(w, sizeof w, "vkWaitSemaphores %d (the device is lost)", r);
                hang_declare(want, v, 0, w);
            } else stall_t0 = 0;
        }
        if (vka_hung()) v = sub;   // every fence submitted so far is taken as done: its CPU-side effects run, the guest's waits return
        if (v > sub) v = sub;
        for (uint64_t f = done + 1; f <= v; f++) post_run_one(f);   // (with several queues: those not run at their own completion, after a hang)
        pthread_mutex_lock(&g_fmu);
        if (v > g_done) __atomic_store_n(&g_done, v, __ATOMIC_RELEASE);
        after_rec *run = NULL, **pp = &g_after;
        while (*pp) { after_rec *a = *pp; if (a->fence <= g_done) { *pp = a->next; a->next = run; run = a; } else pp = &a->next; }
        pthread_cond_broadcast(&g_fcv); pthread_mutex_unlock(&g_fmu);
        while (run) { after_rec *a = run; run = a->next; a->fn(a->arg); free(a); }
    }
    atomic_store(&g_complete_left, 1);
    return NULL;
}
// Lichii (nvn_direct_park, after vka_stop and the worker idle): the device and guest memory go away next, so the completion thread runs the effects and deferred work of
// what was submitted (every fence reads as done) and leaves; joined here
int vka_complete_stop(uint64_t timeout_ns) {
    if (!g_complete_up) return 1;
    pthread_mutex_lock(&g_fmu); atomic_store(&g_complete_stop, 1); pthread_cond_broadcast(&g_scv); pthread_mutex_unlock(&g_fmu);
    uint64_t t0 = now_ns();
    while (!atomic_load(&g_complete_left) && now_ns() - t0 < timeout_ns) usleep(1000);
    if (!atomic_load(&g_complete_left)) return 0;
    pthread_join(g_complete_th, NULL); g_complete_up = 0;
    return 1;
}

// ---------- Scratch memory ----------
// Blocks of host-visible memory handed out linearly. Two owners: the submission being recorded (class 0: the blocks retire at its fence, vka_scratch_retire) and the
// open upload batch (class 1, while vka_scr_up is set on the calling thread: the blocks retire at the batch's value on the upload timeline, vka_up_flush). A block goes
// back to use once that value is reached
typedef struct sblk { struct sblk *next; VkBuffer buf; VkDeviceMemory mem; uint8_t *ptr; VkDeviceSize size, used; uint64_t fence; int up; } sblk;
static sblk *g_open[2], *g_busy;
static _Atomic uint64_t g_scratch_bytes; static _Atomic uint32_t g_scratch_n;
static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;
_Thread_local int vka_scr_up;
static VkSemaphore g_utl;   // the upload timeline: value n = upload batch n has completed
static VkDeviceSize g_ubytes;   // staging taken by the open upload batch (under g_smu)
VkSemaphore vka_up_timeline(void) { return g_utl; }
static uint64_t g_uval_pub;   // the last upload batch value handed to queue 0 (read under V.qmu by other queues' submissions)
uint64_t vka_up_last(void) { return g_uval_pub; }
static uint64_t up_done(void) {   // the upload timeline's value (every batch counts as done after a GPU hang)
    if (vka_hung()) return UINT64_MAX;
    uint64_t v = 0; if (g_utl) vk.vkGetSemaphoreCounterValue(V.dev, g_utl, &v); return v;
}
static int sblk_idle(const sblk *b, uint64_t done, uint64_t udone) { return b->fence <= (b->up ? udone : done); }
static sblk *sblk_new(VkDeviceSize n) {
    sblk *b = calloc(1, sizeof *b); b->size = n;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                                                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};   // (Lichii: indirect mesh draws' converted records, vka_draw.c mesh_records)
    if (vk.vkCreateBuffer(V.dev, &bi, NULL, &b->buf)) { free(b); return NULL; }
    VkMemoryRequirements mr; vk.vkGetBufferMemoryRequirements(V.dev, b->buf, &mr);
    uint32_t t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
    if (t == UINT32_MAX) t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
    void *p = NULL;
    if (t == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &b->mem) || vk.vkBindBufferMemory(V.dev, b->buf, b->mem, 0) || vk.vkMapMemory(V.dev, b->mem, 0, VK_WHOLE_SIZE, 0, &p)) {
        VRED("scratch block of %llu bytes not allocated", (unsigned long long)n); if (b->mem) vk.vkFreeMemory(V.dev, b->mem, NULL); vk.vkDestroyBuffer(V.dev, b->buf, NULL); free(b); return NULL; }
    b->ptr = p; atomic_fetch_add(&g_scratch_bytes, n); atomic_fetch_add(&g_scratch_n, 1); vka_mem_grew(n); return b;
}
uint64_t vka_scratch_bytes(uint32_t *blocks) { if (blocks) *blocks = atomic_load(&g_scratch_n); return atomic_load(&g_scratch_bytes); }
void vka_mem_grew(uint64_t bytes) {
    static _Atomic uint64_t acc, step = 1;
    uint64_t s = atomic_load_explicit(&step, memory_order_relaxed);
    if (s == 1) { const char *e = getenv("DM_VK_MEMLOG"); s = (e ? strtoull(e, NULL, 10) : 512) << 20; atomic_store_explicit(&step, s, memory_order_relaxed); }
    if (!s || atomic_fetch_add(&acc, bytes) + bytes < s) return;
    atomic_store(&acc, 0);
    int ni; uint32_t ns; uint64_t img = vka_tex_mem_nolock(&ni), scr = vka_scratch_bytes(&ns), pl = vka_mem_placed_nolock(), sp = vks_bytes_nolock();
    VLOG("memory: placed pools %.0f MiB, images %.0f MiB (%d), sparse tile blocks %.0f MiB, scratch %.0f MiB (%u blocks): %.0f MiB", pl / 1048576.0, img / 1048576.0, ni, sp / 1048576.0,
         scr / 1048576.0, ns, (pl + img + sp + scr) / 1048576.0);
}
// Idle blocks (their value reached) larger than the standard 16 MiB, or beyond 64 MiB of idle blocks in all, are freed (under g_smu): at every miss and at every retire.
// Kept, every block stayed for the process's life: Mario Kart World 1.8.0's boot uploads (levels of up to 144 MiB, each in a block of its size) left 1.4 GB of scratch
// before its first frame, and the phone's low-memory killer stopped it
static void sblk_trim(uint64_t done, uint64_t udone) {
    VkDeviceSize idle = 0;
    for (sblk **pp = &g_busy; *pp;) {
        sblk *b = *pp;
        int id = sblk_idle(b, done, udone);
        if (id && (b->size > (16u << 20) || idle + b->size > (64u << 20))) {
            *pp = b->next; vk.vkUnmapMemory(V.dev, b->mem); vk.vkDestroyBuffer(V.dev, b->buf, NULL); vk.vkFreeMemory(V.dev, b->mem, NULL);
            atomic_fetch_sub(&g_scratch_bytes, b->size); atomic_fetch_sub(&g_scratch_n, 1); free(b);
        } else { if (id) idle += b->size; pp = &b->next; }
    }
}
int vka_scratch(VkDeviceSize n, VkDeviceSize align, vka_span *out) {
    if (!align) align = 16;
    int c = vka_scr_up ? 1 : 0;
    pthread_mutex_lock(&g_smu);
    sblk *b = g_open[c];
    VkDeviceSize o = b ? (b->used + align - 1) / align * align : 0;
    if (!b || o + n > b->size) {
        // Best fit among the idle blocks (a 144 MiB block kept for a 1 KB request stays alive), then the idle ones beyond the budget freed
        uint64_t done = vka_done(), udone = up_done(); sblk **best = NULL;
        for (sblk **pp = &g_busy; *pp; pp = &(*pp)->next) if ((*pp)->size >= n && sblk_idle(*pp, done, udone) && (!best || (*pp)->size < (*best)->size)) best = pp;
        b = NULL; if (best) { b = *best; *best = b->next; }
        sblk_trim(done, udone);
        if (!b) b = sblk_new(n > (16u << 20) ? (n + 0xFFFF) & ~(VkDeviceSize)0xFFFF : 16u << 20);
        if (!b) { pthread_mutex_unlock(&g_smu); return 0; }
        b->used = 0; b->up = c; b->next = g_open[c]; g_open[c] = b; o = 0;
    }
    b->used = o + n; *out = (vka_span){b->buf, b->ptr + o, o};
    if (c) g_ubytes += n;
    pthread_mutex_unlock(&g_smu);
    return 1;
}
static void retire(int c, uint64_t value) {
    pthread_mutex_lock(&g_smu);
    while (g_open[c]) { sblk *b = g_open[c]; g_open[c] = b->next; b->fence = value; b->next = g_busy; g_busy = b; }
    if (c) { g_ubytes = 0; sblk_trim(vka_done(), up_done()); }   // (the submission's blocks are trimmed at a miss only: a game that takes more than 16 MiB a frame would make and free them every frame)
    pthread_mutex_unlock(&g_smu);
}
void vka_scratch_retire(uint64_t fence) { retire(0, fence); }

// ---------- Upload batches ----------
// The first content of an image the replay makes (its upload from guest memory, or its clear) is recorded into a command buffer of its own, an upload batch, instead of
// the submission being recorded. The batch goes to the queue when its staging passes DM_VK_UPLOAD_BATCH_MB (live, default 64) and always before the replay's next part
// or submission goes (vka_exec.c: chunk, vka_rp_sub_commit), so the queue runs it before every command that uses the image; it ends with a full barrier, whose second
// scope covers every later submission. Its staging retires when the GPU reaches the batch's value on the upload timeline, not at the fence of the submission it was made
// in: one submission that makes hundreds of images (Mario Kart World 1.8.0 at boot: 29,000 images, 1.5 GB of staging in one submission) no longer holds all of it.
// When the batches in flight hold more than DM_VK_UPLOAD_INFLIGHT_MB (live, default 256) of staging, the replay thread waits for the oldest. Only a new image is safe here:
// nothing recorded earlier in the submission reads it (a re-upload of a live image stays in the submission, in order with the draws before it). DM_VK_UPLOAD_BATCH=0 (live)
// records first contents into the submission, as before
// Lichii: 128 batch slots (upstream 16). With a batch per 256 ASTC decodes, Mario Kart World's race start made 52 batches in one
// submission (836 images, 196 MB): the replay thread waited 234 ms for slots to come free, the GPU's decodes then ran one after another behind it instead of beside
// the recording. The staging they hold stays capped by DM_VK_UPLOAD_INFLIGHT_MB (vka_up_throttle)
enum { NUP = 128 };
static struct { VkCommandBuffer cb; uint64_t val; } g_ub[NUP];
static VkCommandPool g_upool; static uint64_t g_uval; static int g_ui = -1; static unsigned g_unext;   // g_ui: the open batch's slot (-1: none); replay thread only
static uint64_t g_n_ubatch, g_n_uwait;
static int up_wait(uint64_t v) {   // 1 = the upload timeline reached v
    if (up_done() >= v) return 1;
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_utl, .pValues = &v};
    uint64_t t0 = now_ns(); VkResult r = vk.vkWaitSemaphores(V.dev, &wi, vka_hang_ns());
    g_n_uwait++; g_uwait_ns += now_ns() - t0;
    if (r && !vka_hung()) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 4) VRED("upload batch %llu not done in %.1f s (%d)", (unsigned long long)v, vka_hang_ns() / 1e9, r); }
    return !r || vka_hung();
}
int vka_up_on(void) { return VKA_ENV("DM_VK_UPLOAD_BATCH", 1) != 0; }
VkCommandBuffer vka_up_begin(void) {
    if (g_ui < 0) {
        static int failed; if (failed) return VK_NULL_HANDLE;
        if (!g_upool) {
            VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = V.qf};
            VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = NUP};
            VkSemaphoreTypeCreateInfo st = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
            VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
            VkCommandBuffer cbs[NUP]; VkCommandPool p = VK_NULL_HANDLE;
            if (vk.vkCreateCommandPool(V.dev, &pi, NULL, &p) || ((ai.commandPool = p), vk.vkAllocateCommandBuffers(V.dev, &ai, cbs)) || vk.vkCreateSemaphore(V.dev, &sci, NULL, &g_utl)) {
                VRED_ONCE("upload batches not set up: first contents are recorded into the submission"); g_utl = VK_NULL_HANDLE; failed = 1; return VK_NULL_HANDLE; }
            for (int i = 0; i < NUP; i++) g_ub[i].cb = cbs[i];
            g_upool = p;
        }
        if (vka_hung()) return VK_NULL_HANDLE;
        unsigned k = g_unext % NUP;   // (the backpressure below keeps them mostly done)
        if (g_ub[k].val && !up_wait(g_ub[k].val)) return VK_NULL_HANDLE;   // (its batch still runs: the caller records into the submission)
        g_unext++;
        vk.vkResetCommandBuffer(g_ub[k].cb, 0);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        if (vk.vkBeginCommandBuffer(g_ub[k].cb, &bi)) return VK_NULL_HANDLE;
        g_ui = (int)k;
    }
    vka_scr_up = 1;
    return g_ub[g_ui].cb;
}
void vka_up_end(void) { vka_scr_up = 0; }
static int is_batch(VkCommandBuffer cb) { for (int i = 0; i < NUP; i++) if (cb && g_ub[i].cb == cb) return 1; return 0; }
int vka_up_is(VkCommandBuffer cb) { return is_batch(cb); }
// ASTC on a device without it, decoded on the GPU by the host (Lichii: nvn_direct_device.astc_to_bc)
int vka_astc_gpu_on(void) { return g_adopt.astc_to_bc != NULL; }
int vka_astc_gpu(VkCommandBuffer cb, const nvn_direct_astc_level *l) { return g_adopt.astc_to_bc && g_adopt.astc_to_bc(g_adopt.astc_context, cb, l); }
static void astc_gpu_flush(VkCommandBuffer cb) { if (g_adopt.astc_flush) g_adopt.astc_flush(g_adopt.astc_context, cb); }   // (the decodes it batches, before the batch ends)
int vka_astc_opaque(const void *blocks, size_t n) { return g_adopt.astc_opaque && g_adopt.astc_opaque(blocks, n); }   // Lichii: BC1 for opaque ASTC (vka_tex.c astc_bc1)
// Lichii's shader recompiler (nvn_direct_device.shader_translate): a program stage translated, its result given back with vka_shader_translated_free
int vka_shader_translate(const nvn_direct_shader_source *s, nvn_direct_shader *o) {
    if (g_adopt.shader_translate) return g_adopt.shader_translate(g_adopt.shader_context, s, o);
    memset(o, 0, sizeof *o); snprintf(o->error, sizeof o->error, "no shader recompiler"); return 0;
}
void vka_shader_translated_free(nvn_direct_shader *o) { if (g_adopt.shader_free) g_adopt.shader_free(g_adopt.shader_context, o); }
void vka_pipeline_notify(int built) {   // (Lichii: nvn_direct_device.pipeline_building / pipeline_built)
    void (*fn)(void *) = built ? g_adopt.pipeline_built : g_adopt.pipeline_building;
    if (fn) fn(g_adopt.pipeline_context);
}
// Lichii: the driver's own use and budget per heap (VK_EXT_memory_budget), MiB, for the VRAM line
void vka_heap_line(char *o, size_t n) {
    static PFN_vkGetPhysicalDeviceMemoryProperties2 get; if (!get && g_adopt.get_instance_proc_addr) get = (PFN_vkGetPhysicalDeviceMemoryProperties2)g_adopt.get_instance_proc_addr(g_adopt.instance, "vkGetPhysicalDeviceMemoryProperties2");
    if (!n) return; o[0] = 0; if (!get) return;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT}; VkPhysicalDeviceMemoryProperties2 p = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &b};
    get(V.pd, &p); size_t k = 0;
    for (uint32_t i = 0; i < p.memoryProperties.memoryHeapCount && k < n; i++)
        k += (size_t)snprintf(o + k, n - k, "%sheap %u%s %.0f / %.0f", i ? ", " : "", i, (p.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " (device)" : "",
                              b.heapUsage[i] / 1048576.0, b.heapBudget[i] / 1048576.0);
}
int vka_mem_budget(uint64_t *usage, uint64_t *budget) {   // the host's device-local use and budget (Lichii: nvn_direct_device.memory_usage / memory_budget); 0 = no budget
    if (!g_adopt.memory_budget || !g_adopt.memory_usage || !(*budget = g_adopt.memory_budget(g_adopt.memory_context))) return 0;
    *usage = g_adopt.memory_usage(g_adopt.memory_context); return 1;
}
// No batch can be begun (a slot's batch still runs after the hang time): the upload goes on in the submission being recorded, with its staging (the batch before it was sent,
// so its image's first barrier and copies come before; an upload stopped half-way would leave a tracked image with part of its content for good)
static VkCommandBuffer up_fallback(void) { vka_scr_up = 0; return vka_rp_cb(); }
VkCommandBuffer vka_up_cur(VkCommandBuffer cb) {   // the open batch when cb was one (vka_up_point may have sent it), else cb
    if (!is_batch(cb)) return cb;
    if (g_ui >= 0) return g_ub[g_ui].cb;
    VkCommandBuffer n = vka_up_begin(); return n ? n : up_fallback();
}
// Lichii: an upload batch also goes to the queue after 256 ASTC decodes. With batches of
// thousands of small decodes (Mario Kart World's boot: a decode, an encode and a copy per mip level, most of them tiny) the NVIDIA driver kept ~2.9 GiB of video memory
// outside every memory heap (NVML counts it, VK_EXT_memory_budget does not), and it stayed: 32 or 256 decodes a batch keep it near 0 at the same boot time;
// a submission per level does too but boots half as fast. Lichii's own texture cache runs the same shaders in small submissions and kept ~0.3 GiB
static unsigned g_up_decodes;   // ASTC decodes recorded into the open upload batch (vka_up_decoded)
void vka_up_decoded(void) { g_up_decodes++; }
VkCommandBuffer vka_up_point(VkCommandBuffer cb) {
    if (g_ui < 0 || cb != g_ub[g_ui].cb) return cb;
    pthread_mutex_lock(&g_smu); VkDeviceSize n = g_ubytes; pthread_mutex_unlock(&g_smu);
    double mb = VKA_ENVF("DM_VK_UPLOAD_BATCH_MB", 64.0);
    if ((double)n < mb * 1048576.0 && g_up_decodes < 256) return cb;
    vka_up_flush();
    VkCommandBuffer nb = vka_up_begin(); return nb ? nb : up_fallback();
}
void vka_up_flush(void) {
    if (g_ui < 0) return;
    g_up_decodes = 0;
    unsigned k = (unsigned)g_ui; g_ui = -1;
    VkCommandBuffer cb = g_ub[k].cb;
    astc_gpu_flush(cb);
    vka_barrier(cb);   // everything the batch wrote, for every later submission on the queue
    VkResult r = vk.vkEndCommandBuffer(cb);
    uint64_t v = ++g_uval;
    VkSemaphore ws[1]; uint64_t wv[1]; VkPipelineStageFlags wst[1]; uint32_t nw = 0;   // the sparse binds queued before it (vks_queue_locked)
    VkTimelineSemaphoreSubmitInfo ti = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &v};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ti, .commandBufferCount = 1, .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &g_utl};
    vka_qlock();
    if (!r) {
        vks_queue_locked(&nw, ws, wv, wst);
        ti.waitSemaphoreValueCount = nw; ti.pWaitSemaphoreValues = wv; si.waitSemaphoreCount = nw; si.pWaitSemaphores = ws; si.pWaitDstStageMask = wst;
        r = vka_hung() ? VK_ERROR_DEVICE_LOST : vk.vkQueueSubmit(V.q, 1, &si, VK_NULL_HANDLE);
        g_uval_pub = v;
    }
    vka_qunlock();
    // Nothing on the GPU will signal it: the host does, so later batches and the slot are not held; only once the batch before it is done (a host signal must be below
    // every value still pending on the queue)
    if (r && !vka_hung() && (v == 1 || up_wait(v - 1))) { VkSemaphoreSignalInfo ss = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = g_utl, .value = v}; vk.vkSignalSemaphore(V.dev, &ss); }
    if (r) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("upload batch %llu: %d (its images keep what they held)", (unsigned long long)v, r); }
    g_ub[k].val = v; g_n_ubatch++;
    retire(1, v);
}
// Backpressure: the batches in flight hold at most DM_VK_UPLOAD_INFLIGHT_MB of staging (the oldest is waited for; the GPU runs them in queue order). Called where the
// replay thread holds no lock (vka_tex_get after the table lock, chunk, the commit): the present and other threads that take the image table do not wait for the GPU
void vka_up_throttle(void) {
    if (!g_utl) return;
    double cap = VKA_ENVF("DM_VK_UPLOAD_INFLIGHT_MB", 256.0) * 1048576.0;
    for (;;) {
        uint64_t udone = up_done(), oldest = UINT64_MAX; VkDeviceSize fl = 0;
        pthread_mutex_lock(&g_smu);
        for (sblk *b = g_busy; b; b = b->next) if (b->up && b->fence > udone) { fl += b->size; if (b->fence < oldest) oldest = b->fence; }
        pthread_mutex_unlock(&g_smu);
        if ((double)fl <= cap || oldest == UINT64_MAX || !up_wait(oldest)) break;
    }
}
uint64_t vka_up_batches(void) { return g_n_ubatch; }
void vka_up_stats(uint64_t *batches, uint64_t *waits, double *wait_ms) { if (batches) *batches = g_n_ubatch; if (waits) *waits = g_n_uwait; if (wait_ms) *wait_ms = g_uwait_ns / 1e6; }

// ---------- Lichii: the teardown ----------
// nvn_direct_destroy, once every thread of the replay has left (nvn_direct_park) and the device is idle: every Vulkan object the replay made, its imports and views of
// guest memory and its host memory go, so the next game starts on a device and a process without them. The work deferred to fences (vka_after) runs first: it gives
// back images and pool memory. Each file destroys its own (the images before their memory blocks, the pipelines before their layouts and modules)
void vka_tex_destroy(void); void vka_imem_destroy(void); void vks_destroy(void); void vka_pso_destroy(void); void vka_shader_destroy(void); void vka_rc_destroy(void);
void vka_exec_destroy(void); void vka_draw_destroy(void); void vka_enc_destroy(void); void vka_mem_destroy(void); void vka_capture_destroy(void); void vka_ctl_destroy(void);
void vka_dlss_destroy(void); void vka_nan_destroy(void); void vka_gprof_destroy(void); void nvn_present_destroy(void); void vka_census_line(void);
void vka_destroy(void) {
    atomic_store(&g_hung, 0);   // (the GPU is idle: what a hang or the stop kept from being freed goes now, and what is deferred from now on runs at once)
    pthread_mutex_lock(&g_fmu); after_rec *run = g_after; g_after = NULL; __atomic_store_n(&g_done, g_fence, __ATOMIC_RELEASE); pthread_mutex_unlock(&g_fmu);
    while (run) { after_rec *a = run; run = a->next; a->fn(a->arg); free(a); }
    nvn_present_destroy();
    vka_capture_destroy(); vka_ctl_destroy(); vka_dlss_destroy(); vka_nan_destroy(); vka_gprof_destroy();
    vka_exec_destroy(); vka_draw_destroy(); vka_enc_destroy();
    vka_pso_destroy(); vka_rc_destroy(); vka_shader_destroy();
    vka_tex_destroy(); vka_imem_destroy(); vks_destroy();
    vka_mem_destroy();
    for (sblk *l[3] = {g_open[0], g_open[1], g_busy}, **p = l; p < l + 3; p++)
        for (sblk *b = *p, *n; b; b = n) { n = b->next; vk.vkUnmapMemory(V.dev, b->mem); vk.vkDestroyBuffer(V.dev, b->buf, NULL); vk.vkFreeMemory(V.dev, b->mem, NULL); free(b); }
    if (g_upool) vk.vkDestroyCommandPool(V.dev, g_upool, NULL);
    if (g_utl) vk.vkDestroySemaphore(V.dev, g_utl, NULL);
    for (int i = 0; i < NVN_DIRECT_MAX_QUEUES; i++) if (V.qtl[i]) vk.vkDestroySemaphore(V.dev, V.qtl[i], NULL);
    vk.vkDestroySemaphore(V.dev, V.tl, NULL);
    vka_census_line();
    free((void *)g_adopt.extensions);
}
#endif

#if defined(__ANDROID__)
// DM_VK_GPU_FLOOR=s (live; default 100): the queue's GPU clock floor through VK_QCOM_queue_perf_hint, s % of the highest clock (100: the highest; 0: the driver's default
// governor). The GPU's governor follows its busy time, and the replay keeps the GPU idle while it records the start of a frame, so it clocks down: the same replayed level
// frame took 21-28 ms of GPU time with the clock up and 93-99 ms after two seconds of idle GPU (Adreno 840), and in the live level the GPU finished 23-33 ms after the
// recording at the default governor, 6-10 ms with the floor at 100 (with DM_VK_CHUNK_PASS). The game's frame-rate controller measures frame start to GPU end, so that tail
// decides between its 20 and 30 fps: in one live session (CPU capped at 2.23 GHz) 85 % of the frames at 30 fps with the floor at 100, 0-16 % at 70 or with the
// driver's governor. The cost is power while a frame's GPU work runs; a lower floor or 0 gives it back
// A hint is the queue's state until changed; the kernel takes the highest of all queues' floors
// DM_VK_GPU_FLOOR_AUTO=1 (live; default 0 = the fixed floor): the floor follows what the frame needs, between 0 and DM_VK_GPU_FLOOR. Each frame's span from the start of the replay's
// recording of its first submission to the GPU end of its last (vka_exec.c, vka_floor_span: recording plus the GPU tail) is held against the period the frame aims at: the game's present period, at most
// 33.3 ms (so a game held at 20 fps by the GPU end still aims at 30). Over the last 30 frames: the 90th percentile within 2 ms of the period raises the floor a step (within
// 0.5 ms: to DM_VK_GPU_FLOOR at once); more than 5 ms below it (DM_VK_GPU_FLOOR_LO; raise: DM_VK_GPU_FLOOR_HI, 2 ms) for 60 frames since the last change lowers it a step. Steps of 25 %
enum { FSPAN = 64 };
static _Atomic uint64_t g_fspan[FSPAN], g_fspan_f[FSPAN];   // per frame (mod FSPAN): its span in ns, and which frame it is
static uint64_t g_floor_frames[5];                            // frames at each step (statistics)
static double g_p90_sum; static uint64_t g_p90_n;
extern _Atomic int64_t vka_period_ns;
void vka_floor_span(uint64_t frame, uint64_t ns) {           // the completion thread: a submission of frame ended ns after the frame began
    unsigned k = frame % FSPAN;
    if (atomic_load(&g_fspan_f[k]) != frame) { atomic_store(&g_fspan[k], 0); atomic_store(&g_fspan_f[k], frame); }
    if (ns > atomic_load(&g_fspan[k])) atomic_store(&g_fspan[k], ns);
}
uint64_t vka_floor_span_of(uint64_t frame) { unsigned k = frame % FSPAN; return atomic_load(&g_fspan_f[k]) == frame ? atomic_load(&g_fspan[k]) : 0; }   // 0 = not in (vka_perf.c DM_VK_ADPF_SPAN)
static int cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static int floor_auto(int top, uint64_t f) {
    static int cur = -1; static uint64_t last_f, changed_f;
    if (cur < 0 || cur > top) cur = top;
    if (f == last_f) return cur;
    last_f = f;
    uint64_t v[30]; int n = 0;
    for (uint64_t g = f > 3 ? f - 3 : 0; g > 0 && g + 34 > f && n < 30; g--) { unsigned k = g % FSPAN; if (atomic_load(&g_fspan_f[k]) == g && atomic_load(&g_fspan[k])) v[n++] = atomic_load(&g_fspan[k]); }
    if (n < 20) return cur;
    qsort(v, n, sizeof *v, cmp_u64);
    double p90 = v[n * 9 / 10] / 1e6, per = atomic_load_explicit(&vka_period_ns, memory_order_relaxed) / 1e6;
    if (per <= 0 || per > 33.4) per = 33.34;
    int was = cur;
    g_p90_sum += p90; g_p90_n++;
    double hi = VKA_ENVF("DM_VK_GPU_FLOOR_HI", 2.0), lo = VKA_ENVF("DM_VK_GPU_FLOOR_LO", 5.0);   // ms below the period: raise above per - hi, lower below per - lo
    // A step down that has to be taken back within 60 frames is not tried again for a while (300 frames, doubling up to 4,800 each time it fails again): on the Adreno
    // 840 a floor of 75 instead of 100 added 5-15 ms to the level's span at once, and the controller went up and down every few frames
    static uint64_t hold_until, backoff = 300, lowered_f;
    if (p90 > per - 0.5) cur = top;
    else if (p90 > per - hi) cur = cur + 25 > top ? top : cur + 25;
    else if (p90 < per - lo && f - changed_f >= 60 && f >= hold_until) { cur = cur - 25 < 0 ? 0 : cur - 25; lowered_f = f; }
    if (cur > was && lowered_f && f - lowered_f < 60) { hold_until = f + backoff; backoff = backoff * 2 > 4800 ? 4800 : backoff * 2; lowered_f = 0; }
    if (lowered_f && f - lowered_f > 600) { backoff = 300; lowered_f = 0; }   // a step down that held
    if (cur != was) { changed_f = f; if (VKA_ENV("DM_VK_GPU_FLOOR_LOG", 0)) VLOG("GPU clock floor %d -> %d %%: frame %llu, span p90 %.1f ms of %.1f", was, cur, (unsigned long long)f, p90, per); }
    g_floor_frames[cur / 25 < 5 ? cur / 25 : 4]++;
    return cur;
}
void vka_floor_stats(char *b, size_t n) {   // the replay statistics: frames at each floor step since the last call
    uint64_t t = 0; for (int i = 0; i < 5; i++) t += g_floor_frames[i];
    if (!t) { if (n) b[0] = 0; return; }
    snprintf(b, n, "GPU clock floor (auto): frames at 0/25/50/75/100 %%: %.0f/%.0f/%.0f/%.0f/%.0f %%; recording start to GPU end p90 %.1f ms on average", 100.0 * g_floor_frames[0] / t, 100.0 * g_floor_frames[1] / t, 100.0 * g_floor_frames[2] / t,
             100.0 * g_floor_frames[3] / t, 100.0 * g_floor_frames[4] / t, g_p90_n ? g_p90_sum / g_p90_n : 0.0);
    g_p90_sum = 0; g_p90_n = 0;
    memset(g_floor_frames, 0, sizeof g_floor_frames);
}
static _Atomic int g_tail_n;   // DM_VK_GPU_FLOOR_AUTO=2: submissions with draws counted at their commit and not yet completed
// The queue's hint (any thread: the replay's, the completion thread's). tail: the floor is s while a counted submission is pending, else 0, decided under the queue lock, so
// a commit and a completion racing cannot apply their floors in the wrong order (before 2026-11-03 a completion's 0 could land after the next commit's floor)
static void floor_set_(int s, int tail) {
    static _Atomic int cur = -1;
    if (!tail && atomic_load(&cur) == s) return;
    vka_qlock();
    if (tail) s = atomic_load(&g_tail_n) > 0 ? s : 0;
    if (atomic_load(&cur) == s) { vka_qunlock(); return; }
    struct { VkStructureType sType; void *pNext; uint32_t type, scale; } h = {(VkStructureType)1000302000, NULL, s >= 100 ? 2u : s > 0 ? 3u : 0u, s > 0 && s < 100 ? (uint32_t)s : 0u};
    VkResult r = ((VkResult (*)(VkQueue, const void *))V.perf_hint)(V.q, &h);
    int was = atomic_exchange(&cur, s);
    vka_qunlock();
    if (was < 0 || VKA_ENV("DM_VK_GPU_FLOOR_LOG", 0) > 1) VLOG("GPU clock floor: %d %% (%s): %d", s, s >= 100 ? "highest" : s ? "scaled" : "default governor", r);
}
// DM_VK_GPU_FLOOR_AUTO=2: the floor only for the tail. The driver's governor keeps the clock while the frame is recorded (its parts reach the GPU as fast as they are
// recorded, so a faster GPU only waits longer); when a submission with draws is handed to the queue the floor goes to DM_VK_GPU_FLOOR, and back to the governor once
// every such submission handed over has completed. A submission is counted when its span op is posted (vka_floor_tail_add) and the count is given back by that op on
// completion, so the two always pair: before 2026-11-03 every completed submission with draws took one off, also those committed before a live switch to mode 2 or whose
// end failed, and the floor could drop with a counted submission still on the GPU
static int floor_mode(void) { return V.perf_hint ? VKA_ENV("DM_VK_GPU_FLOOR_AUTO", 0) : -1; }
static int floor_top(void) { int s = VKA_ENV("DM_VK_GPU_FLOOR", 100); return s < 0 ? 0 : s > 100 ? 100 : s; }
int vka_floor_tail_add(void) { if (floor_mode() != 2) return 0; atomic_fetch_add(&g_tail_n, 1); return 1; }   // vka_exec.c, a submission with draws: 1 = counted
void vka_floor_commit(int counted) { if (counted) floor_set_(floor_top(), 1); }   // before that submission is handed to the queue
void vka_floor_done(int counted) {     // the completion thread, that submission done
    if (!counted) return;
    atomic_fetch_sub(&g_tail_n, 1);
    if (floor_mode() == 2) floor_set_(floor_top(), 1);
}
void vka_gpu_floor(void) {
    int mode = floor_mode(); if (mode < 0 || mode == 2) return;
    int s = VKA_ENV("DM_VK_GPU_FLOOR", 100); if (s < 0) s = 0; if (s > 100) s = 100;
    uint64_t vka_frame(void);
    if (mode == 1) s = floor_auto(s, vka_frame());
    floor_set_(s, 0);
}
#endif
