// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "common/common_types.h"
#include "common/scope_exit.h"
#include "frontend_common/steam_shortcuts.h"

namespace {

// Writes binary VDF the way Steam stores it, independently of the code under test.
class Vdf {
public:
    Vdf& Map(std::string_view key) {
        bytes.push_back(0x00);
        Key(key);
        return *this;
    }
    Vdf& String(std::string_view key, std::string_view value) {
        bytes.push_back(0x01);
        Key(key);
        Key(value);
        return *this;
    }
    Vdf& Int(std::string_view key, u32 value) {
        bytes.push_back(0x02);
        Key(key);
        for (int shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<u8>(value >> shift));
        }
        return *this;
    }
    Vdf& Raw(std::vector<u8> raw) {
        bytes.insert(bytes.end(), raw.begin(), raw.end());
        return *this;
    }
    Vdf& End() {
        bytes.push_back(0x08);
        return *this;
    }
    Vdf& Append(const Vdf& other) {
        bytes.insert(bytes.end(), other.bytes.begin(), other.bytes.end());
        return *this;
    }

    std::vector<u8> bytes;

private:
    void Key(std::string_view text) {
        bytes.insert(bytes.end(), text.begin(), text.end());
        bytes.push_back(0);
    }
};

const Steam::Shortcut Game{
    .app_name = "Test Game",
    .exe = R"(C:\Games\Lichii\lichii.exe)",
    .start_dir = R"(C:\Games\Lichii)",
    .icon = R"(C:\Games\icons\test.ico)",
    .launch_options = R"(-g "D:\Switch\Test Game.nsp")",
};

// CRC-32 of "\"C:\\Games\\Lichii\\lichii.exe\"Test Game" with the top bit set (zlib.crc32).
constexpr u32 GameAppId = 0xF347E0A3;

// The entry Steam's own "Add a Non-Steam Game" writes, filled in for Game.
Vdf GameEntry(std::string_view index, std::string_view name = Game.app_name,
              u32 app_id = GameAppId) {
    Vdf entry;
    entry.Map(index)
        .Int("appid", app_id)
        .String("AppName", name)
        .String("Exe", R"("C:\Games\Lichii\lichii.exe")")
        .String("StartDir", R"("C:\Games\Lichii")")
        .String("icon", Game.icon)
        .String("ShortcutPath", "")
        .String("LaunchOptions", Game.launch_options)
        .Int("IsHidden", 0)
        .Int("AllowDesktopConfig", 1)
        .Int("AllowOverlay", 1)
        .Int("OpenVR", 0)
        .Int("Devkit", 0)
        .String("DevkitGameID", "")
        .Int("DevkitOverrideAppID", 0)
        .Int("LastPlayTime", 0)
        .String("FlatpakAppID", "")
        .String("sortas", "")
        .Map("tags")
        .End()
        .End();
    return entry;
}

// Another program's shortcut, with fields Lichii doesn't write and a type it doesn't use.
Vdf OtherEntry(std::string_view index) {
    Vdf entry;
    entry.Map(index)
        .Int("appid", 0x81234567)
        .String("AppName", "Other Program")
        .String("Exe", R"("C:\Other\other.exe")")
        .String("LaunchOptions", "")
        .Int("LastPlayTime", 1767225600)
        .Raw({0x07, 'u', '6', '4', 0, 1, 2, 3, 4, 5, 6, 7, 8})
        .Map("tags")
        .String("0", "Favorites")
        .End()
        .End();
    return entry;
}

std::vector<u8> File(const Vdf& entries) {
    Vdf file;
    file.Map("shortcuts").Append(entries).End().End();
    return file.bytes;
}

TEST_CASE("Steam shortcut ids match Steam's CRC-32 of the quoted exe and name",
          "[frontend_common]") {
    CHECK(Steam::ShortcutAppId(R"("C:\Games\Lichii\lichii.exe")", "Test Game") == GameAppId);
}

TEST_CASE("Adding a Steam shortcut to a missing file writes Steam's own entry",
          "[frontend_common]") {
    bool replaced = true;
    const auto file = Steam::AddToShortcutsFile({}, Game, &replaced);
    REQUIRE(file.has_value());
    CHECK_FALSE(replaced);
    CHECK(*file == File(GameEntry("0")));
}

