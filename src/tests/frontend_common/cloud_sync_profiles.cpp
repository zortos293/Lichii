// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/scope_exit.h"
#include "frontend_common/cloud_sync/profile_sync.h"
#include "frontend_common/cloud_sync/save_archive.h"
#include "frontend_common/cloud_sync/sync_engine.h"
#include "tests/frontend_common/cloud_sync_machine.h"
#include "tests/frontend_common/fake_drive.h"

using namespace FrontendCommon::CloudSync;
using Tests::CloudSyncMachine;
using Tests::FakeDriveServer;

namespace {

constexpr u64 TitleId = 0x0100152000022000;
const Title Game{TitleId, "Mario Kart 8 Deluxe"};

std::filesystem::path AvatarPath(const CloudSyncMachine& machine, const Common::UUID& user) {
    return machine.nand / "system/save/8000000000000010/su/avators" /
           fmt::format("{}.jpg", user.FormattedString());
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, {}};
}

SyncCallbacks FailOnConflict() {
    return {.resolve_conflict = [](const Conflict&, std::stop_token) {
        FAIL("unexpected conflict");
        return ConflictChoice::Skip;
    }};
}

// Changes profile.json on Drive as another machine's upload would.
template <typename Change>
void EditProfile(FakeDriveServer& drive, const std::string& id, Change&& change) {
    drive.Edit(id, [&](FakeDriveServer::File& file) {
        auto profile = nlohmann::json::parse(file.data);
        change(profile);
        file.data = profile.dump();
        const ZipEntry entry{"profile.json", {file.data.begin(), file.data.end()}};
        file.app_properties["contentHash"] = ContentHash({&entry, 1});
        ++file.revisions;
    });
}

std::string AvatarHash(std::string_view avatar) {
    const ZipEntry entry{"avatar.jpg", {avatar.begin(), avatar.end()}};
    return ContentHash({&entry, 1});
}

FakeDriveServer::File ProfileJson(FakeDriveServer& drive, const Common::UUID& user) {
    return drive.Find({{"lichii", "profileJson"}, {"userUuid", user.FormattedString()}}).at(0);
}

// Another machine: its own NAND, and no sync state.
void ForgetSyncState(FakeDriveServer& drive) {
    std::filesystem::remove(drive.google.directory / "cloud_sync" / "state.json");
}

} // Anonymous namespace

TEST_CASE("CloudSync: a profile and its saves reach another machine with the same uuid",
          "[cloud_sync]") {
    FakeDriveServer drive;
    Common::UUID user;
    {
        CloudSyncMachine machine;
        user = machine.FirstUser();
        Service::Account::ProfileBase base{};
        Service::Account::UserData data{};
        REQUIRE(machine.profiles->GetProfileBaseAndData(user, base, data));
        base.timestamp = 1234;
        data.icon_id = 7;
        data.bg_color_id = 3;
        REQUIRE(machine.profiles->SetProfileBaseAndData(user, base, data));
        std::filesystem::create_directories(AvatarPath(machine, user).parent_path());
        std::ofstream{AvatarPath(machine, user), std::ios::binary} << "jpeg bytes";
        machine.WriteSave({TitleId, user}, "progress");

        const auto result = SyncAll(*machine.profiles, {&Game, 1}, FailOnConflict(), {});
        REQUIRE(result.errors.empty());
        REQUIRE(result.pushed == 2);
    }

    const auto folder = drive.Find({{"lichii", "profile"}, {"userUuid", user.FormattedString()}});
    REQUIRE(folder.size() == 1);
    REQUIRE(folder[0].name == "Eden");
    REQUIRE(folder[0].parents == std::vector{drive.Find({{"lichii", "profiles"}}).at(0).id});
    const auto json = drive.Find({{"lichii", "profileJson"}});
    REQUIRE(json.size() == 1);
    REQUIRE(json[0].name == "profile.json");
    REQUIRE(json[0].parents == std::vector{folder[0].id});
    REQUIRE(nlohmann::json::parse(json[0].data).at("uuid") == user.FormattedString());
    const auto avatar = drive.Find({{"lichii", "avatar"}});
    REQUIRE(avatar.size() == 1);
    REQUIRE(avatar[0].name == "avatar.jpg");
    REQUIRE(avatar[0].data == "jpeg bytes");

    ForgetSyncState(drive);
    CloudSyncMachine other;
    const auto own = other.FirstUser();
    REQUIRE(own != user);
    const auto result = SyncAll(*other.profiles, {&Game, 1}, FailOnConflict(), {});
    REQUIRE(result.errors.empty());
    // The profile and its save came in; this machine's own profile went up.
    REQUIRE(result.pulled == 2);
    REQUIRE(result.pushed == 1);

    REQUIRE(other.profiles->UserExists(own));
    Service::Account::ProfileBase base{};
    Service::Account::UserData data{};
    REQUIRE(other.profiles->GetProfileBaseAndData(user, base, data));
    REQUIRE(ProfileNickname(*other.profiles, user) == "Eden");
    REQUIRE(base.timestamp == 1234);
    REQUIRE(data.icon_id == 7);
    REQUIRE(data.bg_color_id == 3);
    REQUIRE(ReadText(AvatarPath(other, user)) == "jpeg bytes");
    REQUIRE(other.ReadSave({TitleId, user}) == "progress");

    // profiles.dat has the new profile.
    Service::Account::ProfileManager reloaded;
    REQUIRE(reloaded.UserExists(user));
    REQUIRE(reloaded.UserExists(own));

    REQUIRE(drive.Find({{"lichii", "profileJson"}}).size() == 2);
    const auto again = SyncAll(*other.profiles, {&Game, 1}, FailOnConflict(), {});
    REQUIRE(again.pushed + again.pulled == 0);
}

