// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "frontend_common/cloud_sync/drive_client.h"
#include "frontend_common/cloud_sync/google_auth.h"
#include "frontend_common/cloud_sync/save_archive.h"
#include "frontend_common/cloud_sync/sync_engine.h"
#include "frontend_common/cloud_sync/sync_layout.h"
#include "frontend_common/cloud_sync/sync_state.h"
#include "tests/frontend_common/cloud_sync_machine.h"
#include "tests/frontend_common/fake_drive.h"

using namespace FrontendCommon::CloudSync;
using Tests::CloudSyncMachine;
using Tests::FakeDriveServer;

namespace {

constexpr u64 TitleId = 0x0100152000022000;
const Title Game{TitleId, "Mario Kart 8 Deluxe"};

// Answers every conflict with choice, keeping what it was asked.
struct Resolver {
    ConflictChoice choice = ConflictChoice::Skip;
    std::vector<Conflict> asked;

    SyncCallbacks Callbacks() {
        return {.resolve_conflict = [this](const Conflict& conflict, std::stop_token) {
            asked.push_back(conflict);
            return choice;
        }};
    }
};

FakeDriveServer::File SaveFile(FakeDriveServer& drive, const std::string& user) {
    const auto found =
        drive.Find({{"lichii", "save"}, {"titleId", "0100152000022000"}, {"userUuid", user}});
    REQUIRE(found.size() == 1);
    return found.front();
}

// Replaces a save on Drive as another machine's upload would.
void UploadElsewhere(FakeDriveServer& drive, const std::string& id, const PackedSave& save) {
    drive.Edit(id, [&](FakeDriveServer::File& file) {
        file.data.assign(save.zip.begin(), save.zip.end());
        file.app_properties["contentHash"] = save.content_hash;
        ++file.revisions;
    });
}

std::filesystem::path StatePath(FakeDriveServer& drive) {
    return drive.google.directory / "cloud_sync" / "state.json";
}

} // Anonymous namespace

TEST_CASE("CloudSync: the first sync uploads every profile's save and the device save",
          "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const auto user = machine.FirstUser();
    const SaveId account{TitleId, user};
    const SaveId device{TitleId, std::nullopt};
    const SaveId orphan{TitleId, Common::UUID::MakeRandomWithSeed(99)};
    machine.WriteSave(account, "account");
    machine.WriteSave(device, "device");
    machine.WriteSave(orphan, "orphan");
    Resolver resolver;

    auto result = SyncAll(*machine.profiles, {&Game, 1}, resolver.Callbacks(), {});
    REQUIRE(result.errors.empty());
    // The profile and the two saves.
    REQUIRE(result.pushed == 3);

    const auto root = drive.Find({{"lichii", "root"}});
    REQUIRE(root.size() == 1);
    REQUIRE(root[0].name == "Lichii");
    const auto saves = drive.Find({{"lichii", "saves"}});
    REQUIRE(saves.size() == 1);
    REQUIRE(saves[0].name == "Saves");
    REQUIRE(saves[0].parents == std::vector{root[0].id});
    const auto game = drive.Find({{"lichii", "game"}, {"titleId", "0100152000022000"}});
    REQUIRE(game.size() == 1);
    REQUIRE(game[0].name == "Mario Kart 8 Deluxe");
    REQUIRE(game[0].parents == std::vector{saves[0].id});

    const auto account_file = SaveFile(drive, user.FormattedString());
    REQUIRE(account_file.name == "Eden.zip");
    REQUIRE(account_file.parents == std::vector{game[0].id});
    REQUIRE(account_file.app_properties.at("contentHash") ==
            PackSave(LocalSavePath(account))->content_hash);
    REQUIRE(SaveFile(drive, "device").name == "Shared.zip");
    // The orphaned save folder, whose user has no profile, stays local.
    REQUIRE(drive.Find({{"userUuid", orphan.user->FormattedString()}}).empty());

    result = SyncAll(*machine.profiles, {&Game, 1}, resolver.Callbacks(), {});
    REQUIRE(result.errors.empty());
    REQUIRE(result.pushed + result.pulled == 0);
    REQUIRE(SaveFile(drive, user.FormattedString()).revisions == 0);
    REQUIRE(resolver.asked.empty());
}

