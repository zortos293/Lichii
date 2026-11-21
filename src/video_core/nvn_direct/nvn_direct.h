// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVN direct renderer: with the nvn_direct_renderer setting (or
// EDEN_NX2_NVN_DIRECT=1) a game's NVN calls run in Banana Native's NVN front end (bananative/dm/gfx/nvn/hle_nvn.c: objects and command
// records in the game's own memory) and its NVN -> Vulkan replay (bananative/dm/gfx/vk_android),
// not in the game's NVN driver and Lichii's Maxwell GPU path. This is the C interface between
// them and Lichii; the replay's own headers stay out of C++.
//
// One launch per process: the front end and the replay keep process-lifetime state (globals,
// detached threads), so a later game in the same process falls back to Lichii's GPU emulation
// (Core::NvnHook::Install, RendererChoice::EarlierBoot).

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/// At most this many queues of the graphics family are handed to the replay (NVIDIA's family has
/// 16): one per NVN queue the game keeps alive, plus the replay's own work.
#define NVN_DIRECT_MAX_QUEUES 16

/// One level of one layer of an ASTC texture on a device without ASTC: its
/// block-linear ASTC blocks as the game stored them, decoded and re-compressed to BC on the GPU
/// into image's level and layer (VK_IMAGE_LAYOUT_GENERAL, a BC1 or BC3 format).
typedef struct nvn_direct_astc_level {
    VkBuffer source;
    VkDeviceSize source_offset; ///< where the level (of the layer) starts
    VkDeviceSize source_size;   ///< the level's bytes from there
    uint32_t width, height;     ///< the level's texels
    uint32_t block_width, block_height; ///< the ASTC block's texels
    uint32_t gobs_x;            ///< GOB columns of a row of the level, as laid out
    uint32_t block_height_log2; ///< GOBs of a block's height at this level (log2)
    int switch2_gobs;           ///< the Switch 2's order of 16-byte pieces in a GOB
    VkImage image;
    uint32_t level, layer;
    int bc3; ///< BC3 (with alpha), else BC1
} nvn_direct_astc_level;

/// One NVN program stage for Lichii's shader recompiler: its code and control
/// section, and the reads of the draw's state translation makes (constant buffer words, the
/// texture formats of handles, guest memory).
typedef struct nvn_direct_shader_source {
    uint32_t nvn_stage;        ///< 0 vertex, 1 fragment, 2 geometry, 3 tess control, 4 tess evaluation, 5 compute, 6 task, 7 mesh
    /// The control section: NVN2's (NVN_DIRECT_NVN2_CONTROL_SIZE bytes, version 0xc9 at +4) or the
    /// first NVN_DIRECT_NVN1_CONTROL_SIZE bytes of NVN1's (Maxwell code)
    const uint8_t* control;
    const uint8_t* code;       ///< the code area in host memory
    uint32_t code_size;        ///< bytes of the code area
    uint64_t code_address;     ///< the code area's guest address
    uint32_t first_binding;    ///< set 0 binding of the stage's first descriptor
    uint8_t vertex_input_types[32]; ///< vertex: generic input types, 0 float, 1 signed, 2 unsigned
    int convert_depth;         ///< pre-rasterization stages: z from -w..w to 0..w (the GL depth mode)
    uint32_t per_primitive_inputs; ///< fragment after a mesh program: the generic inputs it writes per primitive
    void* context;
    uint32_t (*read_cbuf)(void* context, uint32_t bank, uint32_t offset);
    /// 1 = the size bytes at guest address were read into out
    int (*read_memory)(void* context, uint64_t address, void* out, size_t size);
    /// The texture a handle names: its NVN target and VkFormat; 0 = no texture
    int (*texture_info)(void* context, uint32_t handle, uint32_t* nvn_target, int32_t* vk_format);
    /// Fritchii, mesh: emit the program as a compute shader writing the mesh emulation's blocks
    /// (Shader::Backend::SPIRV::MeshEmulationBlockWords) to its NVN_DIRECT_DESC_MESH_OUT buffer,
    /// and the vertex shader that draws them (nvn_direct_shader::vs_spirv)
    uint32_t mesh_fallback;
} nvn_direct_shader_source;

#define NVN_DIRECT_NVN2_CONTROL_SIZE 0xd8
#define NVN_DIRECT_NVN1_CONTROL_SIZE 0x760

