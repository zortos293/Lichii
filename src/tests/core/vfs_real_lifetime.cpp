// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "core/file_sys/vfs/vfs_real.h"

namespace {

namespace fs = std::filesystem;

// Scratch directory unique to each test invocation: two test processes never
// share paths, and the directory goes away with the test, early exit included.
class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned long long> counter{};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        std::random_device rd;
        path = fs::temp_directory_path() /
               ("lichii_vfs_lifetime_" + std::to_string(stamp) + "_" + std::to_string(rd()) + "_" +
                std::to_string(counter++));
        fs::create_directory(path);
    }
    ~TempDir() {
        std::error_code error;
        fs::remove_all(path, error);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    // Writes a small file to a uniquely named path and returns the path.
    fs::path File(const std::string& name, const std::string& contents) const {
        const fs::path file_path = path / name;
        std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
        out << contents;
        return file_path;
    }

private:
    fs::path path;
};

} // Anonymous namespace

// Regression test for Lichii #1673: a RealVfsFile that outlives its
// RealVfsFilesystem must not dereference the destroyed filesystem. Before the
// fix this was a use-after-free (ASan) or an abort on hardened libc++ when
// ~RealVfsFile locked the destroyed filesystem's mutex during app shutdown.
TEST_CASE("RealVfsFile survives its RealVfsFilesystem", "[core][file_sys]") {
    const TempDir dir;
    const auto path = dir.File("vfs_lifetime.txt", "hello-vfs");

    FileSys::VirtualFile file;
    FileSys::VirtualFile lazy;
    {
        FileSys::RealVfsFilesystem real_fs;
        file = real_fs.OpenFile(path.generic_string(), FileSys::OpenMode::Read);
        REQUIRE(file != nullptr);

        // Open the OS handle before teardown.
        char buf[16] = {};
        REQUIRE(file->Read(reinterpret_cast<u8*>(buf), sizeof(buf) - 1, 0) == 9);

        lazy = real_fs.OpenFile(path.generic_string(), FileSys::OpenMode::Read);
        REQUIRE(lazy != nullptr);
    } // real_fs is destroyed with both files outstanding

    // Destruction of the surviving files must be safe.
    file = nullptr;
    lazy = nullptr;
    SUCCEED("files outlived the filesystem without touching it");
}

TEST_CASE("RealVfsFile operations after filesystem teardown are defined", "[core][file_sys]") {
    const TempDir dir;
    const auto path = dir.File("vfs_postmortem.txt", "hello-vfs");

    FileSys::VirtualFile file;
    {
        FileSys::RealVfsFilesystem real_fs;
        file = real_fs.OpenFile(path.generic_string(), FileSys::OpenMode::Read);
        REQUIRE(file != nullptr);
        char buf[16] = {};
        REQUIRE(file->Read(reinterpret_cast<u8*>(buf), sizeof(buf) - 1, 0) == 9);
    }

    // An already-open handle stays usable; the filesystem object is not touched.
    char buf[16] = {};
    CHECK(file->Read(reinterpret_cast<u8*>(buf), sizeof(buf) - 1, 0) == 9);

    // Operations that need the filesystem itself fail cleanly instead.
    CHECK(file->GetContainingDirectory() == nullptr);
    CHECK_FALSE(file->Rename("renamed"));

    file = nullptr;
}

TEST_CASE("RealVfsFilesystem alive during concurrent file destruction", "[core][file_sys]") {
    // Coverage: files are dropped on another thread while the filesystem is
    // alive, then the remaining files outlive the filesystem. This does NOT
    // race the filesystem's own destruction (see the next case for that).
    // Distinct paths: the cache dedupes same-path opens into one object.
    const TempDir dir;
    std::vector<fs::path> paths;
    for (int i = 0; i < 16; i++) {
        paths.push_back(dir.File("vfs_concurrent_" + std::to_string(i) + ".txt", "hello-vfs"));
    }

    std::vector<FileSys::VirtualFile> files;
    {
        FileSys::RealVfsFilesystem real_fs;
        for (int round = 0; round < 4; round++) {
            for (const auto& p : paths) {
                files.push_back(real_fs.OpenFile(p.generic_string(), FileSys::OpenMode::Read));
            }
        }
        std::thread t([&] {
            for (int i = 0; i < 32; i++) {
                files[i] = nullptr;
            }
        });
        t.join();
    } // real_fs dies with files outstanding

    for (auto& f : files) {
        f = nullptr;
    }
    SUCCEED("concurrent destruction completed safely");
}

TEST_CASE("RealVfsFilesystem destroyed concurrently with file destruction", "[core][file_sys]") {
    // Genuine concurrent teardown: the filesystem object dies on this thread
    // while worker threads are destroying files it handed out. Safe because
    // file destruction only touches the shared state block, which outlives
    // every participant.
    const TempDir dir;
    std::vector<fs::path> paths;
    for (int i = 0; i < 8; i++) {
        paths.push_back(dir.File("vfs_race_" + std::to_string(i) + ".txt", "hello-vfs"));
    }

    auto real_fs = std::make_unique<FileSys::RealVfsFilesystem>();
    std::vector<FileSys::VirtualFile> files;
    for (const auto& p : paths) {
        files.push_back(real_fs->OpenFile(p.generic_string(), FileSys::OpenMode::Read));
        REQUIRE(files.back() != nullptr);
    }

    std::atomic<bool> go{false};
    auto dropper = [&](std::size_t begin, std::size_t end) {
        while (!go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (std::size_t i = begin; i < end; i++) {
            files[i] = nullptr;
        }
    };
    std::thread t1(dropper, 0, files.size() / 2);
    std::thread t2(dropper, files.size() / 2, files.size());

    go.store(true, std::memory_order_release);
    real_fs.reset(); // destroy the filesystem while the droppers run
    t1.join();
    t2.join();

    SUCCEED("filesystem destruction raced file destruction safely");
}

TEST_CASE("RealVfsFilesystem cache shares file objects for the same path", "[core][file_sys]") {
    const TempDir dir;
    const auto path = dir.File("vfs_cache.txt", "hello-vfs");

    FileSys::RealVfsFilesystem real_fs;
    auto a = real_fs.OpenFile(path.generic_string(), FileSys::OpenMode::Read);
    auto b = real_fs.OpenFile(path.generic_string(), FileSys::OpenMode::Read);
    REQUIRE(a != nullptr);
    CHECK(a == b);
}