TEST_CASE("CloudSync: a save changed on one side goes to the other", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const auto user = machine.FirstUser();
    const SaveId account{TitleId, user};
    machine.WriteSave(account, "first");
    Resolver resolver;
    REQUIRE(PushTitle(*machine.profiles, Game, resolver.Callbacks(), {}).pushed == 1);
    const std::string id = SaveFile(drive, user.FormattedString()).id;

    SECTION("Changed here: pushed in place after the game, not before") {
        machine.WriteSave(account, "second");
        auto result = PullTitle(*machine.profiles, Game, resolver.Callbacks(), {});
        REQUIRE(result.pushed + result.pulled == 0);
        result = PushTitle(*machine.profiles, Game, resolver.Callbacks(), {});
        REQUIRE(result.errors.empty());
        REQUIRE(result.pushed == 1);
        const auto file = SaveFile(drive, user.FormattedString());
        REQUIRE(file.id == id);
        REQUIRE(file.revisions == 1);
        REQUIRE(file.app_properties.at("contentHash") == machine.Packed("second").content_hash);
    }

    SECTION("Changed on Drive: pulled before the game, not after") {
        UploadElsewhere(drive, id, machine.Packed("elsewhere"));
        auto result = PushTitle(*machine.profiles, Game, resolver.Callbacks(), {});
        REQUIRE(result.pushed + result.pulled == 0);
        REQUIRE(machine.ReadSave(account) == "first");
        result = PullTitle(*machine.profiles, Game, resolver.Callbacks(), {});
        REQUIRE(result.errors.empty());
        REQUIRE(result.pulled == 1);
        REQUIRE(machine.ReadSave(account) == "elsewhere");
        // The replaced save was backed up.
        REQUIRE(!std::filesystem::is_empty(BackupFolder(account)));
        REQUIRE(PushTitle(*machine.profiles, Game, resolver.Callbacks(), {}).pushed == 0);
    }

    SECTION("Missing here: pulled") {
        std::filesystem::remove_all(LocalSavePath(account));
        const auto result = PullTitle(*machine.profiles, Game, resolver.Callbacks(), {});
        REQUIRE(result.pulled == 1);
        REQUIRE(machine.ReadSave(account) == "first");
    }

    SECTION("Missing on Drive: pushed again, nothing deleted here") {
        drive.Edit(id, [](FakeDriveServer::File& file) { file.trashed = true; });
        const auto result = PushTitle(*machine.profiles, Game, resolver.Callbacks(), {});
        REQUIRE(result.pushed == 1);
        REQUIRE(SaveFile(drive, user.FormattedString()).id != id);
        REQUIRE(machine.ReadSave(account) == "first");
    }

    SECTION("The same on both sides without a sync state: nothing moves") {
        std::filesystem::remove(StatePath(drive));
        const auto result = PushTitle(*machine.profiles, Game, resolver.Callbacks(), {});
        REQUIRE(result.pushed + result.pulled + result.skipped == 0);
        REQUIRE(resolver.asked.empty());
        REQUIRE(std::filesystem::exists(StatePath(drive)));
    }
    REQUIRE(resolver.asked.empty());
}

