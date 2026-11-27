// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/scope_exit.h"
#include "frontend_common/cloud_sync/save_archive.h"
#include "tests/common/restore_eden_path.h"

using namespace FrontendCommon::CloudSync;

namespace {

struct TempDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 fmt::format("lichii-cloud-sync-{}", std::random_device{}());
    TempDir() {
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream{path, std::ios::binary} << text;
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, {}};
}

void MakeSave(const std::filesystem::path& folder, std::string_view variant) {
    Write(folder / "save.dat", fmt::format("progress {}", variant));
    Write(folder / "slots" / "slot1.bin", std::string(5000, 'z'));
    std::filesystem::create_directories(folder / "empty");
}

std::vector<u8> Bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

std::size_t CountFiles(const std::filesystem::path& folder) {
    std::error_code ec;
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator{folder, ec},
                                                  std::filesystem::directory_iterator{}));
}

constexpr u64 TitleId = 0x0100152000022000;

} // Anonymous namespace

TEST_CASE("CloudSync: a packed save restores to the same files", "[cloud_sync]") {
    TempDir temp;
    const auto source = temp.path / "source";
    const auto target = temp.path / "nested" / "target";
    MakeSave(source, "a");

    const auto packed = PackSave(source);
    REQUIRE(packed.has_value());
    REQUIRE(packed->content_hash.size() == 64);
    REQUIRE(RestoreSave(target, packed->zip, temp.path / "backups"));

    REQUIRE(Read(target / "save.dat") == "progress a");
    REQUIRE(Read(target / "slots" / "slot1.bin") == std::string(5000, 'z'));
    REQUIRE(std::filesystem::is_directory(target / "empty"));
    REQUIRE(PackSave(target).value().content_hash == packed->content_hash);
    // Nothing was there to back up, and nothing is left beside the save.
    REQUIRE_FALSE(std::filesystem::exists(temp.path / "backups"));
    REQUIRE(CountFiles(temp.path / "nested") == 1);
    REQUIRE_FALSE(PackSave(temp.path / "missing").has_value());
}

TEST_CASE("CloudSync: a save with a symlink isn't packed", "[cloud_sync]") {
    TempDir temp;
    MakeSave(temp.path / "elsewhere", "a");
    MakeSave(temp.path / "save", "b");
    std::error_code ec;
    std::filesystem::create_directory_symlink(temp.path / "elsewhere",
                                              temp.path / "save" / "linked", ec);
    if (ec) {
        SKIP("Can't make a symlink here: " << ec.message());
    }
    REQUIRE_FALSE(PackSave(temp.path / "save").has_value());
}

TEST_CASE("CloudSync: the content hash ignores mtimes", "[cloud_sync]") {
    TempDir temp;
    MakeSave(temp.path / "one", "a");
    MakeSave(temp.path / "two", "a");
    const auto earlier = std::filesystem::file_time_type::clock::now() - std::chrono::hours{48};
    std::filesystem::last_write_time(temp.path / "two" / "save.dat", earlier);
    std::filesystem::last_write_time(temp.path / "two" / "slots", earlier);

    const auto one = PackSave(temp.path / "one");
    const auto two = PackSave(temp.path / "two");
    REQUIRE(one.has_value());
    REQUIRE(two.has_value());
    REQUIRE(one->content_hash == two->content_hash);
    REQUIRE(one->zip == two->zip);
    REQUIRE(ContentHash(ReadZip(one->zip).value()) == one->content_hash);

    MakeSave(temp.path / "two", "b");
    REQUIRE(PackSave(temp.path / "two").value().content_hash != one->content_hash);
    std::filesystem::create_directories(temp.path / "one" / "another");
    REQUIRE(PackSave(temp.path / "one").value().content_hash != one->content_hash);

    // The same entries in another order, and a name/data boundary moved.
    const std::vector<ZipEntry> ab{{"a", Bytes("1")}, {"b", Bytes("2")}};
    const std::vector<ZipEntry> ba{{"b", Bytes("2")}, {"a", Bytes("1")}};
    REQUIRE(ContentHash(ab) == ContentHash(ba));
    REQUIRE(ContentHash(std::vector<ZipEntry>{{"ab", Bytes("c")}}) !=
            ContentHash(std::vector<ZipEntry>{{"a", Bytes("bc")}}));
}

