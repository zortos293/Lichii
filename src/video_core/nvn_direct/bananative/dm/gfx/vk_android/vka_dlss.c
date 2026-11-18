// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Lichii: NVN2's DLSS evaluation (OP_DLSS, hle_nvn.c "DLSS, dirty tile maps and cooperative-vector matrices") on a host without NVIDIA's network: a spatial upscale. The input colour's subrectangle is drawn
// into the output's with bilinear filtering, each output pixel sampled where the input's jitter put it: input texel k sampled the scene at k + 0.5 - jitter (the
// sign temporal_upscale.frag measured on Fire Emblem Fortune's Weave's DLSS jitter), so the output pixel at input position p reads the input at p + jitter, clamped
// to the subrectangle's texel centres. Without a colour attachment, a draw pipeline or push descriptors: a blit of the subrectangles, without the jitter
#if defined(__ANDROID__)
#include "vka.h"
#include "vka_present_rot.h"
#include "vka_dlss_spv.h"
#include <string.h>

enum { NPIPE = 8 };
static struct { VkFormat f; VkPipeline p; } g_pipe[NPIPE]; static int g_npipe;
static VkPipelineLayout g_lay; static VkDescriptorSetLayout g_dsl; static VkShaderModule g_vs, g_fs; static VkSampler g_samp;