TEST_CASE("CloudSync: a save changed on both sides goes to the conflict callback", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const auto user = machine.FirstUser();
    const SaveId account{TitleId, user};
    machine.WriteSave(account, "base");
    Resolver resolver;
    REQUIRE(PushTitle(*machine.profiles, Game, resolver.Callbacks(), {}).pushed == 1);
    const std::string id = SaveFile(drive, user.FormattedString()).id;
    machine.WriteSave(account, "here");
    UploadElsewhere(drive, id, machine.Packed("there"));
    const auto remote_time = DriveTime(drive.Get(id).modified_time);

    SECTION("Keep local") {
        resolver.choice = ConflictChoice::KeepLocal;
        const auto result = PullTitle(*machine.profiles, Game, resolver.Callbacks(), {});
        REQUIRE(result.pushed == 1);
        const auto file = SaveFile(drive, user.FormattedString());
        REQUIRE(file.id == id);
        REQUIRE(file.app_properties.at("contentHash") == machine.Packed("here").content_hash);
        REQUIRE(machine.ReadSave(account) == "here");
    }

    SECTION("Keep remote") {
        resolver.choice = ConflictChoice::KeepRemote;
        const auto result = PushTitle(*machine.profiles, Game, resolver.Callbacks(), {});
        REQUIRE(result.pulled == 1);
        REQUIRE(machine.ReadSave(account) == "there");
        REQUIRE(!std::filesystem::is_empty(BackupFolder(account)));
    }

    SECTION("Skip, and ask again next time") {
        auto result = PullTitle(*machine.profiles, Game, resolver.Callbacks(), {});
        REQUIRE(result.skipped == 1);
        REQUIRE(result.pushed + result.pulled == 0);
        REQUIRE(machine.ReadSave(account) == "here");
        REQUIRE(SaveFile(drive, user.FormattedString()).app_properties.at("contentHash") ==
                machine.Packed("there").content_hash);
        result = PushTitle(*machine.profiles, Game, resolver.Callbacks(), {});
        REQUIRE(result.skipped == 1);
        REQUIRE(resolver.asked.size() == 2);
    }

    SECTION("Never synced from here") {
        std::filesystem::remove(StatePath(drive));
        const auto result = PullTitle(*machine.profiles, Game, resolver.Callbacks(), {});
        REQUIRE(result.skipped == 1);
    }

    REQUIRE(!resolver.asked.empty());
    const Conflict& conflict = resolver.asked.front();
    REQUIRE(conflict.title_id == TitleId);
    REQUIRE(conflict.title_name == "Mario Kart 8 Deluxe");
    REQUIRE(conflict.nickname == "Eden");
    REQUIRE(conflict.remote_time == remote_time);
    REQUIRE(conflict.local_time > std::chrono::system_clock::now() - std::chrono::hours{1});
}

TEST_CASE("CloudSync: renamed folders and saves on Drive are still found", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const auto user = machine.FirstUser();
    const SaveId account{TitleId, user};
    machine.WriteSave(account, "first");
    Resolver resolver;
    REQUIRE(PushTitle(*machine.profiles, Game, resolver.Callbacks(), {}).pushed == 1);
    const std::string id = SaveFile(drive, user.FormattedString()).id;
    const auto game = drive.Find({{"lichii", "game"}});
    drive.Edit(game.at(0).id, [](FakeDriveServer::File& file) { file.name = "MK8"; });
    drive.Edit(id, [](FakeDriveServer::File& file) { file.name = "mine.zip"; });
    const std::size_t files = drive.Count();

    machine.WriteSave(account, "second");
    machine.WriteSave({TitleId, std::nullopt}, "device");
    const auto result = PushTitle(*machine.profiles, Game, resolver.Callbacks(), {});
    REQUIRE(result.pushed == 2);
    // Only the new device save was added, in the renamed folder.
    REQUIRE(drive.Count() == files + 1);
    REQUIRE(SaveFile(drive, "device").parents == std::vector{game.at(0).id});
    REQUIRE(drive.Get(id).revisions == 1);
    REQUIRE(drive.Get(id).name == "mine.zip");
}

TEST_CASE("CloudSync: a game folder named like another title's gets the title id", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const Title first{TitleId, "Same Name"};
    const Title second{0x0100EA80032EA000, "Same Name"};
    machine.WriteSave({first.id, std::nullopt}, "first");
    machine.WriteSave({second.id, std::nullopt}, "second");
    Resolver resolver;
    const std::vector titles{first, second};
    REQUIRE(SyncAll(*machine.profiles, titles, resolver.Callbacks(), {}).pushed == 3);

    REQUIRE(drive.Find({{"lichii", "game"}, {"titleId", "0100152000022000"}}).at(0).name ==
            "Same Name");
    REQUIRE(drive.Find({{"lichii", "game"}, {"titleId", "0100EA80032EA000"}}).at(0).name ==
            "Same Name [0100EA80032EA000]");
}

