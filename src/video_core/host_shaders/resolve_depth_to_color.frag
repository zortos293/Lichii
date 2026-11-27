// SPDX-License-Identifier: GPL-3.0-or-later

#version 450 core

layout(binding = 0) uniform sampler2DMS depth_tex;
layout(location = 0) in vec2 texcoord;
layout(location = 0) out float color;

void main() {
    ivec2 coord = ivec2(texcoord);
    color = (texelFetch(depth_tex, coord, 0).r + texelFetch(depth_tex, coord, 1).r +
             texelFetch(depth_tex, coord, 2).r + texelFetch(depth_tex, coord, 3).r) * 0.25;
}