TEST_CASE("CloudSync: a profile linked to a Drive profile syncs as it", "[cloud_sync]") {
    FakeDriveServer drive;
    Common::UUID remote;
    {
        CloudSyncMachine machine;
        remote = machine.FirstUser();
        machine.WriteSave({TitleId, remote}, "from the first machine");
        REQUIRE(SyncAll(*machine.profiles, {&Game, 1}, FailOnConflict(), {}).pushed == 2);
    }

    ForgetSyncState(drive);
    CloudSyncMachine other;
    const auto local = other.FirstUser();
    const auto listed = ListRemoteProfiles();
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    REQUIRE(listed->at(0).uuid == remote);
    REQUIRE(listed->at(0).nickname == "Eden");

    REQUIRE(LinkProfile(*other.profiles, local, remote));
    REQUIRE(LinkedProfile(local) == remote);
    // Two local profiles can't sync as one.
    const auto second = Common::UUID::MakeRandomWithSeed(5);
    REQUIRE(other.profiles->CreateNewUser(second, "Second").IsSuccess());
    REQUIRE(!LinkProfile(*other.profiles, second, remote));

    auto result = SyncAll(*other.profiles, {&Game, 1}, FailOnConflict(), {});
    REQUIRE(result.errors.empty());
    // The save was pulled into the local profile's folder; only "Second" was added to Drive.
    REQUIRE(result.pulled == 1);
    REQUIRE(result.pushed == 1);
    REQUIRE(!other.profiles->UserExists(remote));
    REQUIRE(other.ReadSave({TitleId, local}) == "from the first machine");

    other.WriteSave({TitleId, local}, "from the second machine");
    result = PushTitle(*other.profiles, Game, FailOnConflict(), {});
    REQUIRE(result.pushed == 1);
    const auto saves = drive.Find({{"lichii", "save"}});
    REQUIRE(saves.size() == 1);
    REQUIRE(saves[0].app_properties.at("userUuid") == remote.FormattedString());
    REQUIRE(saves[0].revisions == 1);

    REQUIRE(LinkProfile(*other.profiles, local, std::nullopt));
    REQUIRE(!LinkedProfile(local).has_value());
}