TEST_CASE("CloudSync: a sync without Drive fails fast and changes nothing", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const SaveId device{TitleId, std::nullopt};
    machine.WriteSave(device, "kept");
    Resolver resolver;

    SECTION("Unreachable") {
        // Nothing listens on port 1.
        Drive::SetDriveBaseForTests("http://127.0.0.1:1");
        const auto start = std::chrono::steady_clock::now();
        const auto result = PullTitle(*machine.profiles, Game, resolver.Callbacks(), {});
        REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds{5});
        REQUIRE(result.errors.size() == 1);
    }

    SECTION("Signed out") {
        std::filesystem::remove(drive.google.directory / "cloud_sync" / "token.json");
        const auto result = SyncAll(*machine.profiles, {&Game, 1}, resolver.Callbacks(), {});
        REQUIRE(result.errors.size() == 1);
        REQUIRE(result.errors[0].ends_with(": Not signed in to Google Drive"));
    }

    SECTION("Cancelled before") {
        std::stop_source stop;
        stop.request_stop();
        const auto result =
            SyncAll(*machine.profiles, {&Game, 1}, resolver.Callbacks(), stop.get_token());
        REQUIRE(result.cancelled);
    }

    SECTION("Cancelled while Drive answers") {
        drive.list_delay = std::chrono::seconds{2};
        std::stop_source stop;
        const auto start = std::chrono::steady_clock::now();
        // Once a search is on its way.
        std::jthread canceller{[&drive, &stop](std::stop_token own_stop) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (drive.list_requests == 0 && !own_stop.stop_requested() &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
            stop.request_stop();
        }};
        const auto result =
            PullTitle(*machine.profiles, Game, resolver.Callbacks(), stop.get_token());
        REQUIRE(drive.list_requests > 0);
        REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
        REQUIRE(result.cancelled);
        REQUIRE(result.errors.empty());
    }

    REQUIRE(drive.Count() == 0);
    REQUIRE(machine.ReadSave(device) == "kept");
    REQUIRE_FALSE(LastSyncTime());
}

TEST_CASE("CloudSync: a sync without errors records when it finished", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    machine.WriteSave({TitleId, std::nullopt}, "device");
    Resolver resolver;
    REQUIRE_FALSE(LastSyncTime());

    const auto before = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    REQUIRE(PushTitle(*machine.profiles, Game, resolver.Callbacks(), {}).errors.empty());
    const auto synced = LastSyncTime();
    REQUIRE(synced);
    REQUIRE(*synced >= before);

    // A failed sync leaves it alone.
    Drive::SetDriveBaseForTests("http://127.0.0.1:1");
    REQUIRE_FALSE(PullTitle(*machine.profiles, Game, resolver.Callbacks(), {}).errors.empty());
    REQUIRE(LastSyncTime() == synced);
}

TEST_CASE("CloudSync: a sync stopped before it finished records no time", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    std::stop_source stop;
    const SyncCallbacks stop_at_start{
        .progress = [&stop](std::size_t, std::size_t) { stop.request_stop(); }};
    const auto result = SyncSaves(*machine.profiles, {}, stop_at_start, stop.get_token());
    REQUIRE(result.cancelled);
    REQUIRE_FALSE(LastSyncTime());
}

TEST_CASE("CloudSync: a save Drive changed since the decision is decided again", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const auto user = machine.FirstUser();
    const SaveId account{TitleId, user};
    machine.WriteSave(account, "base");
    Resolver resolver;
    REQUIRE(PushTitle(*machine.profiles, Game, resolver.Callbacks(), {}).pushed == 1);
    const std::string id = SaveFile(drive, user.FormattedString()).id;
    machine.WriteSave(account, "here");

    // Another machine uploads between this one's search and its upload.
    const auto there = machine.Packed("there");
    bool uploaded = false;
    drive.before_get = [&](const std::string& file) {
        if (file == id && !std::exchange(uploaded, true)) {
            UploadElsewhere(drive, id, there);
        }
    };
    const auto result = PushTitle(*machine.profiles, Game, resolver.Callbacks(), {});
    REQUIRE(result.pushed == 0);
    REQUIRE(result.skipped == 1);
    REQUIRE(resolver.asked.size() == 1);
    REQUIRE(drive.Get(id).app_properties.at("contentHash") == there.content_hash);
}

