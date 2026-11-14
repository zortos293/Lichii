// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/precompiled_store.h"

namespace {

using VideoCommon::PrecompiledStore;
using State = PrecompiledStore::RecordState;
using Kind = PrecompiledStore::BlobKind;

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("eden-precompiled-store-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path Data() const {
        return path / "kept.bin";
    }
    std::filesystem::path Index() const {
        return path / "kept_index.bin";
    }
};

std::vector<u8> Bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

PrecompiledStore::Record PreparedRecord(u32 index, u32 info, u32 spirv) {
    PrecompiledStore::Record record;
    record.index = index;
    record.key_hash = 0x1000 + index;
    record.env_count = 2;
    record.envs[0] = 7;
    record.envs[1] = 8;
    record.infos[0] = info;
    record.spirv[0] = spirv;
    return record;
}

std::string ReadBlob(const std::filesystem::path& data, const PrecompiledStore::Blob& blob) {
    std::ifstream file(data, std::ios::binary);
    file.seekg(static_cast<std::streamoff>(blob.offset));
    std::string bytes(blob.size, '\0');
    file.read(bytes.data(), blob.size);
    return bytes;
}

} // Anonymous namespace

TEST_CASE("PrecompiledStore: a later open continues with what a run kept", "[video_core]") {
    TempDir dir;
    {
        PrecompiledStore store;
        PrecompiledStore::Loaded loaded;
        REQUIRE(store.Open(dir.Data(), dir.Index(), "stamp A", 4, loaded));
        REQUIRE_FALSE(loaded.resumed);
        REQUIRE(loaded.states.size() == 4);
        const u32 info = store.AddBlob(Kind::Info, 11, Bytes("info"));
        const u32 spirv = store.AddBlob(Kind::Spirv, 11, Bytes("spirv"));
        // The same bytes are kept once per kind.
        REQUIRE(store.AddBlob(Kind::Info, 11, Bytes("info")) == info);
        REQUIRE(info != spirv);
        store.MarkStarted(2);
        store.AddRecord(PreparedRecord(2, info, spirv));
        store.MarkStarted(0);
        store.MarkAbandoned(0);

        std::vector<PrecompiledStore::Blob> blobs;
        std::vector<PrecompiledStore::Record> records;
        store.TakePublished(0, blobs, records);
        REQUIRE(blobs.size() == 2);
        REQUIRE(records.size() == 1);
        REQUIRE(records[0].index == 2);
        store.TakePublished(2, blobs, records);
        REQUIRE(blobs.empty());
        REQUIRE(records.empty());
    }
    PrecompiledStore store;
    PrecompiledStore::Loaded loaded;
    REQUIRE(store.Open(dir.Data(), dir.Index(), "stamp A", 4, loaded));
    REQUIRE(loaded.resumed);
    REQUIRE(loaded.interrupted == 0);
    REQUIRE(loaded.states[0] == State::Pending);
    REQUIRE(loaded.states[2] == State::Prepared);
    REQUIRE(loaded.records.size() == 1);
    REQUIRE(loaded.records[0].key_hash == 0x1002);
    REQUIRE(loaded.records[0].envs[1] == 8);
    REQUIRE(loaded.blobs.size() == 2);
    REQUIRE(ReadBlob(dir.Data(), loaded.blobs[loaded.records[0].infos[0]]) == "info");
    REQUIRE(ReadBlob(dir.Data(), loaded.blobs[loaded.records[0].spirv[0]]) == "spirv");
    // A blob kept before is found again, a new one follows the others.
    REQUIRE(store.AddBlob(Kind::Spirv, 11, Bytes("spirv")) == loaded.records[0].spirv[0]);
    const u32 more = store.AddBlob(Kind::Info, 12, Bytes("more"));
    REQUIRE(more == 2);
    store.AddRecord(PreparedRecord(1, more, PrecompiledStore::NoBlob));
    store.Close();

    PrecompiledStore again;
    REQUIRE(again.Open(dir.Data(), dir.Index(), "stamp A", 4, loaded));
    REQUIRE(loaded.records.size() == 2);
    REQUIRE(ReadBlob(dir.Data(), loaded.blobs[more]) == "more");
}

TEST_CASE("PrecompiledStore: another stamp or record count starts again", "[video_core]") {
    TempDir dir;
    {
        PrecompiledStore store;
        PrecompiledStore::Loaded loaded;
        REQUIRE(store.Open(dir.Data(), dir.Index(), "stamp A", 4, loaded));
        store.AddRecord(PreparedRecord(1, store.AddBlob(Kind::Info, 1, Bytes("x")),
                                       PrecompiledStore::NoBlob));
    }
    {
        PrecompiledStore store;
        PrecompiledStore::Loaded loaded;
        REQUIRE(store.Open(dir.Data(), dir.Index(), "stamp B", 4, loaded));
        REQUIRE_FALSE(loaded.resumed);
        REQUIRE(loaded.records.empty());
    }
    PrecompiledStore store;
    PrecompiledStore::Loaded loaded;
    REQUIRE(store.Open(dir.Data(), dir.Index(), "stamp B", 5, loaded));
    REQUIRE_FALSE(loaded.resumed);
    REQUIRE(loaded.states.size() == 5);
}

