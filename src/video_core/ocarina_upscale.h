// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVIDIA DLSS in place of The Legend of Zelda: Ocarina of Time's own anti-aliasing, a small
// network that runs after the tonemap at the render size (the HUD is drawn over its result). The
// game renders without camera jitter and has no motion vectors, so the host jitters the scene's
// draws and derives the camera's motion from the depth and the camera's matrices.

#pragma once

#include <array>
#include <cmath>
#include <optional>
#include <utility>

#include "common/common_types.h"

namespace VideoCommon::OcarinaUpscale {

constexpr u64 ProgramId = 0x0400486024DEC000ULL;

// The network is four dispatches; these two by the hash of their program (ShaderInfo::unique_hash).
// The third samples the tonemapped frame, the fourth writes the network's result.
constexpr u64 InputPassHash = 0xa2fcaa95c5a162f3ULL;
constexpr u64 OutputPassHash = 0x6bd913e1f34e83ebULL;
// Constant bank 0 offsets of their image handles (both B10G11R11 at the render size).
constexpr u32 InputColorOffset = 0xa8;
constexpr u32 OutputColorOffset = 0xd0;

// The scene's draws bind the camera as vertex constant buffer 3: the view-projection matrix (rows,
// clip = M * world) at 0x30 and the projection at 0x70.
constexpr u32 CameraBuffer = 3;
constexpr u32 ViewProjectionOffset = 0x30;
constexpr u32 ProjectionOffset = 0x70;
constexpr u32 CameraBytes = 0xb0;

/// Frames in a cycle of the camera jitter.
constexpr u32 JitterPhases = 16;

using Matrix = std::array<f64, 16>; ///< Row-major 4x4.

/// Whether a projection (rows) is a GL-style perspective one, as the scene's camera uses.
[[nodiscard]] inline bool IsPerspective(const std::array<f32, 16>& projection) {
    for (const f32 value : projection) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return projection[0] > 0.0f && projection[5] > 0.0f && projection[14] == -1.0f &&
           projection[15] == 0.0f && projection[12] == 0.0f && projection[13] == 0.0f;
}

[[nodiscard]] inline f32 Halton(u32 index, u32 base) {
    f32 result = 0.0f;
    f32 fraction = 1.0f;
    while (index > 0) {
        fraction /= static_cast<f32>(base);
        result += fraction * static_cast<f32>(index % base);
        index /= base;
    }
    return result;
}

/// The sample offset of a frame in render pixels (-0.5 to 0.5): Halton bases 2 and 3.
[[nodiscard]] inline std::array<f32, 2> Jitter(u32 frame) {
    const u32 index = frame % JitterPhases + 1;
    return {Halton(index, 2) - 0.5f, Halton(index, 3) - 0.5f};
}

/// Normalized device coordinates to render pixels and device depth: the viewport (x, y, width,
/// height in render pixels; y_sign -1 when the device negates y before the viewport) and the
/// depth range (device depth = depth_offset + depth_scale * NDC z).
[[nodiscard]] inline Matrix ViewportMatrix(f32 x, f32 y, f32 width, f32 height, f32 y_sign,
                                           f32 depth_offset, f32 depth_scale) {
    const f64 half_width = width * 0.5;
    const f64 half_height = height * 0.5;
    return {
        half_width, 0.0, 0.0, x + half_width,
        0.0, y_sign * half_height, 0.0, y + half_height,
        0.0, 0.0, depth_scale, depth_offset,
        0.0, 0.0, 0.0, 1.0,
    };
}

[[nodiscard]] inline Matrix Multiply(const Matrix& a, const Matrix& b) {
    Matrix result{};
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            f64 sum = 0.0;
            for (size_t k = 0; k < 4; ++k) {
                sum += a[row * 4 + k] * b[k * 4 + column];
            }
            result[row * 4 + column] = sum;
        }
    }
    return result;
}

[[nodiscard]] inline std::optional<Matrix> Invert(Matrix m) {
    Matrix inverse{1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
    for (size_t column = 0; column < 4; ++column) {
        size_t pivot = column;
        for (size_t row = column + 1; row < 4; ++row) {
            if (std::abs(m[row * 4 + column]) > std::abs(m[pivot * 4 + column])) {
                pivot = row;
            }
        }
        if (std::abs(m[pivot * 4 + column]) < 1e-300) {
            return std::nullopt;
        }
        for (size_t k = 0; k < 4; ++k) {
            std::swap(m[column * 4 + k], m[pivot * 4 + k]);
            std::swap(inverse[column * 4 + k], inverse[pivot * 4 + k]);
        }
        const f64 scale = 1.0 / m[column * 4 + column];
        for (size_t k = 0; k < 4; ++k) {
            m[column * 4 + k] *= scale;
            inverse[column * 4 + k] *= scale;
        }
        for (size_t row = 0; row < 4; ++row) {
            const f64 factor = m[row * 4 + column];
            if (row == column || factor == 0.0) {
                continue;
            }
            for (size_t k = 0; k < 4; ++k) {
                m[row * 4 + k] -= factor * m[column * 4 + k];
                inverse[row * 4 + k] -= factor * inverse[column * 4 + k];
            }
        }
    }
    return inverse;
}

/// World to render pixels and device depth (homogeneous) for a camera's view-projection (rows).
[[nodiscard]] inline Matrix ScreenMatrix(const Matrix& viewport, const std::array<f32, 16>& camera) {
    Matrix view_projection;
    for (size_t i = 0; i < 16; ++i) {
        view_projection[i] = camera[i];
    }
    return Multiply(viewport, view_projection);
}

/// Takes (pixel x, pixel y, device depth, 1) of this frame to the same point's pixel in the
/// previous frame (homogeneous), as a column-major matrix for a shader.
[[nodiscard]] inline std::optional<std::array<f32, 16>> Reprojection(const Matrix& previous,
                                                                     const Matrix& current) {
    const std::optional<Matrix> inverse = Invert(current);
    if (!inverse) {
        return std::nullopt;
    }
    const Matrix reprojection = Multiply(previous, *inverse);
    std::array<f32, 16> columns;
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            const f64 value = reprojection[row * 4 + column];
            if (!std::isfinite(value)) {
                return std::nullopt;
            }
            columns[column * 4 + row] = static_cast<f32>(value);
        }
    }
    return columns;
}

} // namespace VideoCommon::OcarinaUpscale