TEST_CASE("CloudSync: a linked profile's own Drive profile isn't imported", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const auto user = machine.FirstUser();
    const auto second = Common::UUID::MakeRandomWithSeed(5);
    REQUIRE(machine.profiles->CreateNewUser(second, "Second").IsSuccess());
    REQUIRE(SyncAll(*machine.profiles, {}, FailOnConflict(), {}).pushed == 2);
    REQUIRE(machine.profiles->RemoveUser(second));

    // Drive still has the profile as itself.
    REQUIRE(LinkProfile(*machine.profiles, user, second));
    std::vector<Conflict> asked;
    const SyncCallbacks keep_remote{
        .resolve_conflict = [&asked](const Conflict& conflict, std::stop_token) {
            asked.push_back(conflict);
            return ConflictChoice::KeepRemote;
        }};
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto result = SyncAll(*machine.profiles, {}, keep_remote, {});
        REQUIRE(result.errors.empty());
        REQUIRE(machine.profiles->GetUserCount() == 1);
    }
    REQUIRE(asked.size() == 1);
    REQUIRE(ProfileNickname(*machine.profiles, user) == "Second");
    REQUIRE(drive.Find({{"lichii", "profileJson"}}).size() == 2);
}

TEST_CASE("CloudSync: a removed profile's link doesn't pair the profile imported in its place",
          "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const auto second = Common::UUID::MakeRandomWithSeed(5);
    const auto third = Common::UUID::MakeRandomWithSeed(6);
    REQUIRE(machine.profiles->CreateNewUser(second, "Second").IsSuccess());
    REQUIRE(machine.profiles->CreateNewUser(third, "Third").IsSuccess());
    REQUIRE(SyncAll(*machine.profiles, {}, FailOnConflict(), {}).pushed == 3);
    REQUIRE(machine.profiles->RemoveUser(second));
    REQUIRE(LinkProfile(*machine.profiles, third, second));
    // Removed outside the frontend, which leaves the link.
    REQUIRE(machine.profiles->RemoveUser(third));

    const auto result = SyncAll(*machine.profiles, {}, FailOnConflict(), {});
    REQUIRE(result.errors.empty());
    REQUIRE(result.pulled == 2);
    REQUIRE(machine.profiles->UserExists(second));
    REQUIRE(machine.profiles->UserExists(third));
    REQUIRE(!LinkedProfile(third).has_value());
    const auto again = SyncAll(*machine.profiles, {}, FailOnConflict(), {});
    REQUIRE(again.errors.empty());
    REQUIRE(again.pushed + again.pulled == 0);
}

TEST_CASE("CloudSync: pulling profiles changes them but deletes nothing", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const auto user = machine.FirstUser();
    const auto second = Common::UUID::MakeRandomWithSeed(5);
    REQUIRE(machine.profiles->CreateNewUser(second, "Second").IsSuccess());
    REQUIRE(SyncAll(*machine.profiles, {}, FailOnConflict(), {}).pushed == 2);

    const auto json =
        drive.Find({{"lichii", "profileJson"}, {"userUuid", user.FormattedString()}}).at(0);
    EditProfile(drive, json.id, [](nlohmann::json& profile) { profile["nickname"] = "Renamed"; });
    const auto other =
        drive.Find({{"lichii", "profileJson"}, {"userUuid", second.FormattedString()}}).at(0);
    drive.Edit(other.id, [](FakeDriveServer::File& file) { file.trashed = true; });

    const auto result = SyncAll(*machine.profiles, {}, FailOnConflict(), {});
    REQUIRE(result.errors.empty());
    REQUIRE(result.pulled == 1);
    // The profile gone from Drive stays here, and goes up again.
    REQUIRE(result.pushed == 1);
    REQUIRE(machine.profiles->GetUserCount() == 2);
    REQUIRE(ProfileNickname(*machine.profiles, user) == "Renamed");
    REQUIRE(machine.profiles->UserExists(second));
}