/// What a descriptor of a translated shader reads (Shader::Info's descriptor lists)
enum {
    NVN_DIRECT_DESC_CBUF,           ///< constant buffer cbuf_index
    NVN_DIRECT_DESC_STORAGE,        ///< storage buffer at the address in c[cbuf_index][cbuf_offset]
    NVN_DIRECT_DESC_TEXTURE_BUFFER, ///< texel buffers and textures: the handle in c[cbuf_index][cbuf_offset]
    NVN_DIRECT_DESC_IMAGE_BUFFER,
    NVN_DIRECT_DESC_TEXTURE,
    NVN_DIRECT_DESC_IMAGE,
    NVN_DIRECT_DESC_LOOP_GUARD,     ///< storage buffer whose first word a loop's safety check sets when it ends a loop (at first_binding + NVN_DIRECT_LOOP_GUARD_SLOT)
    NVN_DIRECT_DESC_MESH_OUT,       ///< Fritchii: the mesh emulation's output buffer (at first_binding + NVN_DIRECT_MESH_OUT_SLOT)
};
#define NVN_DIRECT_LOOP_GUARD_SLOT 62
#define NVN_DIRECT_MESH_OUT_SLOT 63
typedef struct nvn_direct_shader_desc {
    uint8_t kind;              ///< NVN_DIRECT_DESC_*
    uint8_t vk_type;           ///< VkDescriptorType
    uint16_t binding;
    uint32_t count;            ///< descriptors of an array (element k: the handle at cbuf_offset + (k << size_shift))
    uint32_t cbuf_index, cbuf_offset, shift_left;
    uint32_t secondary_index, secondary_offset, secondary_shift_left; ///< a separate sampler word (has_secondary)
    uint32_t size_shift;
    uint8_t has_secondary;
    uint8_t texture_type;      ///< Shader::TextureType
    uint8_t is_depth, is_multisample, is_integer, is_written, is_atomic;
    /// Not a plain constant buffer word: a handle computed per draw, a pointer read from memory
    /// (the replay's bindings cannot fill it). 3: a handle program the replay evaluates per draw.
    /// 4: a storage buffer whose address is read from memory at its pointer + secondary_offset
    uint8_t unsupported;
    uint32_t handle_program;   ///< unsupported 3: the program's index in nvn_direct_shader::hprog
} nvn_direct_shader_desc;

/// A translated stage. Everything is allocated by the translator and given back with shader_free.
typedef struct nvn_direct_shader {
    uint32_t* spirv;
    size_t spirv_words;
    nvn_direct_shader_desc* descs;
    uint32_t desc_count;
    uint32_t local[3], shared; ///< compute, task and mesh: the workgroup; compute: shared memory bytes
    uint32_t input_mask;       ///< vertex: generic inputs read
    uint32_t output_mask;      ///< fragment: colour outputs written
    uint32_t per_primitive_outputs; ///< mesh: generic outputs written per primitive
    uint32_t ssbo_align;       ///< storage buffers are bound at their address rounded down to this
    uint32_t uses_render_area; ///< graphics: reads the render area (NVN_DIRECT_RENDER_AREA_OFFSET)
    /// Handle programs (Shader::Info::handle_programs) and the storage buffers they read, as words:
    /// program count, then each program's length and words; storage buffer count, then each one's
    /// cbuf index, cbuf offset and indirect offset (~0: none). NULL when the shader has none
    uint32_t* hprog;
    uint32_t hprog_words;
    /// Fritchii, mesh_fallback: the vertex shader drawing the blocks (its output buffer at the
    /// vertex range's NVN_DIRECT_MESH_OUT_SLOT), a block's words, the vertices a workgroup draws
    /// and their topology (0 triangles, 1 lines, 2 points)
    uint32_t* vs_spirv;
    size_t vs_words;
    uint32_t mesh_block_words, mesh_group_vertices, mesh_topology;
    char error[256];           ///< why it was not translated
} nvn_direct_shader;