TEST_CASE("CloudSync: every machine syncs the same one of duplicate saves", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const auto user = machine.FirstUser();
    const SaveId account{TitleId, user};
    machine.WriteSave(account, "mine");
    Resolver resolver;
    REQUIRE(PushTitle(*machine.profiles, Game, resolver.Callbacks(), {}).pushed == 1);
    const auto mine = SaveFile(drive, user.FormattedString());

    // Another machine made its own file for the same save, first.
    const auto add_older = [&](const PackedSave& save) {
        return drive.Add({
            .name = "Eden.zip",
            .parents = mine.parents,
            .app_properties = {{"lichii", "save"},
                               {"titleId", "0100152000022000"},
                               {"userUuid", user.FormattedString()},
                               {"contentHash", save.content_hash}},
            .data = {save.zip.begin(), save.zip.end()},
            .created_time = "2026-01-01T00:00:00.000Z",
        });
    };

    SECTION("The same contents: synced as one") {
        const auto older = add_older(machine.Packed("mine"));
        machine.WriteSave(account, "changed");
        const auto result = PushTitle(*machine.profiles, Game, resolver.Callbacks(), {});
        REQUIRE(result.errors.empty());
        REQUIRE(resolver.asked.empty());
        REQUIRE(result.pushed == 1);
        REQUIRE(drive.Get(older).revisions == 1);
        REQUIRE(drive.Get(mine.id).revisions == 0);
    }

    SECTION("Different contents: reported, both kept, and the first one is a conflict") {
        const auto older = add_older(machine.Packed("theirs"));
        const auto result = PushTitle(*machine.profiles, Game, resolver.Callbacks(), {});
        REQUIRE(result.errors.size() == 1);
        REQUIRE(result.skipped == 1);
        REQUIRE(resolver.asked.size() == 1);
        REQUIRE(drive.Get(older).revisions == 0);
        REQUIRE(drive.Get(mine.id).revisions == 0);
        REQUIRE(machine.ReadSave(account) == "mine");
    }
}

TEST_CASE("CloudSync: a synced save missing from the search is looked up, not made again",
          "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const auto user = machine.FirstUser();
    const SaveId account{TitleId, user};
    machine.WriteSave(account, "first");
    Resolver resolver;
    REQUIRE(PushTitle(*machine.profiles, Game, resolver.Callbacks(), {}).pushed == 1);
    const std::string id = SaveFile(drive, user.FormattedString()).id;
    drive.Edit(id, [](FakeDriveServer::File& file) { file.hidden = true; });
    const std::size_t files = drive.Count();

    machine.WriteSave(account, "second");
    const auto result = PushTitle(*machine.profiles, Game, resolver.Callbacks(), {});
    REQUIRE(result.errors.empty());
    REQUIRE(result.pushed == 1);
    REQUIRE(drive.Count() == files);
    REQUIRE(drive.Get(id).revisions == 1);
}

TEST_CASE("CloudSync: a download that doesn't match its hash isn't restored", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const auto user = machine.FirstUser();
    const SaveId account{TitleId, user};
    machine.WriteSave(account, "first");
    Resolver resolver;
    REQUIRE(PushTitle(*machine.profiles, Game, resolver.Callbacks(), {}).pushed == 1);
    const std::string id = SaveFile(drive, user.FormattedString()).id;
    const auto other = machine.Packed("other");
    drive.Edit(id, [&](FakeDriveServer::File& file) {
        file.data.assign(other.zip.begin(), other.zip.end());
        file.app_properties["contentHash"] = machine.Packed("claimed").content_hash;
    });

    const auto result = PullTitle(*machine.profiles, Game, resolver.Callbacks(), {});
    REQUIRE(result.pulled == 0);
    REQUIRE(result.errors.size() == 1);
    REQUIRE(machine.ReadSave(account) == "first");
}

TEST_CASE("CloudSync: an unreadable sync state stops the sync", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    machine.WriteSave({TitleId, std::nullopt}, "device");
    std::filesystem::create_directories(StatePath(drive).parent_path());
    std::ofstream{StatePath(drive)} << "{not json";
    Resolver resolver;
    const auto result = PushTitle(*machine.profiles, Game, resolver.Callbacks(), {});
    REQUIRE(result.errors.size() == 1);
    REQUIRE(drive.Count() == 0);
    std::ifstream file{StatePath(drive)};
    REQUIRE(std::string{std::istreambuf_iterator<char>{file}, {}} == "{not json");
}