TEST_CASE("CloudSync: profile.json names the avatar it was published with", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const auto user = machine.FirstUser();
    std::filesystem::create_directories(AvatarPath(machine, user).parent_path());
    std::ofstream{AvatarPath(machine, user), std::ios::binary} << "first avatar";
    REQUIRE(SyncAll(*machine.profiles, {}, FailOnConflict(), {}).pushed == 1);
    const auto json = ProfileJson(drive, user);
    REQUIRE(nlohmann::json::parse(json.data).at("avatarHash") == AvatarHash("first avatar"));
    const auto avatar = drive.Find({{"lichii", "avatar"}}).at(0);

    // An upload cut short after the avatar: profile.json still names the old one.
    drive.Edit(avatar.id, [](FakeDriveServer::File& file) { file.data = "half uploaded"; });
    EditProfile(drive, json.id, [](nlohmann::json& profile) { profile["nickname"] = "Renamed"; });
    auto result = SyncAll(*machine.profiles, {}, FailOnConflict(), {});
    REQUIRE(result.errors.size() == 1);
    REQUIRE(result.pulled == 0);
    REQUIRE(ProfileNickname(*machine.profiles, user) == "Eden");
    REQUIRE(ReadText(AvatarPath(machine, user)) == "first avatar");

    // Once the avatar it names is there, the profile comes in.
    drive.Edit(avatar.id, [](FakeDriveServer::File& file) { file.data = "second avatar"; });
    EditProfile(drive, json.id, [](nlohmann::json& profile) {
        profile["avatarHash"] = AvatarHash("second avatar");
    });
    result = SyncAll(*machine.profiles, {}, FailOnConflict(), {});
    REQUIRE(result.errors.empty());
    REQUIRE(result.pulled == 1);
    REQUIRE(ProfileNickname(*machine.profiles, user) == "Renamed");
    REQUIRE(ReadText(AvatarPath(machine, user)) == "second avatar");

    // A Drive profile without an avatar leaves the local one, which then goes up again.
    EditProfile(drive, json.id, [](nlohmann::json& profile) { profile["avatarHash"] = ""; });
    result = SyncAll(*machine.profiles, {}, FailOnConflict(), {});
    REQUIRE(result.pulled == 1);
    REQUIRE(ReadText(AvatarPath(machine, user)) == "second avatar");
    result = SyncAll(*machine.profiles, {}, FailOnConflict(), {});
    REQUIRE(result.pushed == 1);
    REQUIRE(nlohmann::json::parse(ProfileJson(drive, user).data).at("avatarHash") ==
            AvatarHash("second avatar"));
}

TEST_CASE("CloudSync: a changed avatar leaves the published one until profile.json names it",
          "[cloud_sync]") {
    FakeDriveServer drive;
    Common::UUID user;
    {
        CloudSyncMachine machine;
        user = machine.FirstUser();
        std::filesystem::create_directories(AvatarPath(machine, user).parent_path());
        std::ofstream{AvatarPath(machine, user), std::ios::binary} << "first avatar";
        REQUIRE(SyncAll(*machine.profiles, {}, FailOnConflict(), {}).pushed == 1);

        // The new avatar goes up, profile.json doesn't.
        std::ofstream{AvatarPath(machine, user), std::ios::binary} << "second avatar";
        drive.update_status = 403;
        const auto result = SyncAll(*machine.profiles, {}, FailOnConflict(), {});
        drive.update_status = 0;
        REQUIRE(result.errors.size() == 1);
        REQUIRE(result.pushed == 0);
        REQUIRE(drive.Find({{"lichii", "avatar"}}).size() == 2);
    }

    ForgetSyncState(drive);
    CloudSyncMachine other;
    const auto result = SyncAll(*other.profiles, {}, FailOnConflict(), {});
    REQUIRE(result.errors.empty());
    REQUIRE(result.pulled == 1);
    REQUIRE(ReadText(AvatarPath(other, user)) == "first avatar");
}

TEST_CASE("CloudSync: a profile.json changed during the avatar upload is decided on again",
          "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const auto user = machine.FirstUser();
    std::filesystem::create_directories(AvatarPath(machine, user).parent_path());
    std::ofstream{AvatarPath(machine, user), std::ios::binary} << "first avatar";
    REQUIRE(SyncAll(*machine.profiles, {}, FailOnConflict(), {}).pushed == 1);

    // Another machine publishes while the new avatar goes up: the read before the update sees it.
    std::ofstream{AvatarPath(machine, user), std::ios::binary} << "second avatar";
    const std::string id = ProfileJson(drive, user).id;
    bool edited = false;
    drive.before_get = [&](const std::string& file) {
        if (file == id && drive.Find({{"lichii", "avatar"}}).size() == 2 &&
            !std::exchange(edited, true)) {
            EditProfile(drive, id, [](nlohmann::json& profile) { profile["nickname"] = "Remote"; });
        }
    };
    std::vector<Conflict> asked;
    const SyncCallbacks keep_local{
        .resolve_conflict = [&asked](const Conflict& conflict, std::stop_token) {
            asked.push_back(conflict);
            return ConflictChoice::KeepLocal;
        }};
    const auto result = SyncAll(*machine.profiles, {}, keep_local, {});
    REQUIRE(result.errors.empty());
    REQUIRE(result.pushed == 1);
    REQUIRE(asked.size() == 1);
    // The avatar uploaded before the retry is the one profile.json names.
    REQUIRE(drive.Find({{"lichii", "avatar"}}).size() == 2);
    const auto profile = nlohmann::json::parse(ProfileJson(drive, user).data);
    REQUIRE(profile.at("nickname") == "Eden");
    REQUIRE(profile.at("avatarHash") == AvatarHash("second avatar"));
}

