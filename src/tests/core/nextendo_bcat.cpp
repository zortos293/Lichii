// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "common/fs/file_lock.h"
#include "core/debugger/nx2_zip.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/bcat/backend/nextendo.h"

namespace {

namespace fs = std::filesystem;
using namespace Service::BCAT::Nextendo;
using Core::Nx2Debug::ZipWriter;

using Files = std::vector<std::pair<std::string, std::string>>;

constexpr u64 Splatoon2 = 0x0100F8F0000A2000;

std::vector<u8> Bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

fs::path TempPath(std::string_view what) {
    return fs::temp_directory_path() /
           fmt::format("lichii-bcat-{}-{}", what,
                       std::chrono::steady_clock::now().time_since_epoch().count());
}

// A synthetic package written with the bug bundle's zip writer: deflated where that is smaller.
std::vector<u8> Package(const Files& files) {
    const auto path = TempPath("zip");
    {
        ZipWriter zip{path};
        for (const auto& [name, text] : files) {
            zip.Add(ZipWriter::Prepare(name, Bytes(text), true));
        }
        zip.Finish();
    }
    std::ifstream in{path, std::ios::binary};
    std::vector<u8> bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    in.close();
    fs::remove(path);
    return bytes;
}

std::size_t CentralRecord(const std::vector<u8>& zip) {
    constexpr std::array<u8, 4> signature{0x50, 0x4b, 0x01, 0x02};
    return static_cast<std::size_t>(
        std::search(zip.begin(), zip.end(), signature.begin(), signature.end()) - zip.begin());
}

std::string ReadText(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

Fetch Serve(std::optional<Response> response) {
    return [response](u64) { return response; };
}

// A new package each call, stamped with the time (to 2 s) the zip writer puts in each entry: two
// made a moment apart can differ, and Update takes that for a new delivery. A test that expects
// Unchanged serves one package for every check.
Response Ok(const Files& files) {
    return {200, Package(files)};
}

struct TempDir {
    fs::path path = TempPath("cache");
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path.parent_path() / (path.filename().string() + "-root"), ec);
    }
    fs::path Target() const {
        return path.parent_path() / (path.filename().string() + "-root") / "0100F8F0000A2000";
    }
};

const Files Schedule{
    {"vsdata/VSSetting_0.byaml", std::string(2048, 'v')},
    {"vsdata/VSSetting_1.byaml", "second"},
    {"coopdata/CoopSetting.byaml", "coop"},
};

} // Anonymous namespace

TEST_CASE("Nextendo BCAT reads a delivery cache package", "[core][bcat]") {
    const auto contents = ReadPackage(Package({
                                          {"line_news/", ""},
                                          {"line_news/nx_line_news_0001", std::string(4096, 'n')},
                                          {"popup_news/nx_popup_0001", "ab"},
                                      }),
                                      0x01006A800016E000);
    REQUIRE(contents.size() == 2);
    REQUIRE(contents.at("line_news/nx_line_news_0001") == Bytes(std::string(4096, 'n')));
    REQUIRE(contents.at("popup_news/nx_popup_0001") == Bytes("ab"));
}

TEST_CASE("Nextendo BCAT rejects packages that are not a plain delivery cache", "[core][bcat]") {
    const auto rejects = [](const Files& files) {
        REQUIRE_THROWS(ReadPackage(Package(files), Splatoon2));
    };
    rejects({{"root_file", "x"}});
    rejects({{"dir/sub/file", "x"}});
    rejects({{"../file", "x"}});
    rejects({{"dir/..", "x"}});
    rejects({{"dir/" + std::string(32, 'f'), "x"}});
    rejects({{"dir/bad name", "x"}});
    rejects({{"dir.x/file", "x"}});
    rejects({{"dir/file-x", "x"}});
    rejects({{"dir/file", "x"}, {"dir/file.", "y"}});
    rejects({{"dir\\file", "x"}});
    rejects({{"Dir/file", "x"}, {"dir/FILE", "y"}});
    rejects({});

    Files many;
    for (std::size_t i = 0; i <= MaxEntries; ++i) {
        many.emplace_back(fmt::format("dir/f{}", i), "x");
    }
    rejects(many);

    REQUIRE_THROWS(ReadPackage(Bytes("not a zip at all, just text"), Splatoon2));

    const auto good = Package({{"dir/file", "abcdef"}});
    REQUIRE_NOTHROW(ReadPackage(good, Splatoon2));

    auto corrupt = good;
    const std::string payload = "abcdef";
    *std::search(corrupt.begin(), corrupt.end(), payload.begin(), payload.end()) = 'X';
    REQUIRE_THROWS(ReadPackage(corrupt, Splatoon2));

    auto link = good;
    const auto central = CentralRecord(link);
    link[central + 5] = 3;
    link[central + 40] = 0xff;
    link[central + 41] = 0xa1;
    REQUIRE_THROWS(ReadPackage(link, Splatoon2));

    auto encrypted = good;
    encrypted[CentralRecord(encrypted) + 8] |= 1;
    REQUIRE_THROWS(ReadPackage(encrypted, Splatoon2));

    auto truncated = good;
    truncated.erase(truncated.begin() + 40, truncated.begin() + 44);
    REQUIRE_THROWS(ReadPackage(truncated, Splatoon2));
}