TEST_CASE("CloudSync: waiting for another sync ends on a stop request", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    Resolver resolver;
    // Another sync, on its own thread: the mutex isn't recursive.
    std::atomic<bool> busy = false;
    // Set when the holder let go only at its deadline: a pull that ignored the stop request.
    std::atomic<bool> held_to_deadline = false;
    std::jthread other_sync{[&busy, &held_to_deadline](std::stop_token own_stop) {
        std::scoped_lock lock{SyncStateMutex()};
        busy = true;
        busy.notify_one();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!own_stop.stop_requested()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                held_to_deadline = true;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }};
    busy.wait(false);
    std::stop_source stop;
    std::jthread canceller{[&stop] {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        stop.request_stop();
    }};
    const auto start = std::chrono::steady_clock::now();
    const auto result = PullTitle(*machine.profiles, Game, resolver.Callbacks(), stop.get_token());
    REQUIRE_FALSE(held_to_deadline);
    REQUIRE(result.cancelled);
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
}

#ifndef _WIN32
TEST_CASE("CloudSync: an unreadable save is reported, not taken as missing", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const SaveId device{TitleId, std::nullopt};
    machine.WriteSave(device, "device");
    const auto file = LocalSavePath(device) / "save.dat";
    std::filesystem::permissions(file, std::filesystem::perms::none);
    if (std::ifstream{file}.is_open()) {
        SKIP("File permissions don't keep this runner from reading the save");
    }
    Resolver resolver;
    const auto result = PushTitle(*machine.profiles, Game, resolver.Callbacks(), {});
    std::filesystem::permissions(file, std::filesystem::perms::owner_all);
    REQUIRE(result.errors.size() == 1);
    REQUIRE(result.pushed == 0);
    REQUIRE(drive.Count() == 0);
}
#endif

TEST_CASE("CloudSync: sides that changed the same way become the base", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const auto user = machine.FirstUser();
    const SaveId account{TitleId, user};
    machine.WriteSave(account, "A");
    Resolver resolver;
    REQUIRE(PushTitle(*machine.profiles, Game, resolver.Callbacks(), {}).pushed == 1);
    const std::string id = SaveFile(drive, user.FormattedString()).id;
    machine.WriteSave(account, "B");
    UploadElsewhere(drive, id, machine.Packed("B"));
    REQUIRE(PullTitle(*machine.profiles, Game, resolver.Callbacks(), {}).pulled == 0);

    SECTION("Drive back to A: pulled") {
        UploadElsewhere(drive, id, machine.Packed("A"));
        const auto result = PullTitle(*machine.profiles, Game, resolver.Callbacks(), {});
        REQUIRE(result.pulled == 1);
        REQUIRE(machine.ReadSave(account) == "A");
    }
    SECTION("Here back to A: pushed") {
        machine.WriteSave(account, "A");
        const auto result = PushTitle(*machine.profiles, Game, resolver.Callbacks(), {});
        REQUIRE(result.pushed == 1);
        REQUIRE(drive.Get(id).app_properties.at("contentHash") == machine.Packed("A").content_hash);
    }
    REQUIRE(resolver.asked.empty());
}