TEST_CASE("CloudSync: keeping Drive's profile backs up the local one", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const auto user = machine.FirstUser();
    std::filesystem::create_directories(AvatarPath(machine, user).parent_path());
    std::ofstream{AvatarPath(machine, user), std::ios::binary} << "local avatar";
    REQUIRE(SyncAll(*machine.profiles, {}, FailOnConflict(), {}).pushed == 1);

    // Changed on both sides.
    EditProfile(drive, ProfileJson(drive, user).id,
                [](nlohmann::json& profile) { profile["nickname"] = "Remote"; });
    Service::Account::ProfileBase base{};
    REQUIRE(machine.profiles->GetProfileBase(user, base));
    base.username = {'L', 'o', 'c', 'a', 'l'};
    REQUIRE(machine.profiles->SetProfileBase(user, base));

    std::vector<Conflict> asked;
    const SyncCallbacks keep_remote{
        .resolve_conflict = [&asked](const Conflict& conflict, std::stop_token) {
            asked.push_back(conflict);
            return ConflictChoice::KeepRemote;
        }};
    const auto result = SyncAll(*machine.profiles, {}, keep_remote, {});
    REQUIRE(result.errors.empty());
    REQUIRE(result.pulled == 1);
    REQUIRE(asked.size() == 1);
    REQUIRE(asked[0].title_id == 0);
    REQUIRE(asked[0].nickname == "Local");
    REQUIRE(ProfileNickname(*machine.profiles, user) == "Remote");

    const auto backups =
        drive.google.directory / "cloud_sync" / "backups" / "profiles" / user.FormattedString();
    const auto backup = std::filesystem::directory_iterator{backups}->path();
    REQUIRE(nlohmann::json::parse(ReadText(backup / "profile.json")).at("nickname") == "Local");
    REQUIRE(ReadText(backup / "avatar.jpg") == "local avatar");
}

TEST_CASE("CloudSync: a profile pull profiles.dat can't take changes nothing", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const auto user = machine.FirstUser();
    REQUIRE(SyncAll(*machine.profiles, {}, FailOnConflict(), {}).pushed == 1);
    const auto json = ProfileJson(drive, user);
    const auto base_hash = json.app_properties.at("contentHash");
    EditProfile(drive, json.id, [](nlohmann::json& profile) { profile["nickname"] = "Remote"; });

    // profiles.dat can't be written while a folder takes its place.
    const auto profiles_dat = machine.nand / "system/save/8000000000000010/su/avators/profiles.dat";
    std::filesystem::remove(profiles_dat);
    std::filesystem::create_directory(profiles_dat);
    auto result = SyncAll(*machine.profiles, {}, FailOnConflict(), {});
    REQUIRE(result.errors.size() == 1);
    REQUIRE(result.pulled == 0);
    REQUIRE(ProfileNickname(*machine.profiles, user) == "Eden");

    // With profiles.dat back, the next sync still sees Drive's change, not a local one.
    std::filesystem::remove(profiles_dat);
    REQUIRE(machine.profiles->WriteUserSaveFile());
    result = SyncAll(*machine.profiles, {}, FailOnConflict(), {});
    REQUIRE(result.errors.empty());
    REQUIRE(result.pulled == 1);
    REQUIRE(result.pushed == 0);
    Service::Account::ProfileManager reloaded;
    REQUIRE(ProfileNickname(reloaded, user) == "Remote");
    REQUIRE(ProfileJson(drive, user).app_properties.at("contentHash") != base_hash);
}

