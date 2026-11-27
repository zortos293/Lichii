// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "frontend_common/cloud_sync/drive_client.h"
#include "tests/frontend_common/fake_google.h"

namespace Drive = FrontendCommon::CloudSync::Drive;
using Tests::FakeGoogle;

namespace {

constexpr const char* FileFields =
    "id,name,parents,appProperties,createdTime,modifiedTime,md5Checksum,headRevisionId,trashed";
constexpr const char* List = "GET /drive/v3/files";
constexpr const char* SaveZip =
    R"({"id":"file.1","name":"Player.zip","parents":["game.1"],"modifiedTime":)"
    R"("2026-11-20T00:00:00.000Z","md5Checksum":"abc","appProperties":)"
    R"({"lichii":"save","titleId":"0100152000022000","userUuid":"device"}})";

// FakeGoogle answering Drive too, signed in with a token that is still good.
struct FakeDrive : FakeGoogle {
    FakeDrive() {
        REQUIRE(FrontendCommon::CloudSync::ImportClientSecret(ClientSecret("id", "secret")));
        SignIn(3600);
        // Last: the destructor doesn't run when the constructor fails.
        Drive::SetDriveBaseForTests(Base());
    }
    ~FakeDrive() {
        Drive::SetDriveBaseForTests(std::string{Drive::DriveBase});
    }

    std::string Param(const std::string& key, const std::string& name, std::size_t index = 0) {
        const auto params = Requests(key).at(index).params;
        const auto it = params.find(name);
        return it == params.end() ? std::string{} : it->second;
    }

    std::string Header(const std::string& key, const std::string& name, std::size_t index = 0) {
        const auto headers = Requests(key).at(index).headers;
        const auto it = headers.find(name);
        return it == headers.end() ? std::string{} : it->second;
    }

    nlohmann::json Body(const std::string& key, std::size_t index = 0) {
        return nlohmann::json::parse(Requests(key).at(index).body);
    }
};

std::vector<u8> Bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

} // Anonymous namespace

