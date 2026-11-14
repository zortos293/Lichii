// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "video_core/texture_cache/types.h"
#include "video_core/textures/texture.h"

namespace VideoCommon {

/// What selects a draw's sampled textures besides its handle words: the pipeline, the address
/// space and channel they are read in, the texture and sampler tables and how a handle splits
/// into their indices.
struct DrawTemplateContext {
    u64 owner;       ///< DrawTemplateTable::NewOwner of the pipeline
    u64 memory;      ///< The memory manager
    u64 image_table; ///< The channel's graphics image table
    u64 tic_address;
    u64 tsc_address;
    u32 tic_limit;
    u32 tsc_limit;
    u32 via_header_index;
    u32 native_tiling;
};
static_assert(std::has_unique_object_representations_v<DrawTemplateContext>);

/// One sampled texture of a draw template: the descriptors it was resolved from, read through
/// host pointers that hold while MemoryManager::HostPointerSerial does, and what they gave.
struct DrawTemplateTexture {
    const u8* tic_host{};
    const u8* tsc_host{};
    Tegra::Texture::TICEntry tic{};
    Tegra::Texture::TSCEntry tsc{};
    ImageViewId view{};
    u32 view_generation{}; ///< TextureCache::ImageViewGeneration of view
    SamplerId sampler{};
};

/// Why a template the key found was not used.
enum class DrawTemplateCheck : u8 { Hit, HostMemory, Scale, ViewDeleted, Descriptors, Count };

[[nodiscard]] inline u64 HashDrawTemplate(const DrawTemplateContext& context,
                                          std::span<const u32> handles) noexcept {
    std::array<u64, sizeof(DrawTemplateContext) / sizeof(u64)> words;
    static_assert(sizeof(words) == sizeof(DrawTemplateContext));
    std::memcpy(words.data(), &context, sizeof(context));
    u64 hash = 0x9e3779b97f4a7c15ULL;
    const auto mix = [&hash](u64 value) {
        hash = (hash ^ value) * 0xff51afd7ed558ccdULL;
        hash ^= hash >> 32;
    };
    for (const u64 word : words) {
        mix(word);
    }
    for (const u32 handle : handles) {
        mix(handle);
    }
    return hash;
}

/// A draw's sampled textures and the image descriptors they gave, kept across frames. A later
/// draw of the same pipeline with the same context and handle words takes them instead of
/// resolving its views and samplers and pushing their descriptors, while Check proves that
/// every byte and object they came from is the same.
template <typename Descriptor, size_t NumStages, size_t RescalingWords>
struct DrawTemplate {
    DrawTemplateContext context{};
    std::vector<u32> handles;
    u64 hash{};
    u64 last_use{}; ///< 0 while the slot is empty
    u64 host_serial{};
    u64 scale_epoch{};
    std::vector<DrawTemplateTexture> textures;
    std::vector<Descriptor> descriptors;
    std::array<std::pair<u32, u32>, NumStages> ranges{}; ///< First descriptor and count per stage
    std::array<u32, RescalingWords> rescaling{};

    [[nodiscard]] bool Matches(const DrawTemplateContext& other, std::span<const u32> other_handles,
                               u64 other_hash) const noexcept {
        return last_use != 0 && hash == other_hash && handles.size() == other_handles.size() &&
               std::memcmp(&context, &other, sizeof(context)) == 0 &&
               std::equal(handles.begin(), handles.end(), other_handles.begin());
    }

    /// `generation(view)` is the view's TextureCache::ImageViewGeneration now.
    template <typename Generation>
    [[nodiscard]] DrawTemplateCheck Check(u64 current_host_serial, u64 current_scale_epoch,
                                          Generation&& generation) const noexcept {
        if (host_serial != current_host_serial) {
            return DrawTemplateCheck::HostMemory;
        }
        if (scale_epoch != current_scale_epoch) {
            return DrawTemplateCheck::Scale;
        }
        for (const DrawTemplateTexture& texture : textures) {
            if (generation(texture.view) != texture.view_generation) {
                return DrawTemplateCheck::ViewDeleted;
            }
            if (std::memcmp(texture.tic_host, &texture.tic, sizeof(texture.tic)) != 0 ||
                std::memcmp(texture.tsc_host, &texture.tsc, sizeof(texture.tsc)) != 0) {
                return DrawTemplateCheck::Descriptors;
            }
        }
        return DrawTemplateCheck::Hit;
    }
};

/// Which of a pipeline's draws keep a template: every one while its templates are found, and one
/// in 16 once 16 draws in a row found none, so a pipeline whose draws never repeat stops paying.
class DrawTemplateRecorder {
public:
    /// Notes whether a draw's lookup found a valid template; returns whether to keep its own.
    [[nodiscard]] bool Note(bool found) noexcept {
        misses = found ? 0 : misses + 1;
        return misses <= 16 || (misses & 15) == 0;
    }

private:
    u32 misses{};
};

/// Every pipeline's draw templates in a fixed number of slots, WAYS per set; a new template
/// replaces the set's least recently used one.
template <typename Template>
class DrawTemplateTable {
public:
    static constexpr size_t WAYS = 4;

    /// `num_sets` is a power of two.
    explicit DrawTemplateTable(size_t num_sets) noexcept : sets{num_sets} {}

    /// A key part no other pipeline has, deleted ones included.
    [[nodiscard]] u64 NewOwner() noexcept {
        return owners.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    [[nodiscard]] Template* Find(const DrawTemplateContext& context, std::span<const u32> handles,
                                 u64 hash) noexcept {
        if (slots.empty()) {
            return nullptr;
        }
        Template* const set = &slots[(hash & (sets - 1)) * WAYS];
        for (size_t way = 0; way < WAYS; ++way) {
            if (set[way].Matches(context, handles, hash)) {
                set[way].last_use = ++uses;
                return &set[way];
            }
        }
        return nullptr;
    }

    /// The slot to keep this key's template in: its own, else the set's least recently used.
    /// Members other than the key are left as they were.
    [[nodiscard]] Template& Insert(const DrawTemplateContext& context, std::span<const u32> handles,
                                   u64 hash) {
        if (slots.empty()) {
            slots.resize(sets * WAYS);
        }
        Template* const set = &slots[(hash & (sets - 1)) * WAYS];
        Template* slot = set;
        for (size_t way = 0; way < WAYS; ++way) {
            if (set[way].Matches(context, handles, hash)) {
                slot = &set[way];
                break;
            }
            if (set[way].last_use < slot->last_use) {
                slot = &set[way];
            }
        }
        slot->context = context;
        slot->handles.assign(handles.begin(), handles.end());
        slot->hash = hash;
        slot->last_use = ++uses;
        return *slot;
    }

    static void Erase(Template& slot) noexcept {
        slot.last_use = 0;
    }

    [[nodiscard]] size_t Capacity() const noexcept {
        return sets * WAYS;
    }

private:
    size_t sets;
    std::vector<Template> slots;
    u64 uses{};
    std::atomic<u64> owners{};
};

} // namespace VideoCommon
