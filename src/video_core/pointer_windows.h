// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <span>

#include "common/alignment.h"
#include "common/common_types.h"
#include "shader_recompiler/shader_info.h"

namespace VideoCommon {

/// GPU memory bound as a storage buffer for global memory reached through loaded pointers
struct PointerWindowRange {
    GPUVAddr gpu_addr{};
    u32 size{};

    auto operator<=>(const PointerWindowRange&) const = default;
};

using PointerWindows = std::array<PointerWindowRange, Shader::NUM_POINTER_WINDOWS>;

/// Bytes assumed reachable past each pointer (the fields of the record it points to)
constexpr u64 POINTER_WINDOW_ACCESS_SIZE = 0x100;
constexpr u64 MAX_POINTER_WINDOW_SIZE = 4ULL << 20;

/// Groups the memory that sorted, unique pointers point to into as few windows as fit: tight
/// windows first, bridging gaps of 64 KiB, then 1 MiB, then 4 MiB only when the pointers do not
/// fit otherwise. A window covers one device-contiguous range; contiguous(begin, end) tells
/// whether [begin, end) is one. Returns false when some pointers are left out.
template <typename Contiguous>
bool GroupPointerWindows(std::span<const GPUVAddr> pointers, Contiguous&& contiguous,
                         PointerWindows& windows) {
    static constexpr std::array<u64, 3> MAX_GAPS{0x10000, 1ULL << 20, MAX_POINTER_WINDOW_SIZE};
    constexpr u64 page_size = 0x1000;
    for (const u64 max_gap : MAX_GAPS) {
        windows = {};
        size_t num_windows{};
        bool fits{true};
        for (const GPUVAddr pointer : pointers) {
            const GPUVAddr access_end{pointer + POINTER_WINDOW_ACCESS_SIZE};
            if (num_windows > 0) {
                PointerWindowRange& window{windows[num_windows - 1]};
                const GPUVAddr window_end{window.gpu_addr + window.size};
                if (pointer <= window_end + max_gap &&
                    access_end - window.gpu_addr <= MAX_POINTER_WINDOW_SIZE &&
                    contiguous(window_end - 1, access_end)) {
                    window.size =
                        static_cast<u32>((std::max)(window_end, access_end) - window.gpu_addr);
                    continue;
                }
            }
            if (num_windows == windows.size()) {
                fits = false;
                break;
            }
            GPUVAddr window_end{access_end};
            if (!contiguous(pointer, window_end)) {
                window_end = Common::AlignUp(pointer + 1, page_size);
            }
            windows[num_windows++] = PointerWindowRange{
                .gpu_addr = pointer,
                .size = static_cast<u32>(window_end - pointer),
            };
        }
        if (fits) {
            return true;
        }
    }
    return false;
}

} // namespace VideoCommon