TEST_CASE("CloudSync: a stop request ends a sync waiting behind a token refresh", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    drive.google.SignIn(0);
    drive.google.Answer("POST /token", 200, R"({"access_token":"access.new","expires_in":3599})",
                        {}, std::chrono::seconds{2});
    std::jthread refresher{[] { FrontendCommon::CloudSync::AccessToken(true); }};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (drive.google.Hits("POST /token") == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    REQUIRE(drive.google.Hits("POST /token") == 1);
    std::stop_source stop;
    std::jthread canceller{[&stop] {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        stop.request_stop();
    }};
    Resolver resolver;
    const auto start = std::chrono::steady_clock::now();
    const auto result = PullTitle(*machine.profiles, Game, resolver.Callbacks(), stop.get_token());
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
    REQUIRE(result.cancelled);
    REQUIRE(result.errors.empty());
}

TEST_CASE("CloudSync: an answer waited for gives way to a stop request", "[cloud_sync]") {
    SECTION("Answered") {
        const auto choice = WaitForAnswer({}, [](auto answer) {
            answer(ConflictChoice::KeepRemote);
            answer(ConflictChoice::KeepLocal);
        });
        REQUIRE(choice == ConflictChoice::KeepRemote);
    }
    SECTION("Stopped while pending") {
        std::stop_source stop;
        std::function<void(ConflictChoice)> late;
        std::jthread canceller{[&stop] {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            stop.request_stop();
        }};
        const auto start = std::chrono::steady_clock::now();
        const auto choice =
            WaitForAnswer(stop.get_token(), [&late](auto answer) { late = std::move(answer); });
        REQUIRE(choice == ConflictChoice::Skip);
        REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
        // An answer after the wait ended is harmless.
        late(ConflictChoice::KeepLocal);
    }
}

TEST_CASE("CloudSync: booting a title stops the syncs touching it", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    const auto user = machine.FirstUser();
    const SaveId account{TitleId, user};
    machine.WriteSave(account, "base");
    Resolver resolver;
    REQUIRE(PushTitle(*machine.profiles, Game, resolver.Callbacks(), {}).pushed == 1);
    machine.WriteSave(account, "here");
    UploadElsewhere(drive, SaveFile(drive, user.FormattedString()).id, machine.Packed("there"));

    // A push waiting for a conflict answer nobody gives.
    std::atomic<bool> asked = false;
    const SyncCallbacks never_answered{
        .resolve_conflict = [&asked](const Conflict&, std::stop_token stop) {
            asked = true;
            return WaitForAnswer(stop, [](auto) {});
        }};
    SyncResult pushed;
    std::jthread push{[&](std::stop_token own_stop) {
        pushed = PushTitle(*machine.profiles, Game, never_answered, own_stop);
    }};
    // Stops the push should BeginGuest not, so that the test fails rather than hangs.
    std::atomic<bool> watchdog_stopped_push = false;
    std::jthread watchdog{[&push, &watchdog_stopped_push](std::stop_token own_stop) {
        std::mutex mutex;
        std::unique_lock lock{mutex};
        std::condition_variable_any{}.wait_for(lock, own_stop, std::chrono::seconds{5},
                                               [] { return false; });
        watchdog_stopped_push = !own_stop.stop_requested();
        push.request_stop();
    }};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!asked && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    REQUIRE(asked);

    {
        const auto start = std::chrono::steady_clock::now();
        const u64 registration = BeginGuest(TitleId);
        SCOPE_EXIT {
            EndGuest(registration);
        };
        REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
        push.join();
        REQUIRE_FALSE(watchdog_stopped_push);
        REQUIRE(pushed.skipped == 1);
        REQUIRE(pushed.pushed == 0);
        REQUIRE_FALSE(SyncRunning());

        // While it runs, only the pull before boot syncs it.
        const auto result = PushTitle(*machine.profiles, Game, resolver.Callbacks(), {});
        REQUIRE(result.errors.size() == 1);
        REQUIRE(resolver.asked.empty());
    }
    PushTitle(*machine.profiles, Game, resolver.Callbacks(), {});
    REQUIRE(resolver.asked.size() == 1);
}

TEST_CASE("CloudSync: ending a replaced guest leaves the later one running", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    machine.WriteSave({TitleId, machine.FirstUser()}, "save");
    Resolver resolver;
    const u64 earlier = BeginGuest(0x0100EA80032EA000);
    const u64 later = BeginGuest(TitleId);
    SCOPE_EXIT {
        EndGuest(later);
    };
    EndGuest(earlier);
    const auto result = PushTitle(*machine.profiles, Game, resolver.Callbacks(), {});
    REQUIRE(result.errors.size() == 1);
    REQUIRE(result.pushed == 0);
    EndGuest(later);
    REQUIRE(PushTitle(*machine.profiles, Game, resolver.Callbacks(), {}).pushed == 1);
}