TEST_CASE("CloudSync: Drive file search pages through the results", "[cloud_sync]") {
    FakeDrive drive;
    drive.Answer(List, 200, std::string{R"({"nextPageToken":"page.2","files":[)"} + SaveZip + "]}");
    drive.Answer(List, 200, R"({"files":[{"id":"file.2","name":"It's.zip"}]})");

    const auto files = Drive::FindFiles({{"lichii", "save"}, {"titleId", "0100152000022000"}},
                                        "game's \\ folder");
    REQUIRE(files.has_value());
    REQUIRE(files->size() == 2);
    const auto& first = files->at(0);
    REQUIRE(first.id == "file.1");
    REQUIRE(first.name == "Player.zip");
    REQUIRE(first.parents == std::vector<std::string>{"game.1"});
    REQUIRE(first.modified_time == "2026-11-20T00:00:00.000Z");
    REQUIRE(first.md5_checksum == "abc");
    REQUIRE(first.app_properties.at("userUuid") == "device");
    REQUIRE(files->at(1).name == "It's.zip");

    REQUIRE(drive.Param(List, "q") ==
            R"(trashed = false and 'game\'s \\ folder' in parents)"
            R"( and appProperties has { key='lichii' and value='save' })"
            R"( and appProperties has { key='titleId' and value='0100152000022000' })");
    REQUIRE(drive.Param(List, "fields") == fmt::format("nextPageToken,files({})", FileFields));
    REQUIRE(drive.Param(List, "pageToken").empty());
    REQUIRE(drive.Param(List, "pageToken", 1) == "page.2");
    REQUIRE(drive.Header(List, "Authorization") == "Bearer access.old");
}

TEST_CASE("CloudSync: Drive folders are created with their properties", "[cloud_sync]") {
    FakeDrive drive;
    drive.Answer("GET /drive/v3/files/generateIds", 200, R"({"ids":["folder.1"]})");
    drive.Answer("GET /drive/v3/files/generateIds", 200, R"({"ids":["folder.2"]})");
    drive.Answer("POST /drive/v3/files", 200, R"({"id":"folder.1","name":"Lichii"})");
    drive.Answer("POST /drive/v3/files", 200,
                 R"({"id":"folder.2","name":"Saves","parents":["folder.1"]})");

    const auto folder = Drive::CreateFolder("Lichii", "", {{"lichii", "root"}});
    REQUIRE(folder.has_value());
    REQUIRE(folder->id == "folder.1");
    REQUIRE(drive.Param("GET /drive/v3/files/generateIds", "count") == "1");
    REQUIRE(drive.Param("POST /drive/v3/files", "fields") == FileFields);
    REQUIRE(drive.Body("POST /drive/v3/files") ==
            nlohmann::json{{"id", "folder.1"},
                           {"name", "Lichii"},
                           {"mimeType", Drive::FolderMimeType},
                           {"appProperties", {{"lichii", "root"}}}});

    const auto saves = Drive::CreateFolder("Saves", "folder.1", {});
    REQUIRE(saves.has_value());
    REQUIRE(saves->id == "folder.2");
    REQUIRE(saves->parents == std::vector<std::string>{"folder.1"});
    REQUIRE(drive.Body("POST /drive/v3/files", 1)["id"] == "folder.2");
    REQUIRE(drive.Body("POST /drive/v3/files", 1)["parents"] ==
            nlohmann::json::array({"folder.1"}));
}

TEST_CASE("CloudSync: a Drive folder made by a failed attempt isn't made again", "[cloud_sync]") {
    FakeDrive drive;
    drive.Answer("GET /drive/v3/files/generateIds", 200, R"({"ids":["folder.1"]})");
    // Made, but answered with a 503: the retry finds the id taken.
    drive.Answer("POST /drive/v3/files", 503, "{}");
    drive.Answer("POST /drive/v3/files", 409, R"({"error":{"code":409,"message":"Taken"}})");
    drive.Answer("GET /drive/v3/files/folder.1", 200, R"({"id":"folder.1","name":"Lichii"})");

    const auto folder = Drive::CreateFolder("Lichii", "", {{"lichii", "root"}});
    REQUIRE(folder.has_value());
    REQUIRE(folder->id == "folder.1");
    REQUIRE(drive.Hits("POST /drive/v3/files") == 2);
    REQUIRE(drive.Body("POST /drive/v3/files", 1)["id"] == "folder.1");
}

TEST_CASE("CloudSync: Drive uploads go through a resumable session", "[cloud_sync]") {
    FakeDrive drive;
    const auto session = drive.Base() + "/upload/session?upload_id=1";
    const std::vector<u8> data = Bytes(std::string(6 * 1024 * 1024, 's'));

    SECTION("for a new file") {
        drive.Answer("POST /upload/drive/v3/files", 200, "", {{"Location", session}});
        drive.Answer("PUT /upload/session", 200, SaveZip);
        const auto file = Drive::UploadFile("Player.zip", "game.1", {{"lichii", "save"}},
                                            "application/zip", data);
        REQUIRE(file.has_value());
        REQUIRE(file->id == "file.1");
        const char* start = "POST /upload/drive/v3/files";
        REQUIRE(drive.Param(start, "uploadType") == "resumable");
        REQUIRE(drive.Param(start, "fields") == FileFields);
        REQUIRE(drive.Header(start, "X-Upload-Content-Length") == std::to_string(data.size()));
        REQUIRE(drive.Header(start, "X-Upload-Content-Type") == "application/zip");
        REQUIRE(drive.Body(start) == nlohmann::json{{"name", "Player.zip"},
                                                    {"mimeType", "application/zip"},
                                                    {"parents", {"game.1"}},
                                                    {"appProperties", {{"lichii", "save"}}}});
    }
    SECTION("for an existing file, keeping its revisions") {
        drive.Answer("PATCH /upload/drive/v3/files/file.1", 200, "", {{"Location", session}});
        drive.Answer("PUT /upload/session", 200, SaveZip);
        const auto file =
            Drive::UpdateFile("file.1", {{"contentHash", "ff"}}, "application/zip", data);
        REQUIRE(file.has_value());
        REQUIRE(file->app_properties.at("lichii") == "save");
        const char* start = "PATCH /upload/drive/v3/files/file.1";
        REQUIRE(drive.Param(start, "uploadType") == "resumable");
        REQUIRE(drive.Body(start) == nlohmann::json{{"appProperties", {{"contentHash", "ff"}}}});
    }
    const auto put = drive.Requests("PUT /upload/session").at(0);
    REQUIRE(put.params.find("upload_id")->second == "1");
    REQUIRE(put.body.size() == data.size());
    REQUIRE(std::vector<u8>{put.body.begin(), put.body.end()} == data);
    REQUIRE(drive.Header("PUT /upload/session", "Authorization") == "Bearer access.old");
}

TEST_CASE("CloudSync: Drive file metadata is read by id", "[cloud_sync]") {
    FakeDrive drive;
    drive.Answer("GET /drive/v3/files/file.1", 200,
                 R"({"id":"file.1","createdTime":"2026-11-19T00:00:00.000Z",)"
                 R"("headRevisionId":"rev.3","trashed":true})");
    const auto file = Drive::GetFile("file.1");
    REQUIRE(file.has_value());
    REQUIRE(file->created_time == "2026-11-19T00:00:00.000Z");
    REQUIRE(file->head_revision_id == "rev.3");
    REQUIRE(file->trashed);
    REQUIRE(drive.Param("GET /drive/v3/files/file.1", "fields") == FileFields);
}

TEST_CASE("CloudSync: Drive files download and rename", "[cloud_sync]") {
    FakeDrive drive;
    drive.Answer("GET /drive/v3/files/file.1", 200, "zip bytes");
    drive.Answer("PATCH /drive/v3/files/file.1", 200, R"({"id":"file.1","name":"New.zip"})");

    REQUIRE(Drive::DownloadFile("file.1") == Bytes("zip bytes"));
    REQUIRE(drive.Param("GET /drive/v3/files/file.1", "alt") == "media");
    const auto renamed = Drive::RenameFile("file.1", "New.zip");
    REQUIRE(renamed.has_value());
    REQUIRE(renamed->name == "New.zip");
    REQUIRE(drive.Body("PATCH /drive/v3/files/file.1") == nlohmann::json{{"name", "New.zip"}});
}

TEST_CASE("CloudSync: Drive downloads over httplib's 100 MiB body limit", "[cloud_sync]") {
    FakeDrive drive;
    std::string large(101 * 1024 * 1024, 'z');
    large.back() = 'e';
    drive.Answer("GET /drive/v3/files/file.1", 200, large);
    const auto data = Drive::DownloadFile("file.1");
    REQUIRE(data.has_value());
    REQUIRE(data->size() == large.size());
    REQUIRE(data->back() == 'e');
}

TEST_CASE("CloudSync: a 401 from Drive refreshes the token once", "[cloud_sync]") {
    FakeDrive drive;
    drive.Answer("POST /token", 200, R"({"access_token":"access.new","expires_in":3599})");
    drive.Answer(List, 401, "{}");

    SECTION("and retries with the new one") {
        drive.Answer(List, 200, R"({"files":[]})");
        const auto files = Drive::FindFiles({{"lichii", "root"}});
        REQUIRE(files.has_value());
        REQUIRE(files->empty());
        REQUIRE(drive.Hits("POST /token") == 1);
        REQUIRE(drive.Hits(List) == 2);
        REQUIRE(drive.Header(List, "Authorization", 1) == "Bearer access.new");
    }
    SECTION("and gives up when that is refused too") {
        REQUIRE_FALSE(Drive::FindFiles({{"lichii", "root"}}).has_value());
        REQUIRE(drive.Hits("POST /token") == 1);
        REQUIRE(drive.Hits(List) == 2);
    }
}

TEST_CASE("CloudSync: a Drive call isn't retried under another sign-in", "[cloud_sync]") {
    FakeDrive drive;
    drive.Answer("POST /token", 200, R"({"access_token":"access.new","expires_in":3599})");
    drive.Answer("POST /revoke", 200, "{}");
    drive.Answer(List, 401, "{}", {}, std::chrono::milliseconds{500});
    // Signed out and in again while the 401 is on its way.
    std::jthread switcher{[&drive](std::stop_token own_stop) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (drive.Hits(List) == 0) {
            if (own_stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        FrontendCommon::CloudSync::SignOut();
        drive.SignIn(3600);
    }};
    REQUIRE_FALSE(Drive::FindFiles({{"lichii", "root"}}).has_value());
    REQUIRE(drive.Hits(List) == 1);
    REQUIRE(Drive::LastError() == "The Google Drive sign-in changed during the request");
    REQUIRE(drive.Hits("POST /token") == 0);
}

TEST_CASE("CloudSync: a Drive call isn't retried under a sign-in granted meanwhile",
          "[cloud_sync]") {
    FakeDrive drive;
    drive.Answer("POST /device/code", 200,
                 R"({"device_code":"device.1","user_code":"ABCD-EFGH",)"
                 R"("verification_url":"u","expires_in":1800,"interval":0})");
    // Another account's sign-in, granted once the request with the old one is out, and the 401
    // only after that.
    std::atomic<bool> granted = false;
    drive.Answer("POST /token", 200,
                 R"({"access_token":"access.new","refresh_token":"refresh.new","expires_in":3599})",
                 {}, {}, [&drive] { return drive.Hits(List) > 0; });
    drive.Answer(List, 401, "{}", {}, {}, [&granted] { return granted.load(); });
    FrontendCommon::CloudSync::SignInResult signed_in{
        FrontendCommon::CloudSync::SignInStatus::Failed, {}};
    std::jthread signer{[&signed_in, &granted](std::stop_token own_stop) {
        signed_in = FrontendCommon::CloudSync::SignIn([](const auto&) {}, own_stop);
        granted = signed_in.status == FrontendCommon::CloudSync::SignInStatus::SignedIn;
    }};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (drive.Hits("POST /token") == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    REQUIRE(drive.Hits("POST /token") == 1);

    REQUIRE_FALSE(Drive::FindFiles({{"lichii", "root"}}).has_value());
    REQUIRE(drive.Hits(List) == 1);
    REQUIRE(drive.Header(List, "Authorization") == "Bearer access.old");
    REQUIRE(Drive::LastError() == "The Google Drive sign-in changed during the request");
    signer.join();
    REQUIRE(signed_in.status == FrontendCommon::CloudSync::SignInStatus::SignedIn);
    REQUIRE(drive.Hits("POST /token") == 1);
}

TEST_CASE("CloudSync: a failed token refresh isn't reported as a sign-out", "[cloud_sync]") {
    FakeDrive drive;
    drive.SignIn(0);
    drive.Answer("POST /token", 503, "{}");
    REQUIRE_FALSE(Drive::FindFiles({{"lichii", "root"}}).has_value());
    REQUIRE(Drive::LastError() == "Could not refresh the Google sign-in (HTTP 503); try again");
    REQUIRE(drive.Hits(List) == 0);
}

TEST_CASE("CloudSync: an unreadable Drive reply says why it failed", "[cloud_sync]") {
    FakeDrive drive;
    drive.Answer(List, 200, "not json");
    drive.Answer("GET /drive/v3/files/file.1", 200, R"({"name":"Player.zip"})");
    REQUIRE_FALSE(Drive::FindFiles({{"lichii", "root"}}).has_value());
    REQUIRE(Drive::LastError() == "Drive sent an unreadable file list");
    REQUIRE_FALSE(Drive::GetFile("file.1").has_value());
    REQUIRE(Drive::LastError() == "Drive sent a file without an id");
}

TEST_CASE("CloudSync: Drive calls back off on 429 and 5xx", "[cloud_sync]") {
    FakeDrive drive;
    const char* download = "GET /drive/v3/files/file.1";

    SECTION("and succeed once Drive answers") {
        drive.Answer(download, 429, R"({"error":{"code":429,"message":"Rate limit"}})");
        drive.Answer(download, 503, "{}");
        drive.Answer(download, 200, "zip");
        REQUIRE(Drive::DownloadFile("file.1") == Bytes("zip"));
        const auto requests = drive.Requests(download);
        REQUIRE(requests.size() == 3);
        REQUIRE(requests[1].time - requests[0].time >= std::chrono::seconds{1});
        REQUIRE(requests[2].time - requests[1].time >= std::chrono::seconds{2});
    }
    SECTION("but not on other errors") {
        drive.Answer(download, 404, R"({"error":{"code":404,"message":"File not found"}})");
        REQUIRE_FALSE(Drive::DownloadFile("file.1").has_value());
        REQUIRE(drive.Hits(download) == 1);
        REQUIRE(Drive::LastError() == "HTTP 404: File not found");
    }
    SECTION("and on a 403 for a rate limit") {
        drive.Answer(download, 403,
                     R"({"error":{"code":403,"message":"Rate limit","errors":[{"domain":)"
                     R"("usageLimits","reason":"userRateLimitExceeded"}]}})");
        drive.Answer(download, 200, "zip");
        REQUIRE(Drive::DownloadFile("file.1") == Bytes("zip"));
        REQUIRE(drive.Hits(download) == 2);
    }
    SECTION("but not on another 403") {
        drive.Answer(download, 403,
                     R"({"error":{"code":403,"message":"Forbidden","errors":[{"domain":)"
                     R"("global","reason":"insufficientFilePermissions"}]}})");
        REQUIRE_FALSE(Drive::DownloadFile("file.1").has_value());
        REQUIRE(drive.Hits(download) == 1);
        REQUIRE(Drive::LastError() == "HTTP 403: Forbidden");
    }
    REQUIRE(drive.Hits("POST /token") == 0);
}

TEST_CASE("CloudSync: Drive isn't called while signed out", "[cloud_sync]") {
    FakeDrive drive;
    std::filesystem::remove(drive.directory / "cloud_sync" / "token.json");
    REQUIRE_FALSE(Drive::FindFiles({{"lichii", "root"}}).has_value());
    REQUIRE(drive.Hits(List) == 0);
}

TEST_CASE("CloudSync: a stop request ends a Drive call right away", "[cloud_sync]") {
    FakeDrive drive;
    std::stop_source stop;
    SECTION("while the reply is on its way") {
        drive.Answer(List, 200, R"({"files":[]})", {}, std::chrono::seconds{2});
    }
    SECTION("while waiting to retry") {
        drive.Answer(List, 503, "{}");
    }
    const auto start = std::chrono::steady_clock::now();
    std::jthread canceller{[&stop] {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        stop.request_stop();
    }};
    REQUIRE_FALSE(Drive::FindFiles({{"lichii", "root"}}, {}, stop.get_token()).has_value());
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
    REQUIRE(Drive::LastError() == "Cancelled");
    REQUIRE(drive.Hits(List) == 1);
}