TEST_CASE("Nextendo BCAT requires every Splatoon 3 region", "[core][bcat]") {
    Files files;
    for (const auto* region : {"ap-default", "eu-default", "jp-default", "us-default"}) {
        for (const auto* file : {"summary.yml", "base.pack.zs.enc", "bh.pack.zs.enc",
                                 "wa.pack.zs.enc", "wb.pack.zs.enc", "wc.pack.zs.enc"}) {
            files.emplace_back(fmt::format("{}/{}", region, file), file);
        }
    }
    REQUIRE(ReadPackage(Package(files), Splatoon3).size() == 24);

    files.pop_back();
    REQUIRE_THROWS(ReadPackage(Package(files), Splatoon3));
    REQUIRE(ReadPackage(Package(files), Splatoon2).size() == 23);
}

TEST_CASE("Nextendo BCAT installs, keeps and replaces a title's cache", "[core][bcat]") {
    const TempDir temp;
    const auto target = temp.Target();
    const auto schedule = Ok(Schedule);

    REQUIRE(Update(Splatoon2, target, Serve(schedule)) == Outcome::Installed);
    REQUIRE(ReadText(target / "vsdata" / "VSSetting_0.byaml") == std::string(2048, 'v'));
    REQUIRE(ReadText(target / "coopdata" / "CoopSetting.byaml") == "coop");
    REQUIRE(ReadText(target / HashFileName).size() == 64);

    // What the bcat services see: the directories and their files, not the hash.
    FileSys::RealVfsFilesystem vfs;
    const auto root = vfs.OpenDirectory(target.string());
    REQUIRE(root != nullptr);
    REQUIRE(root->GetSubdirectories().size() == 2);
    REQUIRE(root->GetSubdirectory("vsdata")->GetFiles().size() == 2);
    REQUIRE(root->GetSubdirectory("coopdata")->GetFile("CoopSetting.byaml")->GetSize() == 4);

    REQUIRE(Update(Splatoon2, target, Serve(schedule)) == Outcome::Unchanged);

    // A stale hash makes the next check install again.
    {
        std::ofstream out{target / HashFileName, std::ios::trunc};
        out << "stale";
    }
    REQUIRE(Update(Splatoon2, target, Serve(schedule)) == Outcome::Installed);

    REQUIRE(Update(Splatoon2, target, Serve(Response{204, {}})) == Outcome::NotPublished);
    REQUIRE(Update(Splatoon2, target, Serve(std::nullopt)) == Outcome::Failed);
    REQUIRE(Update(Splatoon2, target, Serve(Response{500, {}})) == Outcome::Failed);
    REQUIRE(Update(Splatoon2, target, Serve(Response{200, Bytes("garbage")})) ==
            Outcome::Failed);
    REQUIRE(ReadText(target / "coopdata" / "CoopSetting.byaml") == "coop");

    REQUIRE(Update(Splatoon2, target, Serve(Ok({{"vsdata/VSSetting_0.byaml", "new"}}))) ==
            Outcome::Installed);
    REQUIRE(ReadText(target / "vsdata" / "VSSetting_0.byaml") == "new");
    REQUIRE_FALSE(fs::exists(target / "vsdata" / "VSSetting_1.byaml"));
    REQUIRE_FALSE(fs::exists(target / "coopdata"));
    // No stage or backup is left beside the cache, only the empty lock file other Lichii
    // processes take.
    std::vector<std::string> beside;
    for (const auto& entry : fs::directory_iterator{target.parent_path()}) {
        beside.push_back(entry.path().filename().string());
    }
    std::ranges::sort(beside);
    REQUIRE(beside ==
            std::vector<std::string>{"0100F8F0000A2000", "0100F8F0000A2000.nextendo-lock"});
    REQUIRE(fs::file_size(target.parent_path() / "0100F8F0000A2000.nextendo-lock") == 0);
}

