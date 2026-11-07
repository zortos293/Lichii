// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Draws NVIDIA DLSS's result into the game's image: a box filter when DLSS ran above the image's
// size (resolution scaling renders the scene larger than the game's anti-aliased image), then
// optional RCAS (FidelityFX FSR 1's contrast-adaptive sharpening).

#version 460 core
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_shader_explicit_arithmetic_types : require

layout(binding = 0) uniform sampler2D InputTexture;

layout(push_constant) uniform PushConstants {
    // The vertex stage's texture scale and offset come first.
    layout(offset = 16) uvec4 Const0; // FsrRcasCon
    layout(offset = 32) vec2 Scale;   // source pixels per output pixel
    layout(offset = 40) uint Taps;    // bilinear samples per axis, each averaging 2x2 pixels
    layout(offset = 44) uint Sharpen;
};

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 color;

// The source pixels under output pixel p, averaged. The blit samplers take unnormalized (pixel)
// coordinates.
vec3 Resolve(ivec2 p) {
    const vec2 origin = vec2(p) * Scale;
    const vec2 step = Scale / float(Taps);
    vec3 sum = vec3(0.0);
    for (uint y = 0; y < Taps; ++y) {
        for (uint x = 0; x < Taps; ++x) {
            const vec2 position = origin + (vec2(x, y) + 0.5) * step;
            sum += textureLod(InputTexture, position, 0.0).rgb;
        }
    }
    return max(sum / float(Taps * Taps), vec3(0.0));
}

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"

// RCAS's limits assume 0 to 1: sharpen in a range-compressed space.
vec3 Compress(vec3 c) {
    return c / (1.0 + max(max(c.r, c.g), c.b));
}

vec3 Expand(vec3 c) {
    c = clamp(c, vec3(0.0), vec3(1.0 - 1.0 / 65504.0));
    return c / (1.0 - max(max(c.r, c.g), c.b));
}

#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) {
    return AF4(Compress(Resolve(ivec2(p))), 1.0);
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

#include "ffx_fsr1.h"

void main() {
    if (Sharpen == 0u) {
        color = vec4(Resolve(ivec2(gl_FragCoord.xy)), 1.0);
        return;
    }
    AF3 c;
    FsrRcasF(c.r, c.g, c.b, AU2(gl_FragCoord.xy), Const0);
    color = vec4(Expand(c), 1.0);
}
