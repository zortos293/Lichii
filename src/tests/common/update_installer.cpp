// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <fstream>
#include <iterator>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "frontend_common/update_installer.h"

namespace fs = std::filesystem;

namespace {

struct TempDir {
    TempDir() {
        path = fs::temp_directory_path() /
               ("lichii-update-installer-" + std::to_string(std::rand()) + "-" +
                std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path path;
    static inline int counter = 0;
};

void Write(const fs::path& file, const std::string& content) {
    std::ofstream{file, std::ios::binary} << content;
}

std::string Read(const fs::path& file) {
    std::ifstream stream{file, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("Staged and previous files sit beside the target", "[updater]") {
    const fs::path target = "/opt/lichii/lichii-linux-x86_64.AppImage";
    CHECK(UpdateInstaller::StagedPath(target) == "/opt/lichii/lichii-linux-x86_64.AppImage.new");
    CHECK(UpdateInstaller::PreviousPath(target) == "/opt/lichii/lichii-linux-x86_64.AppImage.prev");
}

TEST_CASE("Swap replaces the target and keeps one previous copy", "[updater]") {
    TempDir dir;
    const auto target = dir.path / "lichii.AppImage";
    const auto staged = UpdateInstaller::StagedPath(target);
    const auto previous = UpdateInstaller::PreviousPath(target);
    Write(target, "old");
    fs::permissions(target, fs::perms::owner_all | fs::perms::group_read);
    Write(staged, "new");
    Write(previous, "stale backup");

    std::error_code ec;
    REQUIRE(UpdateInstaller::CanReplace(target));
    REQUIRE(UpdateInstaller::Swap(staged, target, ec));
    CHECK_FALSE(ec);
    CHECK(Read(target) == "new");
    CHECK(Read(previous) == "old");
    CHECK_FALSE(fs::exists(staged));
    const auto permissions = fs::status(target).permissions();
    CHECK((permissions & fs::perms::owner_exec) != fs::perms::none);
    CHECK((permissions & fs::perms::group_read) != fs::perms::none);
}

TEST_CASE("Swap leaves the target untouched when the staged file is missing", "[updater]") {
    TempDir dir;
    const auto target = dir.path / "lichii.AppImage";
    Write(target, "old");

    std::error_code ec;
    CHECK_FALSE(UpdateInstaller::Swap(UpdateInstaller::StagedPath(target), target, ec));
    CHECK(ec);
    CHECK(Read(target) == "old");
    CHECK_FALSE(fs::exists(UpdateInstaller::PreviousPath(target)));
}

TEST_CASE("Rollback exchanges the target with its previous copy", "[updater]") {
    TempDir dir;
    const auto target = dir.path / "lichii.AppImage";
    const auto previous = UpdateInstaller::PreviousPath(target);
    Write(target, "new");
    Write(previous, "old");

    std::error_code ec;
    REQUIRE(UpdateInstaller::Rollback(target, ec));
    CHECK(Read(target) == "old");
    CHECK(Read(previous) == "new");
    REQUIRE(UpdateInstaller::Rollback(target, ec));
    CHECK(Read(target) == "new");
    CHECK(Read(previous) == "old");
    CHECK_FALSE(fs::exists(UpdateInstaller::StagedPath(target)));
}

TEST_CASE("Rollback without a previous copy fails and changes nothing", "[updater]") {
    TempDir dir;
    const auto target = dir.path / "lichii.AppImage";
    Write(target, "current");

    std::error_code ec;
    CHECK_FALSE(UpdateInstaller::Rollback(target, ec));
    CHECK(ec);
    CHECK(Read(target) == "current");
}

TEST_CASE("Only an existing file in a writable folder can be replaced", "[updater]") {
    TempDir dir;
    CHECK_FALSE(UpdateInstaller::CanReplace(dir.path / "missing.AppImage"));
    CHECK_FALSE(UpdateInstaller::CanReplace(dir.path));
    CHECK_FALSE(fs::exists(dir.path / ".lichii-update-write-test"));
}

TEST_CASE("Path list entries inside a folder are removed", "[updater]") {
    using UpdateInstaller::StripPathListEntries;
    CHECK(StripPathListEntries("/tmp/.mount_x/usr/lib:/usr/lib:/tmp/.mount_x:/opt/lib",
                               "/tmp/.mount_x") == "/usr/lib:/opt/lib");
    CHECK(StripPathListEntries("/tmp/.mount_xy/lib:/usr/lib", "/tmp/.mount_x") ==
          "/tmp/.mount_xy/lib:/usr/lib");
    CHECK(StripPathListEntries("", "/tmp/.mount_x").empty());
    CHECK(StripPathListEntries("/a::/b", "/tmp/.mount_x") == "/a:/b");
    CHECK(StripPathListEntries("/a:/b", "") == "/a:/b");
}