TEST_CASE("CloudSync: zip entries outside the save are refused", "[cloud_sync]") {
    TempDir temp;
    const auto folder = temp.path / "save";
    MakeSave(folder, "a");
    const auto before = PackSave(folder).value().content_hash;

    for (const char* name : {"../evil", "/evil", "ok/../../evil", "./evil", "a//evil", "C:evil",
                             "..\\evil", "a/../evil", "../"}) {
        INFO(name);
        const auto zip = WriteZip(std::vector<ZipEntry>{{"fine", Bytes("x")}, {name, Bytes("x")}});
        REQUIRE(zip.has_value());
        REQUIRE_FALSE(RestoreSave(folder, *zip, temp.path / "backups"));
    }
    // A NUL would cut the name short in the OS. minizip writes names up to their NUL, so it is
    // patched in afterwards.
    auto nul = WriteZip(std::vector<ZipEntry>{{"save.dat\x01"
                                               "evil",
                                               Bytes("x")}})
                   .value();
    const std::string_view marked{"save.dat\x01"
                                  "evil"};
    for (auto it = nul.begin();
         (it = std::search(it, nul.end(), marked.begin(), marked.end())) != nul.end();) {
        it[8] = 0;
    }
    REQUIRE(ReadZip(nul).value().at(0).name == std::string("save.dat\0evil", 13));
    REQUIRE_FALSE(RestoreSave(folder, nul, temp.path / "backups"));

    REQUIRE(PackSave(folder).value().content_hash == before);
    REQUIRE_FALSE(std::filesystem::exists(temp.path / "evil"));
    REQUIRE_FALSE(std::filesystem::exists(temp.path / "backups"));
    REQUIRE(CountFiles(temp.path) == 1);
}

TEST_CASE("CloudSync: zip entries for the same path are refused", "[cloud_sync]") {
    TempDir temp;
    const auto folder = temp.path / "save";
    MakeSave(folder, "a");
    const auto before = PackSave(folder).value().content_hash;

    const std::vector<std::vector<ZipEntry>> zips{
        {{"save.dat", Bytes("1")}, {"save.dat", Bytes("2")}},
        // One file on Windows and macOS.
        {{"Save.dat", Bytes("1")}, {"save.DAT", Bytes("2")}},
        {{"dir/", {}}, {"DIR", Bytes("2")}},
    };
    for (const auto& entries : zips) {
        INFO(entries[1].name);
        REQUIRE_FALSE(RestoreSave(folder, WriteZip(entries).value(), temp.path / "backups"));
    }
    REQUIRE(PackSave(folder).value().content_hash == before);
    REQUIRE(CountFiles(temp.path) == 1);
}

TEST_CASE("CloudSync: a save with two files for one restored path isn't packed", "[cloud_sync]") {
    TempDir temp;
    const auto folder = temp.path / "save";
    Write(folder / "Slot.dat", "1");
    Write(folder / "slot.dat", "2");
    if (CountFiles(folder) != 2) {
        SKIP("This filesystem ignores case");
    }
    REQUIRE_FALSE(PackSave(folder).has_value());
}

TEST_CASE("CloudSync: an extracted save must match its zip", "[cloud_sync]") {
    TempDir temp;
    const auto folder = temp.path / "save";
    Write(folder / "Dir" / "one", "1");
    Write(folder / "Dir" / "two", "22");

    // Parent folders may be left out of the zip.
    REQUIRE(FolderMatches(folder, std::vector<ZipEntry>{{"Dir/one", Bytes("1")},
                                                        {"Dir/two", Bytes("22")}}));
    REQUIRE(FolderMatches(folder, std::vector<ZipEntry>{{"Dir/", {}},
                                                        {"Dir/one", Bytes("1")},
                                                        {"Dir/two", Bytes("22")}}));
    // What a case-insensitive filesystem makes of "Dir/one" and "dir/two": one merged folder.
    REQUIRE_FALSE(FolderMatches(folder, std::vector<ZipEntry>{{"Dir/one", Bytes("1")},
                                                              {"dir/two", Bytes("22")}}));
    // Or of the folders "Ä/" and "ä/", which ASCII folding doesn't see.
    const auto unicode = temp.path / "unicode";
    Write(unicode / std::filesystem::path{u8"\u00c4"} / "one", "1");
    Write(unicode / std::filesystem::path{u8"\u00c4"} / "two", "2");
    REQUIRE_FALSE(FolderMatches(unicode, std::vector<ZipEntry>{{"\xC3\x84/", {}},
                                                               {"\xC3\xA4/", {}},
                                                               {"\xC3\x84/one", Bytes("1")},
                                                               {"\xC3\xA4/two", Bytes("2")}}));
    REQUIRE_FALSE(FolderMatches(folder, std::vector<ZipEntry>{{"Dir/one", Bytes("1")},
                                                              {"Dir/two", Bytes("2")}}));
    REQUIRE_FALSE(FolderMatches(folder, std::vector<ZipEntry>{{"Dir/one", Bytes("1")}}));
}

