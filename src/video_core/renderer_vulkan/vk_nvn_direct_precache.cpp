// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef LICHII_NVN_DIRECT

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "common/cityhash.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_nvn_direct_shaders.h"
#include "video_core/shader_warm/nvn_emission_stamp.h"
#include "video_core/shader_warm/shader_warm_builder.h"
#include "video_core/shader_warm/warm_records.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {

namespace Warm = VideoCommon::ShaderWarm;

bool ReadCompute(std::istream& input, std::vector<u8>& code,
                 std::array<u8, Warm::NativeWarmMaxControl>& control, u32& control_size) {
    u32 code_size{};
    if (!input.read(reinterpret_cast<char*>(&code_size), sizeof(code_size))
             .read(reinterpret_cast<char*>(&control_size), sizeof(control_size)) ||
        code_size == 0 || code_size >= Warm::NativeWarmMaxCode ||
        control_size < Warm::NativeWarmMinControl || control_size > control.size()) {
        return false;
    }
    code.resize(code_size);
    return bool(input.read(reinterpret_cast<char*>(code.data()), code.size())
                    .read(reinterpret_cast<char*>(control.data()), control_size));
}

} // namespace

void NvnDirectShaders::InvalidatePrecacheIfCacheLost(u64 title_id) {
    if (title_id == 0 || nvn_direct_precache_cache_restored()) {
        return;
    }
    const auto file = Warm::NativeComputeWarmPath(title_id);
    std::ifstream input(file, std::ios::binary);
    if (Warm::ReadNativeWarmCount(input).value_or(0) != 0) {
        // An empty corpus needs no driver cache. Otherwise a cache lost before this boot
        // invalidates both markers, even if gameplay later creates a new cache without warming.
        std::error_code ec;
        std::filesystem::remove(Warm::WarmMarkerPath(file), ec);
        std::filesystem::remove(Warm::WarmProgressPath(file), ec);
    }
}

