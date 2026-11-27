// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

uint Pack565(vec3 color) {
    const uvec3 q = uvec3(round(clamp(color, 0.0, 1.0) * vec3(31.0, 63.0, 31.0)));
    return (q.r << 11) | (q.g << 5) | q.b;
}

vec3 Unpack565(uint value) {
    const uvec3 q = uvec3((value >> 11) & 31u, (value >> 5) & 63u, value & 31u);
    const uvec3 e = uvec3((q.r << 3) | (q.r >> 2), (q.g << 2) | (q.g >> 4), (q.b << 3) | (q.b >> 2));
    return vec3(e) / 255.0;
}

float Distance2(vec3 a, vec3 b) {
    const vec3 d = a - b;
    return dot(d, d);
}

// BC1 colour block. With punch_through (BC1 only), texels with alpha < 0.5 use the transparent
// index of the three-colour mode; otherwise the four-colour mode is used (BC3 always decodes so).
uvec2 EncodeColor(bool punch_through) {
    vec3 lo = vec3(1.0);
    vec3 hi = vec3(0.0);
    for (int i = 0; i < 16; ++i) {
        if (punch_through && Texel(i).a < 0.5) {
            continue;
        }
        lo = min(lo, Texel(i).rgb);
        hi = max(hi, Texel(i).rgb);
    }
    if (any(greaterThan(lo, hi))) {
        // Every texel transparent: colour0 == colour1 selects three-colour mode, index 3.
        return uvec2(0u, 0xFFFFFFFFu);
    }
    const vec3 center = (lo + hi) * 0.5;
    const vec3 extent = hi - lo;
    const int major = extent.g >= extent.r && extent.g >= extent.b ? 1 : (extent.r >= extent.b ? 0 : 2);
    vec3 covariance = vec3(0.0);
    for (int i = 0; i < 16; ++i) {
        if (punch_through && Texel(i).a < 0.5) {
            continue;
        }
        const vec3 d = Texel(i).rgb - center;
        covariance += d * d[major];
    }
    vec3 e0 = hi;
    vec3 e1 = lo;
    for (int c = 0; c < 3; ++c) {
        if (covariance[c] < 0.0) {
            e0[c] = lo[c];
            e1[c] = hi[c];
        }
    }
    const vec3 inset = (e0 - e1) / 16.0;
    e0 -= inset;
    e1 += inset;

    uint q0 = Pack565(e0);
    uint q1 = Pack565(e1);
    // Four-colour mode needs colour0 > colour1, three-colour mode colour0 <= colour1.
    if (punch_through ? q0 > q1 : q0 < q1) {
        const uint t = q0;
        q0 = q1;
        q1 = t;
    }
    if (!punch_through && q0 == q1) {
        return uvec2(q0 | (q1 << 16), 0u);
    }
    vec3 palette[4];
    palette[0] = Unpack565(q0);
    palette[1] = Unpack565(q1);
    if (punch_through) {
        palette[2] = (palette[0] + palette[1]) * 0.5;
        palette[3] = vec3(0.0);
    } else {
        palette[2] = (2.0 * palette[0] + palette[1]) / 3.0;
        palette[3] = (palette[0] + 2.0 * palette[1]) / 3.0;
    }
    const int entries = punch_through ? 3 : 4;
    uint indices = 0u;
    for (int i = 0; i < 16; ++i) {
        uint best = 0u;
        if (punch_through && Texel(i).a < 0.5) {
            best = 3u;
        } else {
            float best_distance = Distance2(Texel(i).rgb, palette[0]);
            for (int p = 1; p < entries; ++p) {
                const float distance = Distance2(Texel(i).rgb, palette[p]);
                if (distance < best_distance) {
                    best_distance = distance;
                    best = uint(p);
                }
            }
        }
        indices |= best << (2 * i);
    }
    return uvec2(q0 | (q1 << 16), indices);
}

// BC3 alpha block: eight-value mode (alpha0 > alpha1), indices 0 = alpha0, 1 = alpha1, 2..7 the
// ramp from alpha0 to alpha1.
uvec2 EncodeAlpha() {
    float lo = 1.0;
    float hi = 0.0;
    for (int i = 0; i < 16; ++i) {
        lo = min(lo, Texel(i).a);
        hi = max(hi, Texel(i).a);
    }
    const uint a0 = uint(round(hi * 255.0));
    const uint a1 = uint(round(lo * 255.0));
    uint bits_lo = a0 | (a1 << 8);
    uint bits_hi = 0u;
    if (a0 == a1) {
        return uvec2(bits_lo, bits_hi);
    }
    for (uint i = 0u; i < 16u; ++i) {
        const float t = (Texel(int(i)).a * 255.0 - float(a1)) / float(a0 - a1);
        const uint q = uint(clamp(round(t * 7.0), 0.0, 7.0));
        const uint index = q == 7u ? 0u : (q == 0u ? 1u : 8u - q);
        const uint bit = 16u + 3u * i;
        if (bit < 32u) {
            bits_lo |= index << bit;
            if (bit > 29u) {
                bits_hi |= index >> (32u - bit);
            }
        } else {
            bits_hi |= index << (bit - 32u);
        }
    }
    return uvec2(bits_lo, bits_hi);
}