#ifndef _WIN32
TEST_CASE("CloudSync: a restore stops when an earlier one's leftovers can't be removed",
          "[cloud_sync]") {
    TempDir temp;
    const auto folder = temp.path / "save";
    MakeSave(folder, "a");
    const auto before = PackSave(folder).value().content_hash;
    const auto locked = temp.path / "save.restore" / "locked";
    Write(locked / "stale", "old");
    SCOPE_EXIT {
        std::filesystem::permissions(locked, std::filesystem::perms::owner_all);
    };
    std::filesystem::permissions(locked, std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_exec);
    if (std::ofstream{locked / "probe"}.is_open()) {
        SKIP("Folder permissions don't keep this runner from removing files");
    }

    MakeSave(temp.path / "drive", "new");
    REQUIRE_FALSE(
        RestoreSave(folder, PackSave(temp.path / "drive").value().zip, temp.path / "backups"));
    REQUIRE(PackSave(folder).value().content_hash == before);
    REQUIRE_FALSE(std::filesystem::exists(folder / "locked"));
}
#endif

TEST_CASE("CloudSync: a save an interrupted restore moved aside is moved back", "[cloud_sync]") {
    TempDir temp;
    const auto folder = temp.path / "save";
    const auto previous = temp.path / "save.previous";
    MakeSave(previous, "old");
    const auto old_save = PackSave(previous);
    REQUIRE(old_save.has_value());

    SECTION("even when the next restore fails") {
        REQUIRE_FALSE(RestoreSave(folder, Bytes("not a zip"), temp.path / "backups"));
        REQUIRE(PackSave(folder).value().content_hash == old_save->content_hash);
    }
    SECTION("and backed up before the next restore replaces it") {
        MakeSave(temp.path / "drive", "new");
        const auto new_save = PackSave(temp.path / "drive");
        REQUIRE(new_save.has_value());
        REQUIRE(RestoreSave(folder, new_save->zip, temp.path / "backups"));
        REQUIRE(PackSave(folder).value().content_hash == new_save->content_hash);
        const auto backup = std::filesystem::directory_iterator{temp.path / "backups"}->path();
        std::ifstream file{backup, std::ios::binary};
        const std::vector<u8> zip{std::istreambuf_iterator<char>{file}, {}};
        REQUIRE(ContentHash(ReadZip(zip).value()) == old_save->content_hash);
    }
    REQUIRE_FALSE(std::filesystem::exists(previous));
}

TEST_CASE("CloudSync: a restore keeps its backup when older ones are dated later",
          "[cloud_sync]") {
    TempDir temp;
    const auto folder = temp.path / "save";
    const auto backups = temp.path / "backups";
    MakeSave(folder, "old");
    const auto old_save = PackSave(folder);
    REQUIRE(old_save.has_value());
    // Made while the clock was ahead.
    for (std::size_t i = 0; i < BackupsKept; ++i) {
        Write(backups / fmt::format("29990101T00000{}.000000000Z.zip", i), "future");
    }
    MakeSave(temp.path / "drive", "new");
    REQUIRE(RestoreSave(folder, PackSave(temp.path / "drive").value().zip, backups));

    REQUIRE(CountFiles(backups) == BackupsKept);
    REQUIRE_FALSE(std::filesystem::exists(backups / "29990101T000000.000000000Z.zip"));
    bool found = false;
    for (const auto& entry : std::filesystem::directory_iterator{backups}) {
        if (!entry.path().filename().string().starts_with("2999")) {
            std::ifstream file{entry.path(), std::ios::binary};
            const std::vector<u8> zip{std::istreambuf_iterator<char>{file}, {}};
            found = ContentHash(ReadZip(zip).value()) == old_save->content_hash;
        }
    }
    REQUIRE(found);
}