/// Lichii's Vulkan device as the replay adopts it (vka_dev.c makes none of its own).
typedef struct nvn_direct_device {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t queue_family;
    /// The replay's queues: the NVN queues take all but the last one, the replay's own work
    /// (uploads, sparse binds) the last, which is Lichii's graphics queue.
    uint32_t queue_count;
    VkQueue queues[NVN_DIRECT_MAX_QUEUES];
    /// The device extensions Lichii enabled (the replay uses no other).
    const char* const* extensions;
    uint32_t extension_count;
    /// Every queue submission of the replay holds this lock (Lichii: Scheduler::submit_mutex).
    void (*queue_lock)(void* context);
    void (*queue_unlock)(void* context);
    void* queue_context;
    /// Records the decode of an ASTC level (Lichii's astc_decoder.comp and bc_encoder.comp)
    /// into cmdbuf, the replay's upload batch; 0 when nothing was recorded. NULL: no GPU
    /// decode (ASTC textures then have no image unless the device samples ASTC).
    int (*astc_to_bc)(void* context, VkCommandBuffer cmdbuf, const nvn_direct_astc_level* level);
    /// Records what astc_to_bc left pending in cmdbuf (small levels it batches); the replay calls
    /// it before it ends an upload batch. NULL: nothing is left pending.
    void (*astc_flush)(void* context, VkCommandBuffer cmdbuf);
    void* astc_context;
    /// Whether ASTC blocks decode without alpha (Lichii's ASTC::IsOpaque): the image of such a
    /// texture may be BC1 instead of BC3, half the memory. NULL: always BC3.
    int (*astc_opaque)(const void* blocks, size_t size);
    /// The process's device-local memory use and the budget it should stay within (Lichii's
    /// Device::GetDeviceMemoryUsage / GetDeviceMemoryBudget, EDEN_NX2_VRAM_LIMIT_MIB included):
    /// the replay lets go of images it can make again when the use nears the budget. NULL or a
    /// budget of 0: no budget.
    uint64_t (*memory_usage)(void* context);
    uint64_t (*memory_budget)(void* context);
    void* memory_context;
    /// A pipeline the replay makes off the frame path (one its draws skip meanwhile, or one made
    /// ahead from an earlier run's keys) starts and is done: Lichii's "building shaders" count
    /// (VideoCore::ShaderNotify). NULL: not reported.
    void (*pipeline_building)(void* context);
    void (*pipeline_built)(void* context);
    void* pipeline_context;
    /// Lichii's asynchronous shader building: a draw whose pipeline is not made yet is skipped
    /// until asynchronous threads have made it, instead of the frame waiting for its compile
    /// (vka_pso.c async_threads).
    int async_pipelines;
    /// Translates a program stage with Lichii's shader recompiler: 1 = done,
    /// 0 = not (shader->error says why). NULL: no recompiler.
    int (*shader_translate)(void* context, const nvn_direct_shader_source* source, nvn_direct_shader* shader);
    void (*shader_free)(void* context, nvn_direct_shader* shader);
    void* shader_context;
    /// Lichii's resolution setting (Settings::ResolutionScalingInfo): render targets are made
    /// scale_up / 2^scale_shift times their size. 1 and 0: no scale.
    uint32_t scale_up, scale_shift;
    /// Whether the replay labels its command buffers with the game's debug groups and markers
    /// (VK_EXT_debug_utils), as Lichii labels its own: a debugging tool attached, or renderer debug.
    int debug_labels;
} nvn_direct_device;

/// The push constants of a translated stage that Lichii's RescalingPass changed
/// (Shader::Backend::SPIRV::RescalingLayout): a bit per texture and per image descriptor that is
/// scaled, then the pixel stage's down factor. A pipeline's stages are translated one by one, so
/// each stage's descriptors take bits of their own: pixel and compute from bit 0, vertex and mesh
/// from 64 (textures) / 32 (images), the other stages from 96 / 48.
#define NVN_DIRECT_SCALE_PUSH_SIZE 32
#define NVN_DIRECT_SCALE_IMAGES_OFFSET 16
#define NVN_DIRECT_SCALE_DOWN_FACTOR_OFFSET 24
/// The render area a stage reads in place of the console's blit parameters (Lichii's
/// BlitConstantsPass, RenderAreaLayout: width, height and two unused words) follows them in the
/// same push-constant range.
#define NVN_DIRECT_RENDER_AREA_OFFSET NVN_DIRECT_SCALE_PUSH_SIZE
#define NVN_DIRECT_RENDER_AREA_SIZE 16
static inline uint32_t nvn_direct_scale_texture_bit(uint32_t nvn_stage) {
    return nvn_stage == 1 || nvn_stage == 5 ? 0 : nvn_stage == 0 || nvn_stage == 7 ? 64 : 96;
}
static inline uint32_t nvn_direct_scale_texture_bits(uint32_t nvn_stage) { return nvn_stage == 1 || nvn_stage == 5 ? 64 : 32; }
static inline uint32_t nvn_direct_scale_image_bit(uint32_t nvn_stage) {
    return nvn_stage == 1 || nvn_stage == 5 ? 0 : nvn_stage == 0 || nvn_stage == 7 ? 32 : 48;
}
static inline uint32_t nvn_direct_scale_image_bits(uint32_t nvn_stage) { return nvn_stage == 1 || nvn_stage == 5 ? 32 : 16; }

