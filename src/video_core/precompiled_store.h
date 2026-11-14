// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon {

/// What the preparation of a game's precompiled pipelines (precompiled.fork-v<N>.nx2prep) kept:
/// each prepared pipeline's shader infos and SPIR-V, so a draw of it skips translation. It is
/// written while the preparation runs and read back by later launches, so the preparation can
/// stop at any point (skipped, the game closed, a crash) and a later launch continues it.
///
/// Two files: the data file holds the blobs (an info, or one zstd frame of SPIR-V words); the
/// index file is an append-only list of entries: blobs, prepared records and the records a run
/// started. A record's entry is written only after the data it names is flushed, and a torn tail
/// is dropped, so an interrupted run leaves files a later launch reads. Both start with the stamp
/// of the run (the precompiled file, driver, build and shader settings): another stamp starts
/// them again.
class PrecompiledStore {
public:
    static constexpr u32 NoBlob = ~0U;
    static constexpr size_t MaxStages = 5;

    enum class BlobKind : u8 {
        Info = 0,
        Spirv = 1,
    };

    struct Blob {
        u64 offset{};
        u32 size{};
        BlobKind kind{};
        /// CityHash64 of the info's bytes, or of the SPIR-V's words before compression
        u64 hash{};
    };

    struct Record {
        u32 index{};    ///< The record's position in the precompiled file
        u64 key_hash{}; ///< Its pipeline key's hash
        bool compute{};
        /// Prepared without infos to keep: its build failed, or it is not drawn natively
        bool failed{};
        u8 env_count{};
        std::array<u32, MaxStages> envs{NoBlob, NoBlob, NoBlob, NoBlob, NoBlob};
        std::array<u32, MaxStages> infos{NoBlob, NoBlob, NoBlob, NoBlob, NoBlob};
        std::array<u32, MaxStages> spirv{NoBlob, NoBlob, NoBlob, NoBlob, NoBlob};
    };

    enum class RecordState : u8 {
        Pending,  ///< Not prepared yet
        Prepared, ///< Prepared; its infos are kept, or it has none to keep
        Poisoned, ///< Started by two runs that never finished it: never prepared again
    };

    struct Loaded {
        std::vector<Blob> blobs;     ///< By blob id
        std::vector<Record> records; ///< Prepared records, in the order they finished
        std::vector<RecordState> states; ///< By record index
        size_t poisoned{};           ///< Records found poisoned by this open
        size_t interrupted{};        ///< Records an earlier run started and never finished
        bool resumed{};              ///< The files held this stamp's earlier work
    };

    /// A run that started a record this many times without finishing it gives up on it: each
    /// start was in a run that ended without finishing it or cancelling it (a crash).
    static constexpr u32 PoisonStarts = 2;

    PrecompiledStore() = default;
    ~PrecompiledStore();

    PrecompiledStore(const PrecompiledStore&) = delete;
    PrecompiledStore& operator=(const PrecompiledStore&) = delete;

    /// Opens the files for `stamp` and a precompiled file of `record_count` records, reading
    /// what an earlier run with the same stamp kept into `loaded`, or starting them empty.
    /// Returns false when they cannot be written; `loaded` is then empty.
    bool Open(const std::filesystem::path& data_path, const std::filesystem::path& index_path,
              std::string_view stamp, u32 record_count, Loaded& loaded);

    /// Thread safe from here on.

    /// The id of a blob with these bytes, written once per kind and hash.
    u32 AddBlob(BlobKind kind, u64 hash, std::span<const u8> bytes);

    /// Notes that a run starts preparing `index`. An open after a crash finds it unfinished.
    void MarkStarted(u32 index);

    /// Notes that a run stopped before preparing `index` (cancelled): not a crash.
    void MarkAbandoned(u32 index);

    /// Notes a prepared record, after flushing the blobs it names. It is also queued for
    /// TakePublished.
    void AddRecord(const Record& record);

    /// The records added since the last call, and the blobs added since `known_blobs` (the
    /// count the caller already has), for the reader that draws with them.
    void TakePublished(size_t known_blobs, std::vector<Blob>& new_blobs,
                       std::vector<Record>& new_records);

    /// True when records were added that TakePublished has not returned yet.
    bool HasPublished() const;

    /// Flushes and closes both files.
    void Close();

    static constexpr std::string_view DataMagic{"NX2PDAT1"};
    static constexpr std::string_view IndexMagic{"NX2PIDX1"};

private:
    enum class EntryType : u8 {
        Blob = 1,
        Record = 2,
        Started = 3,
        Abandoned = 4,
    };

    bool Start(const std::filesystem::path& data_path, const std::filesystem::path& index_path,
               std::string_view stamp, u32 record_count);
    bool Resume(const std::filesystem::path& data_path, const std::filesystem::path& index_path,
                std::string_view stamp, u32 record_count, Loaded& loaded);
    void WriteMarker(EntryType type, u32 index);
    void WriteRecordLocked(const Record& record);

    mutable std::mutex mutex;
    std::ofstream data;
    std::ofstream index;
    u64 data_offset{};
    /// Blobs written to the data file; those past `indexed_blobs` are not in the index yet
    std::vector<Blob> blobs;
    size_t indexed_blobs{};
    std::unordered_map<u64, u32> blob_ids[2]; ///< By kind, by hash
    std::vector<Record> published;
};

} // namespace VideoCommon
