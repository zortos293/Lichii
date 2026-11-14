// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The steps of tools/nx2-precompile/run_pipeline.sh that its Python scripts did
// (tools/nx2-precompile/py: learn_priors.py, game_tables.py, mesh_draws.py and
// build_precompiled.py with state_model.py and statefields.py), so the emulator can build a
// game's precompiled file from the player's own play without Python. Each step gives the same
// result as its script, and the precompiled file is the script's byte for byte.

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon::Nx2Precompile {

/// A shader environment of a pipeline cache file (GenericEnvironment::Serialize layout), its maps
/// in file order (lichii_cache.py Env).
struct CachedEnv {
    u64 code_size{};
    u32 local_memory_size{};
    u32 texture_bound{};
    u32 start_address{};
    u32 cached_lowest{};
    u32 cached_highest{};
    u32 viewport_transform_state{};
    u32 stage{};
    std::vector<u8> code;
    std::vector<std::pair<u32, u32>> texture_types;
    std::vector<std::pair<u32, s32>> texture_pixel_formats;
    std::vector<std::pair<u64, u32>> cbuf_values;
    std::vector<std::pair<u64, u8>> storage_pointer_types;
    std::vector<std::pair<u64, u32>> cbuf_replacements;
    std::array<u32, 3> workgroup_size{};
    u32 shared_memory_size{};
    std::vector<u8> sph; ///< The program header, for every stage but compute
    std::array<u32, 8> gp_passthrough_mask{};
    bool has_mesh_fields{};
    std::array<u32, 3> mesh_registers{};
    std::vector<u8> mesh_primitive_header;

    /// GenericEnvironment::Serialize's bytes, with the mesh fields of task and mesh programs.
    [[nodiscard]] std::vector<u8> Serialize() const;
};

/// Reads one environment at `offset` (mesh fields for task and mesh programs, as caches from
/// version 53 on hold); returns the offset past it. Throws std::runtime_error on damage.
size_t ParseCachedEnv(std::span<const u8> bytes, size_t offset, CachedEnv& env);

struct CachedPipeline {
    std::vector<CachedEnv> envs;
    std::vector<u8> key; ///< 24 bytes for compute, 1056 for graphics

    [[nodiscard]] bool IsCompute() const {
        return envs.front().stage == 5;
    }
};

/// The records of a pipeline cache file ("yuzucach", version, records). Throws on damage.
std::vector<CachedPipeline> ParseCacheFile(std::span<const u8> bytes);

/// GenericEnvironment's native shader hash of lowered code.
u64 NativeShaderHash(std::span<const u8> code);

/// GraphicsPipelineCacheKey::Hash of a 1056-byte key.
u64 GraphicsKeyHash(std::span<const u8> key);

/// learn_priors.py: the storage pointer answer of each (stage, bank, offset[, table offset]) the
/// recorded programs answered other than "mapped" by majority.
struct LearnedPrior {
    u32 stage{};
    bool indirect{};
    u32 bank{};
    u32 offset{};
    u32 table{};
    u8 answer{};
};
std::vector<LearnedPrior> LearnPriors(std::span<const CachedPipeline> records);

/// One extracted program (nx2-precompile extract), in extract order.
struct ExtractedProgram {
    u64 fingerprint{};
    std::string origin;
};

/// One scanned program (nx2-precompile scan), in the same order.
struct ScannedProgram {
    std::string stage;  ///< "VertexB", "Fragment", ...
    std::string status; ///< "ok", "mesh-hash-only", "capture", "translate"
    u64 code_hash{};    ///< 0 when it did not lower
};

/// A variation: the extract's label and its programs' indices.
struct ExtractedVariation {
    std::string label;
    std::vector<u32> programs;
};

/// game_tables.py's inventory row.
struct InventoryRow {
    std::string stage;
    u64 code_hash{};
    std::string fine;   ///< origin without its variation
    std::string coarse; ///< the file
    bool left_out{};    ///< only in a shader archive version the game does not load
};

struct MeshCombo {
    std::optional<u64> task;
    u64 mesh{};
    u64 pixel{};
};

struct GameTables {
    std::vector<InventoryRow> rows;
    std::vector<std::pair<u64, u64>> pairs; ///< (vertex, pixel), sorted
    std::vector<MeshCombo> combos;          ///< sorted
    std::vector<std::string> left_out_files;
};

/// game_tables.py. `used` are the recorded caches of the game.
GameTables MakeGameTables(std::span<const ExtractedProgram> extracted,
                          std::span<const ExtractedVariation> variations,
                          std::span<const ScannedProgram> scanned,
                          std::span<const CachedPipeline> used);

/// mesh_draws.py's draw: a task program (none) and a mesh program by shader hash, and INIT's
/// `lines`. `controls[ordinal]` is each extracted program's control section.
struct MeshDraw {
    std::optional<u64> task;
    u64 mesh{};
    u32 lines{};
};
std::vector<MeshDraw> MakeMeshDraws(const GameTables& tables,
                                    std::span<const std::span<const u8>> controls);

/// nx2-precompile mesh-envs' output for one draw.
struct MeshEnvironments {
    u64 state_hash{};
    u64 task{};
    u64 mesh{};
    std::vector<u8> task_env; ///< empty without a task program
    std::vector<u8> mesh_env;
};

struct PrecompiledOptions {
    u32 k = 4;      ///< predicted render states per vertex/pixel pair
    u32 mesh_k = 2; ///< per mesh draw
    u32 cache_version{};
};

/// build_precompiled.py's counts, in the order it reports them.
using PrecompiledCounts = std::vector<std::pair<std::string, u64>>;

/// build_precompiled.py: writes the precompiled file (NX2PREP1) to `out`, through `out`.partial.
/// `oracle` holds each lowered code's first oracle environment (nx2-precompile scan), by shader
/// hash. Returns the counts. Throws std::runtime_error.
PrecompiledCounts WritePrecompiled(const std::filesystem::path& out, const GameTables& tables,
                                   const std::map<u64, std::vector<u8>>& oracle,
                                   std::span<const CachedPipeline> train,
                                   std::span<const MeshEnvironments> mesh_envs,
                                   const PrecompiledOptions& options);

} // namespace VideoCommon::Nx2Precompile
