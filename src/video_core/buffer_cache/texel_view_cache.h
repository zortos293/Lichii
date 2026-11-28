// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon {

/// The texel buffer views of one buffer, by offset, size and format. Games stream per-draw data
/// through texel buffers at offsets that move every frame, so keeping every view until the buffer
/// dies holds one for each offset ever bound. NVIDIA's driver keeps buffer and image views in one
/// fixed table and fails every later view creation with VK_ERROR_OUT_OF_DEVICE_MEMORY once that
/// fills (Yoshi and the Mysterious Book, ZOR-611). Each time the cache reaches its sweep size, it
/// drops the views whose last use the GPU has finished, then sets the sweep size to twice what is
/// left.
template <typename Handle>
class TexelViewCache {
public:
    static constexpr size_t MIN_SWEEP_SIZE = 64;

    /// The view for this key with its last use moved to `stamp`, or nullptr.
    [[nodiscard]] Handle* Find(u64 offset, u64 size, u32 format, u64 stamp) {
        const auto it = std::ranges::find_if(views, [&](const View& view) {
            return view.offset == offset && view.size == size && view.format == format;
        });
        if (it == views.end()) {
            return nullptr;
        }
        it->last_use = stamp;
        return &it->handle;
    }

    /// Adds a view last used at `stamp`. is_free(stamp) tells whether the GPU finished the work
    /// recorded at a stamp.
    template <typename IsFree>
    Handle& Insert(u64 offset, u64 size, u32 format, u64 stamp, Handle&& handle, IsFree&& is_free) {
        if (views.size() >= sweep_size) {
            std::erase_if(views, [&](const View& view) { return is_free(view.last_use); });
            sweep_size = (std::max)(MIN_SWEEP_SIZE, views.size() * 2);
        }
        views.push_back({offset, size, format, stamp, std::move(handle)});
        return views.back().handle;
    }

    [[nodiscard]] size_t Size() const noexcept {
        return views.size();
    }

private:
    struct View {
        u64 offset;
        u64 size;
        u32 format;
        u64 last_use;
        Handle handle;
    };

    std::vector<View> views;
    size_t sweep_size{MIN_SWEEP_SIZE};
};

} // namespace VideoCommon
