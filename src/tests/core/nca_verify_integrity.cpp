// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <openssl/evp.h>

#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/nca.h"
#include "tests/common/restore_keys.h"

namespace {

using namespace FileSys;

/// A file that returns nothing for `zero_reads` reads at `stall_at` (all of them if `zero_reads`
/// is ~0), then data again, and hands out at most `chunk` bytes per read.
class StallingFile final : public VectorVfsFile {
public:
    StallingFile(std::vector<u8> data, std::string name, std::size_t stall_at_,
                 std::size_t zero_reads_, std::size_t chunk_)
        : VectorVfsFile(std::move(data), std::move(name)), stall_at{stall_at_},
          zero_reads{zero_reads_}, chunk{chunk_} {}

    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override {
        if (offset >= stall_at && zero_reads != 0) {
            if (zero_reads != ~std::size_t{0}) {
                --zero_reads;
            }
            return 0;
        }
        const std::size_t limit = offset < stall_at ? stall_at - offset : length;
        return VectorVfsFile::Read(data, std::min({length, chunk, limit}), offset);
    }

private:
    std::size_t stall_at;
    mutable std::size_t zero_reads;
    std::size_t chunk;
};

constexpr std::size_t Never = ~std::size_t{0};

std::vector<u8> Contents() {
    std::vector<u8> data(3 * 4096 + 123);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<u8>(i * 7 + 3);
    }
    return data;
}

/// The name an NCA with these contents has: the first half of its SHA-256, in hex, then ".nca".
std::string NcaName(const std::vector<u8>& data) {
    std::array<u8, 32> hash{};
    unsigned int size = 0;
    REQUIRE(EVP_Digest(data.data(), data.size(), hash.data(), &size, EVP_sha256(), nullptr) == 1);
    return Common::HexToString(std::span{hash}.first<16>(), false) + ".nca";
}

/// AppLoader_NCA reads keys through the KeyManager: point it at an empty folder for the test and
/// make it forget afterwards, so the test reads none of the machine's keys and leaves none behind.
class EmptyKeys {
public:
    EmptyKeys()
        : original{Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir)},
          root{std::filesystem::temp_directory_path() /
               fmt::format("lichii-nca-verify-{}", std::random_device{}())} {
        REQUIRE(std::filesystem::create_directories(root));
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, root);
    }
    ~EmptyKeys() {
        Tests::RestoreKeys(original);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    EmptyKeys(const EmptyKeys&) = delete;
    EmptyKeys& operator=(const EmptyKeys&) = delete;

private:
    std::filesystem::path original;
    std::filesystem::path root;
};

Loader::ResultStatus VerifyNca(const VirtualFile& file,
                               std::function<bool(std::size_t, std::size_t)> progress) {
    const EmptyKeys keys;
    Loader::AppLoader_NCA loader{file};
    return loader.VerifyIntegrity(std::move(progress));
}

/// A progress function that fails the test, instead of cancelling, if it is called far more often
/// than the file's few reads and waits can explain.
std::function<bool(std::size_t, std::size_t)> Progress(std::size_t& calls) {
    return [&calls](std::size_t, std::size_t) {
        if (++calls > 1000) {
            FAIL("the verification loop no longer advances");
        }
        return true;
    };
}

} // Anonymous namespace

TEST_CASE("NCA verification waits for a file that returns nothing for a while", "[core][loader]") {
    // As the solid NCZ reader does while it waits to decode again: a few empty reads, then data.
    const auto data = Contents();
    const auto file = std::make_shared<StallingFile>(data, NcaName(data), 5000, 3, data.size());
    std::size_t calls = 0;
    CHECK(VerifyNca(file, Progress(calls)) == Loader::ResultStatus::Success);
}

TEST_CASE("NCA verification fails on a file that stops returning data", "[core][loader]") {
    const auto data = Contents();
    const auto file = std::make_shared<StallingFile>(data, NcaName(data), 5000, Never, data.size());
    std::vector<u8> buffer(4096);
    std::size_t calls = 0;
    const auto begin = std::chrono::steady_clock::now();
    const auto result = Loader::ReadForVerification(
        file, buffer, [](std::span<const u8>) { return true; }, Progress(calls),
        {.stall_limit = std::chrono::milliseconds{200}, .poll_interval = std::chrono::milliseconds{5}});
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    CHECK(result == Loader::ResultStatus::ErrorIntegrityVerificationFailed);
    // It used to read nothing forever, until the user cancelled.
    CHECK(elapsed >= std::chrono::milliseconds{200});
    CHECK(elapsed < std::chrono::seconds{5});
}

TEST_CASE("NCA verification can be cancelled while a file returns nothing", "[core][loader]") {
    const auto data = Contents();
    const auto file = std::make_shared<StallingFile>(data, NcaName(data), 5000, Never, data.size());
    std::size_t waits = 0;
    const auto begin = std::chrono::steady_clock::now();
    const auto result = VerifyNca(file, [&waits](std::size_t done, std::size_t) {
        // Cancel on the third call made while no data comes.
        return !(done == 5000 && ++waits == 3);
    });
    CHECK(result == Loader::ResultStatus::ErrorIntegrityVerificationFailed);
    CHECK(waits == 3);
    CHECK(std::chrono::steady_clock::now() - begin < std::chrono::seconds{5});
}

TEST_CASE("NCA verification passes a whole file, read whole, in pieces or without progress",
          "[core][loader]") {
    const auto data = Contents();
    for (const std::size_t chunk : {data.size(), std::size_t{1000}}) {
        INFO("chunk " << chunk);
        const auto file = std::make_shared<StallingFile>(data, NcaName(data), data.size(), 0, chunk);
        std::size_t calls = 0;
        CHECK(VerifyNca(file, Progress(calls)) == Loader::ResultStatus::Success);
    }
    const auto file = std::make_shared<StallingFile>(data, NcaName(data), data.size(), 0, 1000);
    CHECK(VerifyNca(file, {}) == Loader::ResultStatus::Success);
}
