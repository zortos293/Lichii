// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "common/settings.h"
#include "frontend_common/cloud_sync/save_archive.h"
#include "frontend_common/cloud_sync/sync_engine.h"
#include "tests/frontend_common/cloud_sync_machine.h"
#include "tests/frontend_common/fake_drive.h"

using namespace FrontendCommon::CloudSync;
using Tests::CloudSyncMachine;
using Tests::FakeDriveServer;

namespace {

const WebServiceLogin Alice{"alice", std::string(48, 'a')};
const WebServiceLogin Bob{"bob", std::string(48, 'b')};

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

SyncResult Sync(CloudSyncMachine& machine, Resolver& resolver,
                std::optional<std::optional<WebServiceLogin>> login) {
    return SyncAll(*machine.profiles, {}, resolver.Callbacks(), {}, login);
}

std::vector<FakeDriveServer::File> LoginFiles(FakeDriveServer& drive) {
    return drive.Find({{"lichii", "webServiceLogin"}});
}

// Replaces the login on Drive as another machine's upload would.
void UploadElsewhere(FakeDriveServer& drive, const WebServiceLogin& login) {
    drive.Edit(LoginFiles(drive).at(0).id, [&](FakeDriveServer::File& file) {
        file.data = nlohmann::json{{"username", login.username}, {"token", login.token}}.dump();
        const ZipEntry entry{"Web Service Login.json", {file.data.begin(), file.data.end()}};
        file.app_properties["contentHash"] = ContentHash({&entry, 1});
        ++file.revisions;
    });
}

void ForgetSyncState(FakeDriveServer& drive) {
    std::filesystem::remove(drive.google.directory / "cloud_sync" / "state.json");
}

} // Anonymous namespace

TEST_CASE("CloudSync: the web service login reaches a machine without one", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    Resolver resolver;

    // Not synced unless asked for.
    REQUIRE(Sync(machine, resolver, std::nullopt).errors.empty());
    REQUIRE(LoginFiles(drive).empty());
    // Nothing on either side.
    auto result = Sync(machine, resolver, std::optional<WebServiceLogin>{});
    REQUIRE(result.errors.empty());
    REQUIRE(LoginFiles(drive).empty());
    REQUIRE(!result.web_service_login);

    REQUIRE(Sync(machine, resolver, Alice).errors.empty());
    const auto files = LoginFiles(drive);
    REQUIRE(files.size() == 1);
    REQUIRE(files[0].name == "Web Service Login.json");
    REQUIRE(files[0].parents == std::vector{drive.Find({{"lichii", "root"}}).at(0).id});
    REQUIRE(nlohmann::json::parse(files[0].data).at("username") == "alice");

    ForgetSyncState(drive);
    result = Sync(machine, resolver, std::optional<WebServiceLogin>{});
    REQUIRE(result.errors.empty());
    REQUIRE(result.web_service_login == Alice);
    // Not applied: taken again.
    REQUIRE(Sync(machine, resolver, std::optional<WebServiceLogin>{}).web_service_login == Alice);
    // Applied: nothing to do.
    result = Sync(machine, resolver, Alice);
    REQUIRE(result.errors.empty());
    REQUIRE(!result.web_service_login);
    REQUIRE(LoginFiles(drive).at(0).revisions == files[0].revisions);
    REQUIRE(resolver.asked.empty());
}

TEST_CASE("CloudSync: the side whose web service login changed wins", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    Resolver resolver;
    REQUIRE(Sync(machine, resolver, Alice).errors.empty());

    // Changed here.
    REQUIRE(Sync(machine, resolver, Bob).errors.empty());
    REQUIRE(nlohmann::json::parse(LoginFiles(drive).at(0).data).at("username") == "bob");

    // Changed on Drive.
    UploadElsewhere(drive, Alice);
    auto result = Sync(machine, resolver, Bob);
    REQUIRE(result.errors.empty());
    REQUIRE(result.web_service_login == Alice);
    REQUIRE(Sync(machine, resolver, Alice).errors.empty());

    // A device that drops its login gets Drive's back rather than deleting it.
    result = Sync(machine, resolver, std::optional<WebServiceLogin>{});
    REQUIRE(result.web_service_login == Alice);
    REQUIRE(LoginFiles(drive).size() == 1);
    REQUIRE(resolver.asked.empty());
}

TEST_CASE("CloudSync: a web service login changed on both sides is asked", "[cloud_sync]") {
    FakeDriveServer drive;
    CloudSyncMachine machine;
    Resolver resolver;
    REQUIRE(Sync(machine, resolver, Alice).errors.empty());
    UploadElsewhere(drive, Bob);
    const WebServiceLogin carol{"carol", std::string(48, 'c')};

    resolver.choice = ConflictChoice::Skip;
    auto result = Sync(machine, resolver, carol);
    REQUIRE(result.skipped == 1);
    REQUIRE(!result.web_service_login);
    REQUIRE(resolver.asked.size() == 1);
    REQUIRE(resolver.asked[0].web_service);
    REQUIRE(resolver.asked[0].nickname == "carol");
    REQUIRE(resolver.asked[0].remote_nickname == "bob");

    resolver.choice = ConflictChoice::KeepLocal;
    result = Sync(machine, resolver, carol);
    REQUIRE(result.errors.empty());
    REQUIRE(nlohmann::json::parse(LoginFiles(drive).at(0).data).at("username") == "carol");

    UploadElsewhere(drive, Bob);
    resolver.choice = ConflictChoice::KeepRemote;
    result = Sync(machine, resolver, Alice);
    REQUIRE(result.web_service_login == Bob);
}

TEST_CASE("CloudSync: the default web service username is no login", "[cloud_sync]") {
    const auto username = Settings::values.eden_username.GetValue();
    const auto token = Settings::values.eden_token.GetValue();
    Settings::values.eden_token = std::string(48, 'x');
    Settings::values.eden_username = Settings::values.eden_username.GetDefault();
    REQUIRE(!LocalWebServiceLogin());
    Settings::values.eden_username = "alice";
    REQUIRE(LocalWebServiceLogin() == WebServiceLogin{"alice", std::string(48, 'x')});
    Settings::values.eden_username = username;
    Settings::values.eden_token = token;
}