/// Hands Lichii's device to the replay and initialises it (vka_init); returns whether it is up.
/// Called once, after the device exists. When it fails the boot falls back to GPU emulation
/// (Core::NvnHook::DirectFailed).
int nvn_direct_adopt_device(const nvn_direct_device* device);
/* Fritchii: one NVN2 program from the game files, translated and its pipeline library made ahead (nvn_recompile.c). 1 made, 0 known already, -1 failed */
int vka_rc_prepare(const uint8_t* control, size_t control_size, const uint8_t* code, size_t code_size);

/// Lichii's guest memory as the replay's memory pools need it.
typedef struct nvn_direct_memory {
    /// Maps the pages behind guest [address, address + size) (page aligned) once more, at a host
    /// range of their own: read-write and never protected, so Vulkan imports them while the
    /// guest's own view (the fastmem arena) may be write-tracked or partly unmapped. Pages the
    /// guest has not mapped are zero pages of the view (their bytes in *unmapped). NULL on
    /// failure. The view is given back with unmap_view.
    void* (*map_view)(void* context, uint64_t address, uint64_t size, uint64_t* unmapped);
    /// Takes down a view map_view made, of size bytes.
    void (*unmap_view)(void* context, void* view, uint64_t size);
    /// Arms (1) or disarms (0) Lichii's tracking of CPU writes to the pool pages [address,
    /// address + size): a write to an armed page is reported (nvn_direct_cpu_written).
    void (*track_pages)(void* context, uint64_t address, uint64_t size, int armed);
    /// The backing memory of the guest page at address (never write-protected), NULL when the
    /// guest has not mapped it.
    void* (*backing)(void* context, uint64_t address);
    /// The guest range [*address, *address + *size) of a Switch 2 native image handle (nx2:gsv's
    /// images, as gsv 428 answers it); 0 when the handle names no image with bound memory.
    int (*native_image)(void* context, uint64_t handle, uint64_t* address, uint64_t* size);
    void* context;
    /// The file behind the backing memory (Lichii's memfd), -1 when there is none, and the
    /// backing view's base: the backing byte at p is the file's byte p - backing_base. Pools are
    /// imported as udmabufs of it where the driver refuses host-pointer imports of it (amdgpu).
    int backing_fd;
    const uint8_t* backing_base;
#if defined(_WIN32)
    /// Windows, where there is no file to map: a virtual pool's host range (HostMemory's alias
    /// regions), inaccessible until alias_map maps backing memory [backing_offset, backing_offset
    /// + size) into it at address (0 on failure); alias_unmap makes pages inaccessible again and
    /// alias_release gives the range back.
    void* (*alias_reserve)(void* context, uint64_t size);
    int (*alias_map)(void* context, void* address, uint64_t backing_offset, uint64_t size);
    void (*alias_unmap)(void* context, void* address, uint64_t size);
    void (*alias_release)(void* context, void* region);
#endif
} nvn_direct_memory;

/// Takes the front end's slot table (the names of upstream's names.txt, in its order), the
/// arena the game's addresses are offsets into (host = arena + address for every address below
/// guest_size), the guest memory's views and the game's NVN: switch2 = NVN2 (else NVN1), sdk_major
/// the NintendoSDK major its sdk was built with (-1 unknown, 0 a prerelease SDK 0.x; NVN1 reports
/// other API versions to SDK 10 and older). Called once, when the game's NVN is taken over.
void nvn_direct_init(uint8_t* arena, uint64_t guest_size, const nvn_direct_memory* memory,
                     int switch2, int sdk_major);

