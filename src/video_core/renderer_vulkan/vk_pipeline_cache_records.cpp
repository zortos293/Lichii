// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <fstream>
#include <numeric>

#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache_records.h"

namespace Vulkan {
namespace {

/// Prefer candidates that keep the first record's pointers, then those that add the most.
void OrderComputeTranslations(std::vector<CachedComputeTranslation>& candidates) {
    if (candidates.size() < 2) {
        return;
    }
    std::vector<std::vector<u64>> keys;
    keys.reserve(candidates.size());
    for (const CachedComputeTranslation& candidate : candidates) {
        keys.push_back(candidate.env.MappedStoragePointerKeys());
    }
    std::vector<size_t> order(candidates.size());
    std::iota(order.begin(), order.end(), size_t{0});
    const auto keeps_first = [&](size_t index) {
        return std::ranges::includes(keys[index], keys.front());
    };
    std::ranges::stable_sort(order, [&](size_t lhs, size_t rhs) {
        const bool lhs_keeps{keeps_first(lhs)};
        if (lhs_keeps != keeps_first(rhs)) {
            return lhs_keeps;
        }
        return lhs_keeps && keys[lhs].size() > keys[rhs].size();
    });
    std::vector<CachedComputeTranslation> ordered;
    ordered.reserve(candidates.size());
    for (const size_t index : order) {
        ordered.push_back(std::move(candidates[index]));
    }
    candidates = std::move(ordered);
}

} // Anonymous namespace

std::unordered_map<u64, u8> LoadUsedPipelines(const std::filesystem::path& path, u8 max_age) {
    std::unordered_map<u64, u8> used;
    std::ifstream file{path, std::ios::binary};
    u32 magic{}, count{};
    if (!file.read(reinterpret_cast<char*>(&magic), sizeof(magic)) || magic != UsedPipelinesMagic ||
        !file.read(reinterpret_cast<char*>(&count), sizeof(count))) {
        return used;
    }
    used.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        u64 hash{};
        u8 age{};
        if (!file.read(reinterpret_cast<char*>(&hash), sizeof(hash)) ||
            !file.read(reinterpret_cast<char*>(&age), sizeof(age))) {
            break;
        }
        if (age < max_age) { // lists saved with a longer window
            used.emplace(hash, age);
        }
    }
    return used;
}

CachedComputeTranslation ReadCachedComputeTranslation(std::span<const u8> payload,
                                                      VideoCommon::PipelineRecord record,
                                                      const ComputePipelineCacheKey& key) {
    using VideoCommon::FileEnvironment;
    CachedComputeTranslation candidate{.env = {}, .record = record};
    Common::UniqueFunction<void, std::istream&, FileEnvironment> read_compute{
        [&](std::istream& file, FileEnvironment env) {
            ComputePipelineCacheKey read_key;
            file.read(reinterpret_cast<char*>(&read_key), sizeof(read_key));
            if (read_key != key) {
                throw std::ios_base::failure("Compute pipeline record has another key");
            }
            VideoCommon::CheckComputeRecord(env, key.shared_memory_size, key.workgroup_size);
            candidate.env = std::move(env);
        }};
    Common::UniqueFunction<void, std::istream&, std::vector<FileEnvironment>> read_graphics{
        [](std::istream&, std::vector<FileEnvironment>) {
            throw std::ios_base::failure("Graphics pipeline in a compute record");
        }};
    VideoCommon::ReadPipelinePayload(payload, read_compute, read_graphics);
    return candidate;
}

CachedComputeLoad RestoreCachedComputeLayouts(
    const ComputePipelineCacheKey& key, std::vector<CachedComputeTranslation> candidates,
    u32 read_properties, const TranslateCachedCompute& translate) {
    CachedComputeLoad load;
    const auto try_layout = [&](std::vector<CachedComputeTranslation>& translations,
                                CachedComputeScope scope) {
        OrderComputeTranslations(translations);
        size_t chosen = 0;
        auto result = CachedComputeResult::Failed;
        for (; chosen < translations.size(); ++chosen) {
            auto& env = translations[chosen].env;
            result = translate(RecordLookupKey(key, env), env, scope);
            if (result != CachedComputeResult::Failed) {
                break;
            }
        }
        if (scope != CachedComputeScope::AnyLayoutOnly || result != CachedComputeResult::ExactLayout) {
            ++load.layouts;
            AddUnchosenComputeTranslations(translations, chosen, read_properties, load.incomplete,
                                           load.copies);
        }
        return result;
    };

    if (candidates.empty()) {
        return load;
    }
    const auto first_layout = RecordLookupKey(key, candidates.front().env);
    if (std::ranges::all_of(candidates, [&](const auto& candidate) {
            return RecordLookupKey(key, candidate.env) == first_layout;
        })) {
        try_layout(candidates, CachedComputeScope::EitherLayout);
        return load;
    }
    // A shared pipeline keeps the original pointer preference across all its records.
    if (try_layout(candidates, CachedComputeScope::AnyLayoutOnly) != CachedComputeResult::ExactLayout) {
        return load;
    }
    std::ranges::sort(candidates, {}, [](const auto& candidate) { return candidate.record.begin; });

    std::unordered_map<ComputePipelineLookupKey, size_t> group_indices;
    std::vector<std::vector<CachedComputeTranslation>> groups;
    for (auto& candidate : candidates) {
        const auto layout = RecordLookupKey(key, candidate.env);
        const auto [it, inserted] = group_indices.try_emplace(layout, groups.size());
        if (inserted) {
            groups.emplace_back();
        }
        groups[it->second].push_back(std::move(candidate));
    }
    for (auto& translations : groups) {
        if (try_layout(translations, CachedComputeScope::ExactLayoutOnly) ==
            CachedComputeResult::Cancelled) {
            break;
        }
    }
    return load;
}