TEST_CASE("Nextendo BCAT reinstalls over a hash marker it can't read", "[core][bcat]") {
    const TempDir temp;
    const auto target = temp.Target();
    const auto schedule = Ok(Schedule);
    REQUIRE(Update(Splatoon2, target, Serve(schedule)) == Outcome::Installed);
    const auto hash = ReadText(target / HashFileName);

    fs::remove(target / HashFileName);
    fs::create_directory(target / HashFileName);
    REQUIRE(Update(Splatoon2, target, Serve(schedule)) == Outcome::Installed);
    REQUIRE(ReadText(target / HashFileName) == hash);

    {
        std::ofstream out{target / HashFileName, std::ios::app};
        out << "x";
    }
    REQUIRE(Update(Splatoon2, target, Serve(schedule)) == Outcome::Installed);
    REQUIRE(Update(Splatoon2, target, Serve(schedule)) == Outcome::Unchanged);
}

TEST_CASE("Nextendo BCAT installs Violet's event for Scarlet", "[core][bcat]") {
    const TempDir temp;
    std::vector<u64> asked;
    const Fetch fetch = [&](u64 title_id) -> std::optional<Response> {
        asked.push_back(title_id);
        if (title_id == PokemonViolet) {
            return Ok({{"raid/event", "tera"}});
        }
        return Response{204, {}};
    };
    REQUIRE(Update(PokemonScarlet, temp.Target(), fetch) == Outcome::Installed);
    REQUIRE(asked == std::vector<u64>{PokemonScarlet, PokemonViolet});
    REQUIRE(ReadText(temp.Target() / "raid" / "event") == "tera");
}

TEST_CASE("Nextendo BCAT restores a cache an interrupted install moved away", "[core][bcat]") {
    const TempDir temp;
    const auto target = temp.Target();
    auto backup = target;
    backup += ".nextendo-backup";
    fs::create_directories(backup / "vsdata");
    {
        std::ofstream out{backup / "vsdata" / "VSSetting_0.byaml"};
        out << "old";
    }
    SECTION("before the next check") {
        REQUIRE(Update(Splatoon2, target, Serve(std::nullopt)) == Outcome::Failed);
    }
    SECTION("after the services created an empty folder, with Nextendo off") {
        fs::create_directories(target);
        RecoverInterrupted(target);
    }
    REQUIRE(ReadText(target / "vsdata" / "VSSetting_0.byaml") == "old");
    REQUIRE_FALSE(fs::exists(backup));
}

namespace {

fs::path WithSuffix(const fs::path& target, std::string_view suffix) {
    auto path = target;
    path += suffix;
    return path;
}

void WriteText(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary};
    out << text;
}

} // Anonymous namespace

// Another Lichii process installing the same title holds target + ".nextendo-lock" while it writes
// its stage and swaps it in. A second installer that cleared that stage, or a recovery that took
// its backup, left the target with part of a package under a complete hash marker (#2207).
TEST_CASE("Nextendo BCAT waits for another process installing the same title", "[core][bcat]") {
    using namespace std::chrono_literals;
    const TempDir temp;
    const auto target = temp.Target();
    const auto stage = WithSuffix(target, ".nextendo-stage");
    const auto lock_path = WithSuffix(target, ".nextendo-lock");
    const auto package = Package(Schedule);
    const auto contents = ReadPackage(package, Splatoon2);
    fs::create_directories(target.parent_path());

    // The other process is halfway through writing its stage.
    std::optional<Common::FS::FileLock> other{std::in_place, lock_path};
    REQUIRE(other->Held());
    WriteText(stage / "vsdata" / "VSSetting_0.byaml", "other's first file");

    SECTION("it keeps the lock past the wait") {
        REQUIRE_THROWS(Install(contents, target, "other-hash", 100ms));
        REQUIRE(ReadText(stage / "vsdata" / "VSSetting_0.byaml") == "other's first file");
        REQUIRE_FALSE(fs::exists(target));
    }
    SECTION("it finishes during the wait") {
        std::thread finish{[&] {
            std::this_thread::sleep_for(200ms);
            other.reset();
        }};
        std::string error;
        bool installed = false;
        try {
            installed = Install(contents, target, std::string(64, 'a'), 10s);
        } catch (const std::exception& e) {
            error = e.what();
        }
        finish.join();
        REQUIRE(error == "");
        REQUIRE(installed);
        for (const auto& [name, data] : contents) {
            REQUIRE(ReadText(target / name) == std::string(data.begin(), data.end()));
        }
        REQUIRE(ReadText(target / ".nextendo_bcat_hash") == std::string(64, 'a'));
        REQUIRE_FALSE(fs::exists(stage));
    }
}