void NvnDirectShaders::LoadDiskResources(const Device& device, u64 title_id, std::stop_token stop,
                                         const VideoCore::DiskResourceLoadCallback& callback) {
    const bool asked = Warm::TakeWarmBeforeStart();
    if (title_id == 0 || !Settings::values.use_disk_shader_cache.GetValue() ||
        !Warm::PrecacheBeforeStart(Settings::values.precache_shaders.GetValue(), asked,
                                   Warm::WarmRequested(Warm::ComputeWarmPath(title_id)))) {
        return;
    }

    const auto file = Warm::NativeComputeWarmPath(title_id);
    std::ifstream input(file, std::ios::binary);
    const auto count = Warm::ReadNativeWarmCount(input);
    const auto uuid = device.GetPipelineCacheUUID();
    const auto& resolution = Settings::values.resolution_info;
    const std::array variant_inputs{
        Warm::EmissionStamp(),
        Warm::GeneratedNvnEmissionStamp,
        u64{resolution.up_scale},
        u64{resolution.down_shift},
        u64{Settings::values.disable_shader_loop_safety_checks.GetValue()},
        u64{Settings::values.renderer_debug.GetValue()},
    };
    const u64 variant = Common::CityHash64WithSeed(
        reinterpret_cast<const char*>(variant_inputs.data()), sizeof(variant_inputs),
        Common::CityHash64(reinterpret_cast<const char*>(uuid.data()), uuid.size()));
    const auto stamp =
        count ? Warm::CurrentWarmStamp(file, *count, device.GetDriverVersion(),
                                       device.GetDeviceID(), Warm::NativeWarmVersion, variant)
              : std::nullopt;
    const bool restored = nvn_direct_precache_cache_restored() != 0;
    const auto marker = Warm::ReadWarmMarker(file);
    const bool pending = stamp && (!marker || !stamp->Matches(*marker) || (*count && !restored));
    const u32 native_total = pending ? *count : 0;

    const auto report = [&](size_t done, size_t total) {
        if (callback && total) {
            callback(VideoCore::LoadCallbackStage::Warm, done, total);
        }
    };
    using namespace std::chrono_literals;
    u32 historical_done{}, historical_total{};
    for (;;) {
        if (stop.stop_requested() || Warm::TakeSkipWarm()) {
            return;
        }
        nvn_direct_prewarm_progress(&historical_done, &historical_total);
        if (historical_done >= historical_total) {
            break;
        }
        report(historical_done, size_t{historical_total} + native_total);
        std::this_thread::sleep_for(100ms);
    }
    if (!pending) {
        return;
    }
    if (native_total == 0) {
        if (input.peek() == std::char_traits<char>::eof()) {
            Warm::WriteWarmMarker(file, *stamp);
        }
        return;
    }

    auto built_before = restored ? Warm::ReadWarmProgress(file, *stamp) : std::unordered_set<u64>{};
    std::vector<u8> built(native_total);
    std::atomic<u32> done{}, active{};
    std::atomic<bool> cancel{}, invalid{};
    std::mutex read_mutex;
    u32 next{};
    const auto work = [&] {
        Common::SetCurrentThreadName("NVNPrecache");
        std::vector<u8> code;
        std::array<u8, Warm::NativeWarmMaxControl> control;
        u32 control_size{};
        while (!cancel.load() && !stop.stop_requested() && !invalid.load()) {
            u32 index;
            {
                std::scoped_lock lock{read_mutex};
                if (next == native_total || cancel.load() || invalid.load() ||
                    stop.stop_requested()) {
                    break;
                }
                index = next++;
                if (!ReadCompute(input, code, control, control_size)) {
                    invalid = true;
                    break;
                }
            }
            if (!built_before.contains(index)) {
                built[index] = nvn_direct_precache_compute(code.data(), code.size(), control.data(),
                                                           control_size) != 0;
            }
            ++done;
        }
    };
    const size_t workers =
        std::min({native_total, std::max(1u, std::thread::hardware_concurrency()), 8u});
    {
        std::vector<std::jthread> threads;
        threads.reserve(workers);
        report(historical_total, size_t{historical_total} + native_total);
        for (size_t i = 0; i < workers; ++i) {
            ++active;
            try {
                threads.emplace_back([&] {
                    try {
                        work();
                    } catch (const std::exception& e) {
                        LOG_WARNING(Render_NvnDirect, "Shader precache: {}", e.what());
                        invalid = true;
                    }
                    --active;
                });
            } catch (const std::system_error& e) {
                --active;
                LOG_WARNING(Render_NvnDirect, "Starting a shader precache worker failed: {}",
                            e.what());
                break;
            }
        }
        while (active.load() != 0) {
            if (stop.stop_requested() || Warm::TakeSkipWarm()) {
                cancel = true;
            }
            report(size_t{historical_total} + done, size_t{historical_total} + native_total);
            std::this_thread::sleep_for(100ms);
        }
    }
    if (invalid.load() || (done == native_total && input.peek() != std::char_traits<char>::eof())) {
        LOG_WARNING(Render_NvnDirect, "Shader precache: malformed native compute file");
        return;
    }
    const auto made = std::count(built.begin(), built.end(), u8{1});
    const bool complete = done == native_total;
    if (!stop.stop_requested() && (made || complete) && nvn_direct_save_precache()) {
        if (complete) {
            Warm::WriteWarmMarker(file, *stamp);
            std::error_code ec;
            std::filesystem::remove(Warm::WarmProgressPath(file), ec);
        } else {
            for (u32 i = 0; i < native_total; ++i) {
                if (built[i]) {
                    built_before.insert(i);
                }
            }
            Warm::WriteWarmProgress(file, *stamp, built_before);
        }
    }
    LOG_INFO(Render_NvnDirect,
             "Shader precache: {} of {} native compute programs tried, {} newly built", done.load(),
             native_total, made);
}

} // namespace Vulkan

#endif
