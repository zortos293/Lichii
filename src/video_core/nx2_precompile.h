// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// nx2-precompile (tools/nx2-precompile/README.md): finds a game's shader programs in its files,
// predicts the pipelines they are drawn with and writes the game's precompiled file, which
// PipelineCache::PreparePrecompiled builds before the pipelines are first drawn. The tool's
// commands run here, and so does Build, the whole pipeline in the emulator from the player's
// own recorded play.

#include <atomic>
#include <filesystem>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace FileSys::Nx2ShaderExtract {
struct Result;
}

namespace VideoCommon::Nx2Precompile {

/// The nx2-precompile command line.
int RunTool(int argc, char** argv);

enum class BuildStep : u32 {
    Reading,  ///< the recorded shader caches
    Scanning, ///< every program, lowered and translated against the fact oracle
    Mesh,     ///< the mesh draws' environments
    Writing,  ///< the predicted pipelines and the file
};

struct BuildProgress {
    std::atomic<BuildStep> step{BuildStep::Reading};
    std::atomic<u64> done{};
    std::atomic<u64> total{};
};

struct BuildReport {
    u64 recorded_pipelines{}; ///< graphics and compute records in the caches
    u64 programs{};           ///< extracted
    u64 records{};            ///< in the precompiled file
    u64 environments{};
    std::vector<std::string> lines; ///< what each step found, for the log
};

/// Writes the precompiled file (NX2PREP1, `cache_version`) of a game to `out` from its extracted
/// programs and the shader caches recorded while playing it (shader cache v2 logs,
/// lichii-cache.v2.bin, or pipeline cache files of the format before them), as run_pipeline.sh
/// does with nx2-precompile and its Python scripts, and gives the same file. Returns false when
/// stopped. Throws std::exception on a failure.
bool Build(const FileSys::Nx2ShaderExtract::Result& extracted,
           std::span<const std::filesystem::path> caches, const std::filesystem::path& out,
           u32 cache_version, unsigned threads, BuildProgress* progress, std::stop_token stop,
           BuildReport& report);

/// The graphics and compute pipelines a shader cache v2 log records (live), 0 without one.
u64 CountRecordedPipelines(const std::filesystem::path& cache);

} // namespace VideoCommon::Nx2Precompile