TEST_CASE("Adding a Steam shortcut keeps every other entry byte for byte", "[frontend_common]") {
    Vdf existing = OtherEntry("0");
    existing.Append(OtherEntry("4"));
    bool replaced = true;
    const auto file = Steam::AddToShortcutsFile(File(existing), Game, &replaced);
    REQUIRE(file.has_value());
    CHECK_FALSE(replaced);
    // The new entry goes after the highest index, so Steam's numbering stays unique.
    CHECK(*file == File(Vdf{existing}.Append(GameEntry("5"))));
}

TEST_CASE("Adding the same Steam shortcut again updates it instead of adding a copy",
          "[frontend_common]") {
    const auto first = Steam::AddToShortcutsFile(File(OtherEntry("0")), Game);
    REQUIRE(first.has_value());

    Steam::Shortcut renamed = Game;
    renamed.app_name = "Renamed Game";
    bool replaced = false;
    const auto second = Steam::AddToShortcutsFile(*first, renamed, &replaced);
    REQUIRE(second.has_value());
    CHECK(replaced);
    const u32 renamed_id = Steam::ShortcutAppId(R"("C:\Games\Lichii\lichii.exe")", "Renamed Game");
    CHECK(*second ==
          File(Vdf{OtherEntry("0")}.Append(GameEntry("1", "Renamed Game", renamed_id))));
}

TEST_CASE("The same program with other launch options is a separate Steam shortcut",
          "[frontend_common]") {
    const auto first = Steam::AddToShortcutsFile({}, Game);
    REQUIRE(first.has_value());
    Steam::Shortcut fullscreen = Game;
    fullscreen.launch_options = "-f " + Game.launch_options;
    bool replaced = true;
    const auto second = Steam::AddToShortcutsFile(*first, fullscreen, &replaced);
    REQUIRE(second.has_value());
    CHECK_FALSE(replaced);
    CHECK(second->size() > first->size());
}

TEST_CASE("A file that isn't a Steam shortcuts file is left alone", "[frontend_common]") {
    // Cut off in the middle of an entry.
    auto truncated = File(OtherEntry("0"));
    truncated.resize(truncated.size() - 6);
    CHECK_FALSE(Steam::AddToShortcutsFile(truncated, Game).has_value());

    // Bytes after the end of the file.
    auto trailing = File(OtherEntry("0"));
    trailing.push_back(0x08);
    CHECK_FALSE(Steam::AddToShortcutsFile(trailing, Game).has_value());

    // Some other binary VDF.
    Vdf other;
    other.Map("appinfo").Int("version", 1).End().End();
    CHECK_FALSE(Steam::AddToShortcutsFile(other.bytes, Game).has_value());
}

TEST_CASE("The Steam account marked MostRecent wins, whatever its display name holds",
          "[frontend_common]") {
    const auto root = std::filesystem::temp_directory_path() /
                      fmt::format("lichii-steam-{}", std::random_device{}());
    SCOPE_EXIT {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    };
    const auto write = [](const std::filesystem::path& path, std::string_view text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream{path, std::ios::binary} << text;
    };
    // Account 222 saved its settings last, so it is the fallback when nothing is marked.
    const auto older = root / "userdata" / "111" / "config" / "localconfig.vdf";
    const auto newer = root / "userdata" / "222" / "config" / "localconfig.vdf";
    write(older, "");
    write(newer, "");
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(older, now - std::chrono::hours{1});
    std::filesystem::last_write_time(newer, now);

    // Account 111 (SteamID64 76561197960265839) is marked, and its name escapes quotes and a
    // backslash, so reading up to the next quote would lose track of the keys after it.
    write(root / "config" / "loginusers.vdf", R"("users"
{
    "76561197960265839"
    {
        "AccountName"    "first"
        "PersonaName"    "say \"hi\" \\"
        "MostRecent"     "1"
    }
    "76561197960265950"
    {
        "AccountName"    "second"
        "PersonaName"    "second"
        "MostRecent"     "0"
    }
}
)");
    const auto config = Steam::FindAccountConfigDir(root);
    REQUIRE(config.has_value());
    CHECK(*config == root / "userdata" / "111" / "config");
}

} // Anonymous namespace
