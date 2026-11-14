// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <mutex>
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/neural_rendering/model.h"
#include "video_core/neural_rendering/runtime.h"

namespace VideoCore::NeuralRendering {
namespace {
std::mutex status_mutex;
RuntimeStatus status;
std::atomic<std::uint64_t> frames{};
} // namespace

std::filesystem::path UserFolder() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "ngx" / "nr" / ModelVersion;
}

RuntimeStatus GetStatus() {
    std::scoped_lock lock{status_mutex};
    auto result = status;
    result.submitted_frames = frames.load(std::memory_order_relaxed);
    return result;
}

void ResetStatus() {
    std::scoped_lock lock{status_mutex};
    status = {};
    frames = 0;
}

void ReportReady() {
    std::scoped_lock lock{status_mutex};
    status.ready = true;
    status.error.clear();
}

/// Clear readiness and runtime failures when the user turns neural rendering off.
void ReportInactive() {
    std::scoped_lock lock{status_mutex};
    status.ready = false;
    status.error.clear();
}

void ReportFailure(std::string error) {
    LOG_WARNING(Render_Vulkan, "Neural rendering: {}; continuing with NVIDIA DLSS", error);
    std::scoped_lock lock{status_mutex};
    status.ready = false;
    status.error = std::move(error);
}

void ReportFrame() {
    ++frames;
}
} // namespace VideoCore::NeuralRendering