TEST_CASE("CloudSync: a restore backs up the save it replaces", "[cloud_sync]") {
    TempDir temp;
    const auto folder = temp.path / "save";
    const auto backups = temp.path / "backups";
    MakeSave(folder, "old");
    const auto old_save = PackSave(folder);
    MakeSave(temp.path / "drive", "new");
    const auto new_save = PackSave(temp.path / "drive");
    REQUIRE(old_save.has_value());
    REQUIRE(new_save.has_value());

    SECTION("and leaves it as it was when the zip is unreadable") {
        REQUIRE_FALSE(RestoreSave(folder, Bytes("not a zip"), backups));
        auto truncated = new_save->zip;
        truncated.resize(truncated.size() / 2);
        REQUIRE_FALSE(RestoreSave(folder, truncated, backups));
        REQUIRE(PackSave(folder).value().content_hash == old_save->content_hash);
        REQUIRE_FALSE(std::filesystem::exists(backups));
    }
    SECTION("and keeps the last few backups") {
        REQUIRE(RestoreSave(folder, new_save->zip, backups));
        REQUIRE(PackSave(folder).value().content_hash == new_save->content_hash);
        REQUIRE(CountFiles(backups) == 1);
        const auto backup = std::filesystem::directory_iterator{backups}->path();
        // Read without keeping the file open: Windows can't remove it when the restores rotate it.
        REQUIRE(ContentHash(ReadZip(Bytes(Read(backup))).value()) == old_save->content_hash);

        for (std::size_t i = 0; i < BackupsKept + 2; ++i) {
            REQUIRE(RestoreSave(folder, (i % 2 ? new_save : old_save)->zip, backups));
        }
        REQUIRE(CountFiles(backups) == BackupsKept);
        // The last restore put the old save back; the newest backup is the save it replaced.
        std::vector<std::filesystem::path> names;
        for (const auto& entry : std::filesystem::directory_iterator{backups}) {
            names.push_back(entry.path());
        }
        std::ranges::sort(names);
        std::ifstream newest{names.back(), std::ios::binary};
        const std::vector<u8> newest_zip{std::istreambuf_iterator<char>{newest}, {}};
        REQUIRE(ContentHash(ReadZip(newest_zip).value()) == new_save->content_hash);
        REQUIRE(PackSave(folder).value().content_hash == old_save->content_hash);
    }
    REQUIRE(CountFiles(temp.path) == std::size_t{2} + std::filesystem::exists(backups));
}

TEST_CASE("CloudSync: local saves are found where the save data factory puts them",
          "[cloud_sync]") {
    TempDir temp;
    const auto previous_save = Common::FS::GetEdenPath(Common::FS::EdenPath::SaveDir);
    const auto previous_config = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir);
    SCOPE_EXIT {
        Tests::RestoreEdenPath(Common::FS::EdenPath::SaveDir, previous_save);
        Tests::RestoreEdenPath(Common::FS::EdenPath::ConfigDir, previous_config);
    };
    std::filesystem::create_directories(temp.path / "nand");
    std::filesystem::create_directories(temp.path / "config");
    Common::FS::SetEdenPath(Common::FS::EdenPath::SaveDir, temp.path / "nand");
    Common::FS::SetEdenPath(Common::FS::EdenPath::ConfigDir, temp.path / "config");

    const auto user = Common::UUID::MakeRandomWithSeed(7);
    const u128 id = user.AsU128();
    const SaveId account{.title_id = TitleId, .user = user};
    const SaveId device{.title_id = TitleId};
    const auto save = temp.path / "nand" / "user" / "save";

    REQUIRE(LocalSavePath(account) ==
            save / "0000000000000000" / fmt::format("{:016X}{:016X}", id[1], id[0]) /
                "0100152000022000");
    REQUIRE(LocalSavePath(device) ==
            save / "0000000000000000" / std::string(32, '0') / "0100152000022000");

    const auto future_account = save / "account" / user.RawString() / "0100152000022000" / "0";
    const auto future_device = save / "device" / "0100152000022000" / "0";
    // A restore into the future layout cut short mid-swap: the save is moved back and chosen.
    for (const auto& future : {future_account, future_device}) {
        auto previous = future;
        previous += ".previous";
        std::filesystem::create_directories(previous);
        std::ofstream{previous / "save.dat"} << "moved aside";
    }
    REQUIRE(LocalSavePath(account) == future_account);
    REQUIRE(LocalSavePath(device) == future_device);
    for (const auto& future : {future_account, future_device}) {
        auto previous = future;
        previous += ".previous";
        REQUIRE_FALSE(std::filesystem::exists(previous));
        std::ifstream file{future / "save.dat"};
        REQUIRE(std::string{std::istreambuf_iterator<char>{file}, {}} == "moved aside");
    }
    std::filesystem::create_directories(future_account);
    std::filesystem::create_directories(future_device);
    REQUIRE(LocalSavePath(account) == future_account);
    REQUIRE(LocalSavePath(device) == future_device);

    const auto backups = temp.path / "config" / "cloud_sync" / "backups" / "0100152000022000";
    REQUIRE(BackupFolder(account) == backups / user.FormattedString());
    REQUIRE(BackupFolder(device) == backups / "device");
}