void AddUnchosenComputeTranslations(std::span<const CachedComputeTranslation> candidates,
                                    size_t chosen, u32 read_properties,
                                    std::vector<VideoCommon::PipelineRecord>& incomplete,
                                    std::vector<VideoCommon::PipelineRecord>& copies) {
    if (candidates.empty()) {
        return;
    }
    std::vector<bool> removed(candidates.size());
    for (size_t index = 0; index < chosen && index < candidates.size(); ++index) {
        removed[index] = candidates[index].env.LacksRecordedValue(read_properties);
    }
    std::vector<size_t> file_order(candidates.size());
    std::iota(file_order.begin(), file_order.end(), size_t{0});
    std::ranges::sort(file_order, {}, [&](size_t index) { return candidates[index].record.begin; });
    const size_t first{file_order.front()};
    const auto next{
        std::ranges::find(file_order, false, [&](size_t index) -> bool { return removed[index]; })};
    if (removed[first] && next != file_order.end() &&
        candidates[*next].env.MappedStoragePointerKeys() !=
            candidates[first].env.MappedStoragePointerKeys()) {
        removed[first] = false;
    }
    for (size_t index = 0; index < candidates.size(); ++index) {
        if (removed[index]) {
            incomplete.push_back(candidates[index].record);
        }
    }
    // A copy has the same bytes as an earlier record kept, so they bind the same pointers and the
    // order tries the earlier one first. RemovePipelines checks the bytes again.
    for (auto later = file_order.begin(); later != file_order.end(); ++later) {
        const VideoCommon::PipelineRecord& record{candidates[*later].record};
        if (removed[*later] || *later == chosen) {
            continue;
        }
        const auto original{std::find_if(file_order.begin(), later, [&](size_t index) {
            const VideoCommon::PipelineRecord& kept{candidates[index].record};
            return !removed[index] && kept.hash == record.hash &&
                   kept.end - kept.begin == record.end - record.begin;
        })};
        if (original != later) {
            removed[*later] = true;
            VideoCommon::PipelineRecord removed_copy{record};
            removed_copy.copy_of = candidates[*original].record.begin;
            copies.push_back(removed_copy);
        }
    }
}

void AddEqualGraphicsKeys(const std::filesystem::path& filename,
                          std::span<const DuplicateGraphicsRecord> duplicates,
                          std::vector<VideoCommon::PipelineRecord>& records) {
    std::ifstream file{filename, std::ios::binary};
    const auto read_key{[&file](u64 offset) {
        GraphicsPipelineCacheKey key{};
        file.seekg(static_cast<std::streamoff>(offset));
        file.read(reinterpret_cast<char*>(&key), sizeof(key));
        return key;
    }};
    for (const auto& [record, first_key] : duplicates) {
        const GraphicsPipelineCacheKey key{read_key(record.end - sizeof(GraphicsPipelineCacheKey))};
        if (key == read_key(first_key) && file) {
            records.push_back(record);
        }
    }
}

void AddIncompleteGraphics(std::span<const std::pair<u64, VideoCommon::PipelineRecord>> incomplete,
                           std::vector<DuplicateGraphicsRecord>& duplicates,
                           std::unordered_set<u64>& disk_pipelines,
                           std::vector<VideoCommon::PipelineRecord>& removed) {
    for (const auto& [hash, record] : incomplete) {
        const size_t later_records{
            std::erase_if(duplicates, [&](const DuplicateGraphicsRecord& duplicate) {
                return duplicate.first_key == record.end - sizeof(GraphicsPipelineCacheKey);
            })};
        if (later_records == 0) {
            disk_pipelines.erase(hash);
        }
        removed.push_back(record);
    }
}

} // namespace Vulkan