static int ready(void) {
    static int st; if (st) return st > 0;
    st = -1;
    if (!V.push_desc) return 0;
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 1, .pBindings = &b};
    VkPushConstantRange pc[2] = {{VK_SHADER_STAGE_VERTEX_BIT, 0, 6 * sizeof(float)}, {VK_SHADER_STAGE_FRAGMENT_BIT, 32, 4 * sizeof(float)}};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_dsl, .pushConstantRangeCount = 2, .pPushConstantRanges = pc};
    VkShaderModuleCreateInfo mv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vka_rot_vert, .pCode = vka_rot_vert};
    VkShaderModuleCreateInfo mf = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vka_dlss_frag, .pCode = vka_dlss_frag};
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkResult r;
    if ((r = vk.vkCreateDescriptorSetLayout(V.dev, &dl, NULL, &g_dsl)) || (r = vk.vkCreatePipelineLayout(V.dev, &li, NULL, &g_lay)) || (r = vk.vkCreateShaderModule(V.dev, &mv, NULL, &g_vs)) ||
        (r = vk.vkCreateShaderModule(V.dev, &mf, NULL, &g_fs)) || (r = vk.vkCreateSampler(V.dev, &si, NULL, &g_samp))) { VRED("DLSS upscale: layout / shaders / sampler %d: blits instead", r); return 0; }
    st = 1; return 1;
}
static VkPipeline pipeline(VkFormat f) {
    for (int i = 0; i < g_npipe; i++) if (g_pipe[i].f == f) return g_pipe[i].p;
    if (g_npipe == NPIPE) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = g_vs, .pName = "main"},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = g_fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo dss = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &f};
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rci, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &dss, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = g_lay};
    VkPipeline p = VK_NULL_HANDLE; VkResult r = vk.vkCreateGraphicsPipelines(V.dev, VK_NULL_HANDLE, 1, &gi, NULL, &p);
    if (r) { VRED_ONCE("DLSS upscale pipeline (format %d): %d: blits instead", f, r); return VK_NULL_HANDLE; }
    g_pipe[g_npipe++] = (typeof(g_pipe[0])){f, p};
    return p;
}
// A texture's level and layer (its view's) as level / layer of its image, and the format the view reads it in
static int view_of(vka_img *im, const tex_t *t, const view_t *v, int has_v, uint32_t *lv, uint32_t *layer, vka_fmt *f) {
    *lv = has_v && v->set_levels ? v->base_level : 0; *layer = has_v && v->set_layers ? v->min_layer : 0;
    return vka_img_lv(im, lv) && *layer < im->layers && vka_fmt_of(has_v && v->set_format ? v->format : t->d.format, f);
}
#define SC(im, v) ((im)->scaled ? vka_scaled(v) : (v))
void vka_dlss_eval(VkCommandBuffer cb, const p_dlss *c) {
    vka_img *s = vka_tex_get(c->color, cb), *d = vka_tex_get(c->output, cb);
    uint32_t sl, sz, dl, dz; vka_fmt sf, df;
    if (!s || !d || !view_of(s, c->color, &c->cv, c->has_cv, &sl, &sz, &sf) || !view_of(d, c->output, &c->ov, c->has_ov, &dl, &dz, &df)) return;
    if (s->samples > 1 || d->samples > 1 || sf.kind || df.kind) { VRED_ONCE("DLSS evaluation of formats %#x → %#x or multisampled textures: not evaluated", c->color->d.format, c->output->d.format); return; }
    static uint64_t last_net;
    if (c->net != last_net) {
        last_net = c->net;
        VLOG("DLSS evaluation, network %llu: %dx%d at %d,%d of texture serial %llu (%#x) → %dx%d at %d,%d of texture serial %llu (%#x), jitter %.3f,%.3f%s", (unsigned long long)c->net,
             c->in_size[0], c->in_size[1], c->in_base[0], c->in_base[1], (unsigned long long)s->serial, c->color->d.format, c->out_size[0], c->out_size[1], c->out_base[0], c->out_base[1],
             (unsigned long long)d->serial, c->output->d.format, c->jitter[0], c->jitter[1], c->reset ? ", history reset" : "");
    }
    vka_tex_own(d, cb);
    const int32_t sw = (int32_t)(s->w >> sl ? s->w >> sl : 1), sh = (int32_t)(s->h >> sl ? s->h >> sl : 1), dw = (int32_t)(d->w >> dl ? d->w >> dl : 1), dh = (int32_t)(d->h >> dl ? d->h >> dl : 1);
    int32_t ix0 = SC(s, c->in_base[0]), iy0 = SC(s, c->in_base[1]), ix1 = SC(s, c->in_base[0] + c->in_size[0]), iy1 = SC(s, c->in_base[1] + c->in_size[1]);
    const int32_t ox0 = SC(d, c->out_base[0]), oy0 = SC(d, c->out_base[1]), ox1 = SC(d, c->out_base[0] + c->out_size[0]), oy1 = SC(d, c->out_base[1] + c->out_size[1]);
    if (ix0 < 0) ix0 = 0; if (iy0 < 0) iy0 = 0; if (ix1 > sw) ix1 = sw; if (iy1 > sh) iy1 = sh;
    const int32_t cx0 = ox0 < 0 ? 0 : ox0, cy0 = oy0 < 0 ? 0 : oy0, cx1 = ox1 > dw ? dw : ox1, cy1 = oy1 > dh ? dh : oy1;
    if (ix1 <= ix0 || iy1 <= iy0 || cx1 <= cx0 || cy1 <= cy0) return;
    const float js = s->scaled ? (float)vka_scale_up / (float)(1u << vka_scale_shift) : 1.0f;   // the jitter in the image's pixels
    VkImageView sv = vka_view(s, sf.view, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, sl, 1, sz, 1, NULL);
    VkImageView dv = (d->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? vka_view(d, df.view, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, dl, 1, dz, 1, NULL) : VK_NULL_HANDLE;
    VkPipeline p = sv && dv && (s->usage & VK_IMAGE_USAGE_SAMPLED_BIT) && ready() ? pipeline(df.view) : VK_NULL_HANDLE;
    if (p) {
        VkRenderingAttachmentInfo a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        const VkRect2D rc = {{cx0, cy0}, {(uint32_t)(cx1 - cx0), (uint32_t)(cy1 - cy0)}};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = rc, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &a};
        VkDescriptorImageInfo ii = {g_samp, sv, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
        // The vertex shader's texture coordinate at viewport position q (0-1): m q + t, the output's span over the input's, offset by the jitter
        const float vpc[6] = {(float)(ix1 - ix0) / sw, 0, 0, (float)(iy1 - iy0) / sh, (ix0 + c->jitter[0] * js) / sw, (iy0 + c->jitter[1] * js) / sh};
        const float fpc[4] = {(ix0 + 0.5f) / sw, (iy0 + 0.5f) / sh, (ix1 - 0.5f) / sw, (iy1 - 0.5f) / sh};
        const VkViewport v = {(float)ox0, (float)oy0, (float)(ox1 - ox0), (float)(oy1 - oy0), 0, 1};
        vk.vkCmdBeginRendering(cb, &ri);
        vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        vk.vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_lay, 0, 1, &wr);
        vk.vkCmdPushConstants(cb, g_lay, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof vpc, vpc);
        vk.vkCmdPushConstants(cb, g_lay, VK_SHADER_STAGE_FRAGMENT_BIT, 32, sizeof fpc, fpc);
        vk.vkCmdSetViewport(cb, 0, 1, &v); vk.vkCmdSetScissor(cb, 0, 1, &rc);
        vk.vkCmdDraw(cb, 3, 1, 0, 0);
        vk.vkCmdEndRendering(cb);
        vka_dyn_invalidate();   // its pipeline, descriptors and push constants replaced the draws'
    } else {
        VkFormatProperties sp, dp; vk.vkGetPhysicalDeviceFormatProperties(V.pd, s->f.vk, &sp); vk.vkGetPhysicalDeviceFormatProperties(V.pd, d->f.vk, &dp);
        if (!(sp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(dp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
            VRED_ONCE("DLSS evaluation %#x → %#x: neither a draw nor a blit takes these formats: not evaluated", c->color->d.format, c->output->d.format); return; }
        if (ox0 != cx0 || oy0 != cy0 || ox1 != cx1 || oy1 != cy1) { VRED_ONCE("DLSS evaluation %#x → %#x: no draw into the output, and its subrectangle leaves it: not evaluated", c->color->d.format, c->output->d.format); return; }
        VRED_ONCE("DLSS evaluation %#x → %#x: blitted, without the jitter (no draw into the output)", c->color->d.format, c->output->d.format);
        VkImageBlit r = {{VK_IMAGE_ASPECT_COLOR_BIT, sl, sz, 1}, {{ix0, iy0, 0}, {ix1, iy1, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, dl, dz, 1}, {{ox0, oy0, 0}, {ox1, oy1, 1}}};
        vk.vkCmdBlitImage(cb, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r,
                          sp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    vka_img_written(d);
    vka_barrier(cb);
}
#undef SC

// Lichii: the teardown (vka_dev.c vka_destroy)
void vka_dlss_destroy(void) {
    for (int i = 0; i < g_npipe; i++) vk.vkDestroyPipeline(V.dev, g_pipe[i].p, NULL);
    if (g_lay) vk.vkDestroyPipelineLayout(V.dev, g_lay, NULL);
    if (g_dsl) vk.vkDestroyDescriptorSetLayout(V.dev, g_dsl, NULL);
    if (g_vs) vk.vkDestroyShaderModule(V.dev, g_vs, NULL);
    if (g_fs) vk.vkDestroyShaderModule(V.dev, g_fs, NULL);
    if (g_samp) vk.vkDestroySampler(V.dev, g_samp, NULL);
}
#endif