/// Lichii saw a CPU write to tracked pool memory [address, address + size) (guest addresses):
/// textures over it are uploaded again, and its pages are no longer tracked until uploaded.
void nvn_direct_cpu_written(uint64_t address, uint64_t size);

/// The guest unmaps [address, address + size): the pool memory imported over it is given back
/// first (after the GPU is done with it), so no import keeps pages the guest reuses. Then the
/// replay's readers of guest memory wait until nvn_direct_guest_unmap_done, which the caller
/// calls once the pages are unmapped: a reader that found a page mapped reads it before.
void nvn_direct_guest_unmap(uint64_t address, uint64_t size);
void nvn_direct_guest_unmap_done(void);
int nvn_direct_name_count(void);
const char* nvn_direct_name(int slot);
/// Whether the front end implements the slot's function (else the hook's stub answers 0).
int nvn_direct_implemented(int slot);
/// Whether the function takes or returns floats (v registers).
int nvn_direct_float_args(int slot);

#define NVN_DISPATCH_FLOAT_ARGS 1
#define NVN_DISPATCH_FLOAT_RETURN 2

/// Runs an implemented slot's function with the guest's x0-x7 and v0-v7 (low 64 bits) as its
/// arguments, and the guest's stack at sp for the arguments past them; returns x0, and v0 through
/// *v0 for a float result. lr: the guest's return address (the call site the front end records).
uint64_t nvn_direct_call(int slot, const uint64_t x[8], const uint64_t v[8], uint64_t sp, uint64_t lr,
                         uint64_t* v0);

/// A presented image as Lichii's presenter shows it. The image stays in
/// VK_IMAGE_LAYOUT_GENERAL; it is drawn on the replay's queues and finished at fence.
typedef struct nvn_direct_frame {
    uint64_t id;
    VkImage image;
    VkImageView view; ///< 2D, level 0, layer 0, sampled only, in format
    VkFormat format;
    uint32_t width, height;           ///< the texture's size (the crop's units)
    uint32_t scaled_width, scaled_height; ///< the image's (the render scale's)
    int32_t crop[4]; ///< x, y, width, height the game shows (nvnWindowSetCrop); width 0: all of it
    uint32_t interval; ///< the window's present interval (vblanks per frame)
    uint64_t fence;
} nvn_direct_frame;

/// Lichii's framebuffer address of a direct-mode frame: id | NVN_DIRECT_FRAME_TAG (no device
/// address has that bit).
#define NVN_DIRECT_FRAME_TAG (1ull << 62)

/// The image of the present the calling thread just made (nvnQueuePresentTexture): waits until
/// the replay reached it and keeps a reference to it for Lichii. 0: nothing to show (the null
/// backend, or a texture the replay never drew or uploaded).
int nvn_direct_present_take(nvn_direct_frame* frame);

/// On Lichii's GPU thread: the frame taken with id (the newest one held if it was let go
/// already), once the GPU finished drawing it (waits, bounded). 0: no frame was ever taken.
int nvn_direct_present_frame(uint64_t id, nvn_direct_frame* frame);

/// Whether an implemented slot's call would wait for the replay's GPU now:
/// nvnSyncWait on a fenced sync the GPU has not reached, nvnQueueFinish on a queue whose last
/// submission it has not. Lichii then sleeps the guest thread in the guest kernel until it would
/// not, so the core runs the game's other threads meanwhile.
int nvn_direct_call_blocks(int slot, const uint64_t x[8]);

/// The emulator shuts down, before the guest's cores are suspended: nothing
/// more is replayed or submitted, every fence reads as done, and the game's threads waiting on the
/// host inside an NVN call (a fence, the replay worker's queue, a present) return. For the rest of
/// the process.
void nvn_direct_stop(void);

/// Before Lichii's device goes away (after nvn_direct_stop, which it implies): saves the pipeline
/// cache a last time and waits (bounded) until every thread of the replay and the front end has
/// left; the process exits when one does not.
void nvn_direct_park(void);

/// After nvn_direct_park, once the device is idle and before it is destroyed: the replay's Vulkan
/// objects, its imports and views of guest memory and the front end's mappings of it are
/// destroyed, and a line says whether any object was left.
void nvn_direct_destroy(void);

/// nvnDeviceGetInteger's answer for item, as the game's own driver gave it.
void nvn_direct_set_device_integer(unsigned item, int value);

#ifdef __cplusplus
}
#endif