TEST_CASE("Nextendo BCAT recovery leaves another process's swap alone", "[core][bcat]") {
    using namespace std::chrono_literals;
    const TempDir temp;
    const auto target = temp.Target();
    const auto backup = WithSuffix(target, ".nextendo-backup");
    // Between the other process's two renames: the old cache is its backup, the target is gone.
    WriteText(backup / "vsdata" / "VSSetting_0.byaml", "old");
    {
        const Common::FS::FileLock other{WithSuffix(target, ".nextendo-lock")};
        REQUIRE(other.Held());
        // The services serve an empty cache rather than create the target it renames its stage to.
        REQUIRE_FALSE(RecoverInterrupted(target, 100ms));
        REQUIRE(ReadText(backup / "vsdata" / "VSSetting_0.byaml") == "old");
        REQUIRE_FALSE(fs::exists(target));
    }
    // Once no process holds it, the backup is an interrupted install's and is put back.
    REQUIRE(RecoverInterrupted(target, 100ms));
    REQUIRE(ReadText(target / "vsdata" / "VSSetting_0.byaml") == "old");
    REQUIRE_FALSE(fs::exists(backup));
}

TEST_CASE("Nextendo BCAT recovery waits on another process's first install", "[core][bcat]") {
    using namespace std::chrono_literals;
    const TempDir temp;
    const auto target = temp.Target();
    const auto lock_path = WithSuffix(target, ".nextendo-lock");
    // Nothing installed and nothing being installed: no lock file is left beside the cache.
    REQUIRE(RecoverInterrupted(target, 100ms));
    REQUIRE_FALSE(fs::exists(lock_path));

    WriteText(WithSuffix(target, ".nextendo-stage") / "vsdata" / "VSSetting_0.byaml", "new");
    const Common::FS::FileLock other{lock_path};
    REQUIRE(other.Held());
    REQUIRE_FALSE(RecoverInterrupted(target, 100ms));
    REQUIRE_FALSE(fs::exists(target));
}

// FileLock's timed wait only tries once on a path that already stayed taken through a wait, so a
// recovery that timed out made the install right after it give up at once.
TEST_CASE("Nextendo BCAT installs wait in full after a recovery timed out", "[core][bcat]") {
    using namespace std::chrono_literals;
    const TempDir temp;
    const auto target = temp.Target();
    const auto contents = ReadPackage(Package(Schedule), Splatoon2);
    WriteText(WithSuffix(target, ".nextendo-backup") / "vsdata" / "VSSetting_0.byaml", "old");

    std::optional<Common::FS::FileLock> other{std::in_place, WithSuffix(target, ".nextendo-lock")};
    REQUIRE(other->Held());
    REQUIRE_FALSE(RecoverInterrupted(target, 100ms));
    std::thread finish{[&] {
        std::this_thread::sleep_for(200ms);
        other.reset();
    }};
    // Joined before any check fails, so a failure doesn't end the run.
    std::string error;
    bool installed = false;
    try {
        installed = Install(contents, target, std::string(64, 'a'), 2s);
    } catch (const std::exception& e) {
        error = e.what();
    }
    finish.join();
    REQUIRE(error == "");
    REQUIRE(installed);
    REQUIRE(ReadText(target / "coopdata" / "CoopSetting.byaml") == "coop");
}

