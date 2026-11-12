// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>

namespace VideoCommon::BatchedHandles {
using U32 = std::uint32_t;
using U64 = std::uint64_t;

inline bool Enabled() {
    static const bool value = [] {
        const char* text = std::getenv("LICHII_BATCHED_HANDLES");
        return text && std::strcmp(text, "1") == 0;
    }();
    return value;
}
inline bool Validate() {
    static const bool value = [] {
        const char* text = std::getenv("LICHII_BATCHED_HANDLES_VALIDATE");
        return text && std::strcmp(text, "1") == 0;
    }();
    return value;
}
inline std::atomic<bool>& FailedValidation() {
    static std::atomic<bool> failed{};
    return failed;
}

// One array's bounded snapshot. This assumes draw inputs are stable while its
// handles resolve; validation checks each copied word before it is consumed.
template <class Memory>
class Cursor {
public:
    template <class Descriptor, class Cbufs>
    Cursor(const Memory& memory, const Descriptor& descriptor, const Cbufs& cbufs,
           bool enabled = Enabled(), bool validation = Validate())
        : memory{memory}, count{descriptor.count} {
        if (!enabled || FailedValidation().load(std::memory_order_relaxed) || count < 64 ||
            descriptor.handle_program != ~U32{0} || descriptor.has_secondary ||
            descriptor.size_shift < 2 || descriptor.size_shift > 4 ||
            !memory.CanBatchHandleWords()) {
            return;
        }
        if (descriptor.cbuf_index >= cbufs.size() || !cbufs[descriptor.cbuf_index].enabled) return;
        stride_words = U32{1} << (descriptor.size_shift - 2);
        const U64 last_offset = U64{descriptor.cbuf_offset} + U64{count - 1} * stride_words * sizeof(U32);
        const U64 cbuf_address = cbufs[descriptor.cbuf_index].address;
        if (last_offset > (std::numeric_limits<U32>::max)() ||
            cbuf_address > (std::numeric_limits<U64>::max)() - last_offset - (sizeof(U32) - 1)) return;
        address = cbuf_address + descriptor.cbuf_offset;
        if (address & (sizeof(U32) - 1)) return;
        active = true;
        validate = validation;
    }

    std::optional<U32> Read(U32 index) {
        if (!active || index >= count) return std::nullopt;
        if (index < first || index - first >= stored) {
            if (FailedValidation().load(std::memory_order_relaxed)) {
                active = false;
                return std::nullopt;
            }
            const U64 begin = address + U64{index} * stride_words * sizeof(U32);
            const size_t page_handles = (4096 - (begin & 4095) - sizeof(U32)) /
                                            (stride_words * sizeof(U32)) + 1;
            const size_t handles = (std::min)({size_t{64}, size_t{count - index}, page_handles});
            const size_t words = (handles - 1) * stride_words + 1;
            if (!memory.TryReadHandleWords(begin, std::span<U32>{snapshot.data(), words})) {
                stored = 0;
                return std::nullopt;
            }
            first = index;
            stored = static_cast<U32>(handles);
        }
        const U32 value = snapshot[(index - first) * stride_words];
        if (validate) {
            const U32 reference = memory.template Read<U32>(address + U64{index} * stride_words * sizeof(U32));
            if (value != reference) {
                FailedValidation().store(true, std::memory_order_relaxed);
                active = false;
                failed_validation = true;
                stored = 0;
                return reference;
            }
        }
        return value;
    }

    bool ValidationFailed() const { return failed_validation; }

private:
    const Memory& memory;
    U64 address{};
    U32 count{}, first{}, stored{}, stride_words{1};
    bool active{}, validate{}, failed_validation{};
    std::array<U32, 256> snapshot;
};
} // namespace VideoCommon::BatchedHandles