TEST_CASE("CloudSync: a new link starts without the old pairing's bases", "[cloud_sync]") {
    FakeDriveServer drive;
    Common::UUID remote;
    {
        CloudSyncMachine machine;
        remote = machine.FirstUser();
        machine.WriteSave({TitleId, remote}, "remote save");
        REQUIRE(SyncAll(*machine.profiles, {&Game, 1}, FailOnConflict(), {}).pushed == 2);
    }

    ForgetSyncState(drive);
    CloudSyncMachine other;
    const auto first = other.FirstUser();
    REQUIRE(LinkProfile(*other.profiles, first, remote));
    REQUIRE(SyncAll(*other.profiles, {&Game, 1}, FailOnConflict(), {}).pulled == 1);
    REQUIRE(LinkProfile(*other.profiles, first, std::nullopt));

    const auto second = Common::UUID::MakeRandomWithSeed(5);
    REQUIRE(other.profiles->CreateNewUser(second, "Eden").IsSuccess());
    other.WriteSave({TitleId, second}, "another save");
    REQUIRE(LinkProfile(*other.profiles, second, remote));
    std::vector<Conflict> asked;
    const SyncCallbacks skip{
        .resolve_conflict = [&asked](const Conflict& conflict, std::stop_token) {
            asked.push_back(conflict);
            return ConflictChoice::Skip;
        }};
    const auto result = PushTitle(*other.profiles, Game, skip, {});
    // Only the first profile's save, now under its own uuid.
    REQUIRE(result.pushed == 1);
    REQUIRE(asked.size() == 1);
    REQUIRE(asked[0].title_id == TitleId);
    const auto save = drive.Find({{"lichii", "save"}, {"userUuid", remote.FormattedString()}});
    REQUIRE(save.at(0).revisions == 0);
}

TEST_CASE("CloudSync: profiles that changed the same way become the base", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const auto user = machine.FirstUser();
    REQUIRE(SyncAll(*machine.profiles, {}, FailOnConflict(), {}).pushed == 1);
    const std::string id = ProfileJson(drive, user).id;
    const auto rename = [&](std::string_view nickname) {
        Service::Account::ProfileBase base{};
        REQUIRE(machine.profiles->GetProfileBase(user, base));
        base.username.fill(0);
        std::copy(nickname.begin(), nickname.end(), base.username.begin());
        REQUIRE(machine.profiles->SetProfileBase(user, base));
    };
    rename("B");
    EditProfile(drive, id, [](nlohmann::json& profile) { profile["nickname"] = "B"; });
    auto result = SyncAll(*machine.profiles, {}, FailOnConflict(), {});
    REQUIRE(result.pushed + result.pulled == 0);

    SECTION("Drive back to A: pulled") {
        EditProfile(drive, id, [](nlohmann::json& profile) { profile["nickname"] = "Eden"; });
        result = SyncAll(*machine.profiles, {}, FailOnConflict(), {});
        REQUIRE(result.pulled == 1);
        REQUIRE(ProfileNickname(*machine.profiles, user) == "Eden");
    }
    SECTION("Here back to A: pushed") {
        rename("Eden");
        result = SyncAll(*machine.profiles, {}, FailOnConflict(), {});
        REQUIRE(result.pushed == 1);
        REQUIRE(nlohmann::json::parse(ProfileJson(drive, user).data).at("nickname") == "Eden");
    }
}