TEST_CASE("Nextendo BCAT keeps what another process installed while it waited", "[core][bcat]") {
    using namespace std::chrono_literals;
    const TempDir temp;
    const auto target = temp.Target();
    const auto lock_path = WithSuffix(target, ".nextendo-lock");
    REQUIRE(Update(Splatoon2, target, Serve(Ok({{"vsdata/VSSetting_0.byaml", "first"}}))) ==
            Outcome::Installed);

    const auto package = Package(Schedule);
    std::string package_hash;
    {
        const TempDir scratch;
        REQUIRE(Update(Splatoon2, scratch.Target(), Serve(Response{200, package})) ==
                Outcome::Installed);
        package_hash = ReadText(scratch.Target() / HashFileName);
    }

    // The other process takes the lock after this one looked at target, and installs while this
    // one fetches and waits.
    std::string other_hash;
    SECTION("the same package") {
        other_hash = package_hash;
    }
    SECTION("a newer package") {
        other_hash = std::string(64, 'c');
    }
    std::optional<Common::FS::FileLock> other;
    std::thread finish;
    const Fetch fetch = [&](u64) -> std::optional<Response> {
        other.emplace(lock_path);
        REQUIRE(other->Held());
        finish = std::thread{[&] {
            std::this_thread::sleep_for(200ms);
            WriteText(target / "vsdata" / "VSSetting_0.byaml", "other's");
            WriteText(target / HashFileName, other_hash);
            other.reset();
        }};
        return Response{200, package};
    };
    const auto outcome = Update(Splatoon2, target, fetch);
    finish.join();
    REQUIRE(outcome == Outcome::Unchanged);
    REQUIRE(ReadText(target / "vsdata" / "VSSetting_0.byaml") == "other's");
    REQUIRE(ReadText(target / HashFileName) == other_hash);
}

TEST_CASE("Nextendo BCAT waits for another process no later than the deadline", "[core][bcat]") {
    using namespace std::chrono_literals;
    const TempDir temp;
    const auto target = temp.Target();
    REQUIRE(Update(Splatoon2, target, Serve(Ok({{"vsdata/VSSetting_0.byaml", "first"}}))) ==
            Outcome::Installed);
    // A backup makes the recovery wait for the lock too.
    WriteText(WithSuffix(target, ".nextendo-backup") / "vsdata" / "VSSetting_0.byaml", "old");

    const Common::FS::FileLock other{WithSuffix(target, ".nextendo-lock")};
    REQUIRE(other.Held());
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(Update(Splatoon2, target, Serve(Ok(Schedule)), start + 300ms) == Outcome::Failed);
    REQUIRE(std::chrono::steady_clock::now() - start < 1500ms);
    REQUIRE(ReadText(target / "vsdata" / "VSSetting_0.byaml") == "first");
}

TEST_CASE("Nextendo BCAT recovery waits on a first install before its stage exists",
          "[core][bcat]") {
    using namespace std::chrono_literals;
    const TempDir temp;
    const auto target = temp.Target();
    // Another process took the lock for a first install and hasn't written its stage yet: only the
    // lock file shows it.
    fs::create_directories(target.parent_path());
    const Common::FS::FileLock other{WithSuffix(target, ".nextendo-lock")};
    REQUIRE(other.Held());
    REQUIRE_FALSE(RecoverInterrupted(target, 100ms));
    REQUIRE_FALSE(fs::exists(target));
}

TEST_CASE("Nextendo BCAT leaves the cache alone where it can't be locked", "[core][bcat]") {
    using namespace std::chrono_literals;
    const TempDir temp;
    const auto target = temp.Target();
    const auto lock_path = WithSuffix(target, ".nextendo-lock");
    REQUIRE(Update(Splatoon2, target, Serve(Ok({{"vsdata/VSSetting_0.byaml", "first"}}))) ==
            Outcome::Installed);

    // A folder where the lock file goes: the lock can't be opened, so whether another process holds
    // it is unknown.
    fs::remove(lock_path);
    fs::create_directories(lock_path);
    REQUIRE(Update(Splatoon2, target, Serve(Ok({{"vsdata/VSSetting_0.byaml", "second"}}))) ==
            Outcome::Failed);
    REQUIRE(ReadText(target / "vsdata" / "VSSetting_0.byaml") == "first");

    // The backup of an install cut off between its renames stays until the lock can be taken.
    const auto backup = WithSuffix(target, ".nextendo-backup");
    fs::rename(target, backup);
    REQUIRE_FALSE(RecoverInterrupted(target, 100ms));
    REQUIRE_FALSE(fs::exists(target));
    REQUIRE(ReadText(backup / "vsdata" / "VSSetting_0.byaml") == "first");

    fs::remove(lock_path);
    REQUIRE(RecoverInterrupted(target, 100ms));
    REQUIRE(ReadText(target / "vsdata" / "VSSetting_0.byaml") == "first");
    REQUIRE_FALSE(fs::exists(backup));
}