TEST_CASE("PrecompiledStore: a cut-off index keeps the entries before the cut", "[video_core]") {
    TempDir dir;
    {
        PrecompiledStore store;
        PrecompiledStore::Loaded loaded;
        REQUIRE(store.Open(dir.Data(), dir.Index(), "s", 8, loaded));
        store.AddRecord(PreparedRecord(3, store.AddBlob(Kind::Info, 1, Bytes("a")),
                                       PrecompiledStore::NoBlob));
        store.AddRecord(PreparedRecord(4, store.AddBlob(Kind::Info, 2, Bytes("b")),
                                       PrecompiledStore::NoBlob));
    }
    // Cut the last record's entry in half.
    const auto size = std::filesystem::file_size(dir.Index());
    std::filesystem::resize_file(dir.Index(), size - 30);
    {
        PrecompiledStore store;
        PrecompiledStore::Loaded loaded;
        REQUIRE(store.Open(dir.Data(), dir.Index(), "s", 8, loaded));
        REQUIRE(loaded.resumed);
        REQUIRE(loaded.states[3] == State::Prepared);
        REQUIRE(loaded.states[4] == State::Pending);
        // The next entries follow the last whole one.
        store.AddRecord(PreparedRecord(5, PrecompiledStore::NoBlob, PrecompiledStore::NoBlob));
    }
    PrecompiledStore store;
    PrecompiledStore::Loaded loaded;
    REQUIRE(store.Open(dir.Data(), dir.Index(), "s", 8, loaded));
    REQUIRE(loaded.states[3] == State::Prepared);
    REQUIRE(loaded.states[5] == State::Prepared);
}

TEST_CASE("PrecompiledStore: an index naming data the file lacks stops there", "[video_core]") {
    TempDir dir;
    {
        PrecompiledStore store;
        PrecompiledStore::Loaded loaded;
        REQUIRE(store.Open(dir.Data(), dir.Index(), "s", 8, loaded));
        store.AddRecord(PreparedRecord(1, store.AddBlob(Kind::Info, 1, Bytes("first")),
                                       PrecompiledStore::NoBlob));
        store.AddRecord(PreparedRecord(2, store.AddBlob(Kind::Info, 2, Bytes("second")),
                                       PrecompiledStore::NoBlob));
    }
    // As after a power loss: the data file lost its last blob, the index did not.
    const auto size = std::filesystem::file_size(dir.Data());
    std::filesystem::resize_file(dir.Data(), size - 3);
    PrecompiledStore store;
    PrecompiledStore::Loaded loaded;
    REQUIRE(store.Open(dir.Data(), dir.Index(), "s", 8, loaded));
    REQUIRE(loaded.states[1] == State::Prepared);
    REQUIRE(loaded.states[2] == State::Pending);
    REQUIRE(loaded.blobs.size() == 1);
}

TEST_CASE("PrecompiledStore: a record two runs never finished is given up", "[video_core]") {
    TempDir dir;
    for (int run = 0; run < 2; ++run) {
        PrecompiledStore store;
        PrecompiledStore::Loaded loaded;
        REQUIRE(store.Open(dir.Data(), dir.Index(), "s", 4, loaded));
        if (run == 1) {
            // The first run's start of record 1 had no end: interrupted once, still tried.
            REQUIRE(loaded.interrupted == 1);
            REQUIRE(loaded.poisoned == 0);
            REQUIRE(loaded.states[1] == State::Pending);
        }
        store.MarkStarted(1);
        // Ends like a crash: no record, no abandon.
    }
    {
        PrecompiledStore store;
        PrecompiledStore::Loaded loaded;
        REQUIRE(store.Open(dir.Data(), dir.Index(), "s", 4, loaded));
        REQUIRE(loaded.poisoned == 1);
        REQUIRE(loaded.states[1] == State::Poisoned);
    }
    // Written down: found done (failed) from now on.
    PrecompiledStore store;
    PrecompiledStore::Loaded loaded;
    REQUIRE(store.Open(dir.Data(), dir.Index(), "s", 4, loaded));
    REQUIRE(loaded.poisoned == 0);
    REQUIRE(loaded.states[1] == State::Prepared);
    REQUIRE(loaded.records.size() == 1);
    REQUIRE(loaded.records[0].failed);
}