TEST_CASE("CloudSync: failed profile imports leave the profiles as they were", "[cloud_sync]") {
    FakeDriveServer drive;
    Common::UUID remote;
    {
        CloudSyncMachine machine;
        remote = machine.FirstUser();
        REQUIRE(SyncAll(*machine.profiles, {}, FailOnConflict(), {}).pushed == 1);
    }
    ForgetSyncState(drive);
    CloudSyncMachine other;
    const auto users = other.profiles->GetAllUsers();
    const auto profiles_dat = other.nand / "system/save/8000000000000010/su/avators/profiles.dat";
    std::filesystem::remove(profiles_dat);
    std::filesystem::create_directory(profiles_dat);
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto result = SyncAll(*other.profiles, {}, FailOnConflict(), {});
        REQUIRE(result.pulled == 0);
        REQUIRE_FALSE(result.errors.empty());
        REQUIRE(other.profiles->GetUserCount() == 1);
        REQUIRE(other.profiles->GetAllUsers() == users);
    }
    std::filesystem::remove(profiles_dat);
    REQUIRE(other.profiles->WriteUserSaveFile());
    const auto result = SyncAll(*other.profiles, {}, FailOnConflict(), {});
    REQUIRE(result.pulled == 1);
    REQUIRE(other.profiles->GetUserCount() == 2);
    REQUIRE(other.profiles->UserExists(remote));
}

TEST_CASE("CloudSync: a pull takes the avatar its profile.json names", "[cloud_sync]") {
    FakeDriveServer drive;
    Common::UUID user;
    {
        CloudSyncMachine machine;
        user = machine.FirstUser();
        std::filesystem::create_directories(AvatarPath(machine, user).parent_path());
        std::ofstream{AvatarPath(machine, user), std::ios::binary} << "published avatar";
        REQUIRE(SyncAll(*machine.profiles, {}, FailOnConflict(), {}).pushed == 1);
    }
    const auto avatar = drive.Find({{"lichii", "avatar"}}).at(0);
    REQUIRE(avatar.app_properties.at("contentHash") == AvatarHash("published avatar"));
    // Another machine publishing at the same time left an older avatar beside it.
    drive.Add({.name = "avatar.jpg",
               .parents = avatar.parents,
               .app_properties = {{"lichii", "avatar"},
                                  {"userUuid", user.FormattedString()},
                                  {"contentHash", AvatarHash("other avatar")}},
               .data = "other avatar",
               .created_time = "2026-01-01T00:00:00.000Z"});

    ForgetSyncState(drive);
    CloudSyncMachine other;
    const auto result = SyncAll(*other.profiles, {}, FailOnConflict(), {});
    REQUIRE(result.errors.empty());
    REQUIRE(result.pulled == 1);
    REQUIRE(ReadText(AvatarPath(other, user)) == "published avatar");
}

TEST_CASE("CloudSync: the profile list shows the duplicate sync picks", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const auto user = machine.FirstUser();
    REQUIRE(SyncAll(*machine.profiles, {}, FailOnConflict(), {}).pushed == 1);
    const auto json = ProfileJson(drive, user);
    auto older = nlohmann::json::parse(json.data);
    older["nickname"] = "Older";
    const std::string older_data = older.dump();
    const ZipEntry older_entry{"profile.json", {older_data.begin(), older_data.end()}};
    drive.Add({.name = "profile.json",
               .parents = json.parents,
               .app_properties = {{"lichii", "profileJson"},
                                  {"userUuid", user.FormattedString()},
                                  {"contentHash", ContentHash({&older_entry, 1})}},
               .data = older_data,
               .created_time = "2026-01-01T00:00:00.000Z"});
    const auto listed = ListRemoteProfiles();
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    REQUIRE(listed->at(0).nickname == "Older");
}

TEST_CASE("CloudSync: profiles aren't synced while a guest runs", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const auto user = machine.FirstUser();
    machine.WriteSave({TitleId, user}, "save");
    const Title other{0x0100EA80032EA000, "Other"};
    {
        const u64 registration = BeginGuest(other.id);
        SCOPE_EXIT {
            EndGuest(registration);
        };
        auto result = SyncAll(*machine.profiles, {&Game, 1}, FailOnConflict(), {});
        REQUIRE(result.errors.empty());
        // The save went up, the profile waits.
        REQUIRE(result.pushed == 1);
        REQUIRE(drive.Find({{"lichii", "profileJson"}}).empty());
        result = SyncSaves(*machine.profiles, {&Game, 1}, FailOnConflict(), {});
        REQUIRE(result.pushed + result.pulled == 0);
    }
    const auto result = SyncAll(*machine.profiles, {&Game, 1}, FailOnConflict(), {});
    REQUIRE(result.pushed == 1);
    REQUIRE(drive.Find({{"lichii", "profileJson"}}).size() == 1);
}
