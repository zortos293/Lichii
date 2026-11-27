// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stop_token>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "frontend_common/cloud_sync/build_client.h"
#include "frontend_common/cloud_sync/google_auth.h"
#include "tests/frontend_common/fake_google.h"

using namespace FrontendCommon::CloudSync;
using Tests::FakeGoogle;

namespace {

constexpr const char* DeviceCodeEndpoint = "POST /device/code";
constexpr const char* TokenEndpoint = "POST /token";
constexpr const char* RevokeEndpoint = "POST /revoke";
constexpr const char* Code =
    R"({"device_code":"device.1","user_code":"ABCD-EFGH",)"
    R"("verification_url":"https://www.google.com/device","expires_in":1800,"interval":0})";
constexpr const char* Granted =
    R"({"access_token":"access.new","refresh_token":"refresh.new","expires_in":3599,)"
    R"("scope":"https://www.googleapis.com/auth/drive.file","token_type":"Bearer"})";

std::string Param(const FakeGoogle::Request& request, const std::string& key) {
    const auto it = request.params.find(key);
    return it == request.params.end() ? std::string{} : it->second;
}

SignInResult SignInWith(FakeGoogle& google, std::vector<DeviceCode>& shown,
                        std::stop_token stop = {}) {
    REQUIRE(ImportClientSecret(FakeGoogle::ClientSecret("client.id", "client.secret")));
    return SignIn([&shown](const DeviceCode& code) { shown.push_back(code); }, stop);
}

#ifndef _WIN32
// Whether a file can be made in folder: a runner that bypasses permissions (root) can.
bool CanWriteIn(const std::filesystem::path& folder) {
    const auto probe = folder / "probe";
    if (!std::ofstream{probe}.is_open()) {
        return false;
    }
    std::filesystem::remove(probe);
    return true;
}
#endif

} // Anonymous namespace

TEST_CASE("CloudSync: device flow sign-in", "[cloud_sync]") {
    FakeGoogle google;
    std::vector<DeviceCode> shown;

    SECTION("is granted") {
        google.Answer(DeviceCodeEndpoint, 200, Code);
        google.Answer(TokenEndpoint, 200, Granted);
        REQUIRE(SignInWith(google, shown).status == SignInStatus::SignedIn);
        REQUIRE(shown.size() == 1);
        REQUIRE(shown[0].user_code == "ABCD-EFGH");
        REQUIRE(shown[0].verification_url == "https://www.google.com/device");
        REQUIRE(shown[0].expires_in == 1800);

        const auto code_request = google.Requests(DeviceCodeEndpoint).at(0);
        REQUIRE(Param(code_request, "client_id") == "client.id");
        REQUIRE(Param(code_request, "scope") == DriveScope);
        const auto poll = google.Requests(TokenEndpoint).at(0);
        REQUIRE(Param(poll, "client_secret") == "client.secret");
        REQUIRE(Param(poll, "device_code") == "device.1");
        REQUIRE(Param(poll, "grant_type") == "urn:ietf:params:oauth:grant-type:device_code");

        REQUIRE(SignedIn());
        REQUIRE(AccessToken() == "access.new");
        REQUIRE(google.Hits(TokenEndpoint) == 1);
#ifndef _WIN32
        REQUIRE(std::filesystem::status(google.directory / "cloud_sync" / "token.json")
                    .permissions() == (std::filesystem::perms::owner_read |
                                       std::filesystem::perms::owner_write));
#endif
    }
    SECTION("polls while pending and slows down when asked") {
        google.Answer(DeviceCodeEndpoint, 200, Code);
        google.Answer(TokenEndpoint, 428, R"({"error":"authorization_pending"})");
        google.Answer(TokenEndpoint, 403, R"({"error":"slow_down"})");
        google.Answer(TokenEndpoint, 428, R"({"error":"authorization_pending"})");
        google.Answer(TokenEndpoint, 200, Granted);
        REQUIRE(SignInWith(google, shown).status == SignInStatus::SignedIn);
        const auto polls = google.Requests(TokenEndpoint);
        REQUIRE(polls.size() == 4);
        REQUIRE(polls[2].time - polls[1].time >= std::chrono::seconds{5});
        REQUIRE(polls[3].time - polls[2].time >= std::chrono::seconds{5});
    }
    SECTION("is denied") {
        google.Answer(DeviceCodeEndpoint, 200, Code);
        google.Answer(TokenEndpoint, 403, R"({"error":"access_denied"})");
        REQUIRE(SignInWith(google, shown).status == SignInStatus::Denied);
        REQUIRE_FALSE(SignedIn());
    }
    SECTION("expires") {
        google.Answer(DeviceCodeEndpoint, 200, Code);
        google.Answer(TokenEndpoint, 428, R"({"error":"authorization_pending"})");
        google.Answer(TokenEndpoint, 400, R"({"error":"expired_token"})");
        REQUIRE(SignInWith(google, shown).status == SignInStatus::Expired);
        REQUIRE(google.Hits(TokenEndpoint) == 2);
        REQUIRE_FALSE(SignedIn());
    }
    SECTION("expires while the code is shown") {
        google.Answer(DeviceCodeEndpoint, 200,
                      R"({"device_code":"device.1","user_code":"ABCD-EFGH",)"
                      R"("verification_url":"u","expires_in":1,"interval":0})");
        REQUIRE(ImportClientSecret(FakeGoogle::ClientSecret("client.id", "client.secret")));
        const auto result = SignIn(
            [](const DeviceCode&) { std::this_thread::sleep_for(std::chrono::milliseconds{1100}); },
            {});
        REQUIRE(result.status == SignInStatus::Expired);
        REQUIRE(google.Hits(TokenEndpoint) == 0);
    }
    SECTION("is cancelled") {
        google.Answer(DeviceCodeEndpoint, 200,
                      R"({"device_code":"device.1","user_code":"ABCD-EFGH",)"
                      R"("verification_url":"u","expires_in":1800,"interval":60})");
        std::stop_source stop;
        std::jthread canceller{[&stop] {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            stop.request_stop();
        }};
        const auto started = std::chrono::steady_clock::now();
        REQUIRE(SignInWith(google, shown, stop.get_token()).status == SignInStatus::Cancelled);
        REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::seconds{10});
        REQUIRE(google.Hits(TokenEndpoint) == 0);
    }
}

TEST_CASE("CloudSync: the access token is refreshed before it expires", "[cloud_sync]") {
    FakeGoogle google;
    REQUIRE(ImportClientSecret(FakeGoogle::ClientSecret("client.id", "client.secret")));
    google.Answer(TokenEndpoint, 200, R"({"access_token":"access.new","expires_in":3599})");

    google.SignIn(3600);
    REQUIRE(AccessToken() == "access.old");
    REQUIRE(google.Hits(TokenEndpoint) == 0);

    google.SignIn(60);
    REQUIRE(AccessToken() == "access.new");
    const auto refresh = google.Requests(TokenEndpoint).at(0);
    REQUIRE(Param(refresh, "grant_type") == "refresh_token");
    REQUIRE(Param(refresh, "refresh_token") == "refresh.old");
    REQUIRE(Param(refresh, "client_id") == "client.id");
    // The refresh token is kept when Google sends none, and the new access token is stored.
    REQUIRE(AccessToken() == "access.new");
    REQUIRE(google.Hits(TokenEndpoint) == 1);

    REQUIRE(AccessToken(true) == "access.new");
    REQUIRE(Param(google.Requests(TokenEndpoint).at(1), "refresh_token") == "refresh.old");
    REQUIRE(google.Hits(TokenEndpoint) == 2);
}

TEST_CASE("CloudSync: a refused refresh token signs out", "[cloud_sync]") {
    FakeGoogle google;
    REQUIRE(ImportClientSecret(FakeGoogle::ClientSecret("client.id", "client.secret")));
    google.SignIn(0);

    SECTION("invalid_grant") {
        google.Answer(TokenEndpoint, 400, R"({"error":"invalid_grant","error_description":"Bad"})");
        REQUIRE_FALSE(AccessToken().has_value());
        REQUIRE_FALSE(SignedIn());
        REQUIRE_FALSE(AccessToken().has_value());
        REQUIRE(google.Hits(TokenEndpoint) == 1);
    }
    SECTION("but a server error keeps the sign-in") {
        google.Answer(TokenEndpoint, 500, "{}");
        REQUIRE_FALSE(AccessToken().has_value());
        REQUIRE(SignedIn());
    }
}

TEST_CASE("CloudSync: a token isn't refreshed with another client", "[cloud_sync]") {
    FakeGoogle google;
    std::vector<DeviceCode> shown;
    google.Answer(DeviceCodeEndpoint, 200, Code);
    google.Answer(TokenEndpoint, 200, Granted);
    REQUIRE(SignInWith(google, shown).status == SignInStatus::SignedIn);

    REQUIRE(ImportClientSecret(FakeGoogle::ClientSecret("other.id", "other.secret")));
    const auto refused = SessionAccessToken(true, std::nullopt);
    REQUIRE_FALSE(refused.access_token.has_value());
    REQUIRE(refused.error == "The sign-in is from another OAuth client; sign in again to use this "
                             "one");
    REQUIRE(google.Hits(TokenEndpoint) == 1);
    REQUIRE(SignedIn());

    REQUIRE(ImportClientSecret(FakeGoogle::ClientSecret("client.id", "client.secret")));
    REQUIRE(AccessToken(true) == "access.new");
    REQUIRE(Param(google.Requests(TokenEndpoint).at(1), "client_id") == "client.id");
}

TEST_CASE("CloudSync: a token saved without its client takes the one that refreshed it",
          "[cloud_sync]") {
    FakeGoogle google;
    REQUIRE(ImportClientSecret(FakeGoogle::ClientSecret("client.id", "client.secret")));
    google.SignIn(0);
    google.Answer(TokenEndpoint, 200, R"({"access_token":"access.new","expires_in":3599})");
    REQUIRE(AccessToken() == "access.new");

    REQUIRE(ImportClientSecret(FakeGoogle::ClientSecret("other.id", "other.secret")));
    const auto refused = SessionAccessToken(true, std::nullopt);
    REQUIRE_FALSE(refused.access_token.has_value());
    REQUIRE(refused.error == "The sign-in is from another OAuth client; sign in again to use this "
                             "one");
    REQUIRE(google.Hits(TokenEndpoint) == 1);
    REQUIRE(SignedIn());
    REQUIRE(std::filesystem::exists(google.directory / "cloud_sync" / "token.json"));
}

TEST_CASE("CloudSync: a code that arrives after sign-out isn't shown", "[cloud_sync]") {
    FakeGoogle google;
    google.Answer(DeviceCodeEndpoint, 200, Code, {}, std::chrono::milliseconds{500});
    std::vector<DeviceCode> shown;
    std::jthread signer_out{[&google](std::stop_token own_stop) {
        while (google.Hits(DeviceCodeEndpoint) == 0) {
            if (own_stop.stop_requested()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        SignOut();
    }};
    REQUIRE(SignInWith(google, shown).status == SignInStatus::Cancelled);
    REQUIRE(shown.empty());
    REQUIRE(google.Hits(TokenEndpoint) == 0);
}

TEST_CASE("CloudSync: sign-out revokes the refresh token", "[cloud_sync]") {
    FakeGoogle google;
    google.Answer(RevokeEndpoint, 200, "{}");
    google.SignIn(3600);
    SignOut();
    REQUIRE_FALSE(SignedIn());
    REQUIRE(Param(google.Requests(RevokeEndpoint).at(0), "token") == "refresh.old");
}

TEST_CASE("CloudSync: an imported client takes the build's place", "[cloud_sync]") {
    FakeGoogle google;
    const auto build = BuildClientId.empty() || BuildClientSecret.empty()
                           ? std::nullopt
                           : std::optional{ClientCredentials{std::string{BuildClientId},
                                                             std::string{BuildClientSecret}}};
    REQUIRE(LoadClientCredentials() == build);
    REQUIRE_FALSE(ClientImported());

    REQUIRE_FALSE(ImportClientSecret(R"({"web":{"client_id":"w","client_secret":"s"}})"));
    REQUIRE_FALSE(ImportClientSecret(R"({"installed":{"client_id":"i"}})"));
    REQUIRE_FALSE(ImportClientSecret("not json"));
    REQUIRE(LoadClientCredentials() == build);

    REQUIRE(ImportClientSecret(FakeGoogle::ClientSecret("user.id", "user.secret")));
    REQUIRE(LoadClientCredentials() == ClientCredentials{"user.id", "user.secret"});
    REQUIRE(ClientImported());

    // With no client at all, sign-in fails before asking Google.
    std::filesystem::remove(google.directory / "cloud_sync" / "client_secret.json");
    if (!build) {
        const auto result = SignIn([](const DeviceCode&) {}, {});
        REQUIRE(result.status == SignInStatus::Failed);
        REQUIRE(google.Hits(DeviceCodeEndpoint) == 0);
    }
}

TEST_CASE("CloudSync: a grant that arrives after cancel or sign-out isn't saved", "[cloud_sync]") {
    FakeGoogle google;
    google.Answer(DeviceCodeEndpoint, 200, Code);
    google.Answer(TokenEndpoint, 200, Granted, {}, std::chrono::milliseconds{500});
    std::vector<DeviceCode> shown;
    std::stop_source stop;
    // Stopped by its jthread's destructor if the sign-in fails before polling.
    const auto when_polled = [&google](auto action) {
        return std::jthread{[&google, action](std::stop_token own_stop) {
            while (google.Hits(TokenEndpoint) == 0) {
                if (own_stop.stop_requested()) {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
            action();
        }};
    };

    SECTION("cancel") {
        auto canceller = when_polled([&stop] { stop.request_stop(); });
        REQUIRE(SignInWith(google, shown, stop.get_token()).status == SignInStatus::Cancelled);
    }
    SECTION("sign-out") {
        auto signer_out = when_polled([] { SignOut(); });
        REQUIRE(SignInWith(google, shown).status == SignInStatus::Cancelled);
    }
    REQUIRE_FALSE(SignedIn());
}

TEST_CASE("CloudSync: a stop request ends a Google request right away", "[cloud_sync]") {
    FakeGoogle google;
    std::stop_source stop;
    std::vector<DeviceCode> shown;
    google.Answer(DeviceCodeEndpoint, 200, Code, {}, std::chrono::seconds{2});
    const auto start = std::chrono::steady_clock::now();
    // Stops once the request is out; its own stop ends the wait if the sign-in never sends it.
    std::atomic<bool> requested = false;
    std::jthread canceller{[&google, &stop, &requested](std::stop_token own_stop) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (google.Hits(DeviceCodeEndpoint) == 0) {
            if (own_stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        requested = true;
        stop.request_stop();
    }};
    const auto result = SignInWith(google, shown, stop.get_token());
    canceller.request_stop();
    canceller.join();
    REQUIRE(requested);
    REQUIRE(result.status == SignInStatus::Cancelled);
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
    REQUIRE(shown.empty());
}

TEST_CASE("CloudSync: a stop request ends the wait behind another refresh", "[cloud_sync]") {
    FakeGoogle google;
    REQUIRE(ImportClientSecret(FakeGoogle::ClientSecret("client.id", "client.secret")));
    google.SignIn(0);
    google.Answer(TokenEndpoint, 200, R"({"access_token":"access.new","expires_in":3599})", {},
                  std::chrono::seconds{2});
    std::jthread refresher{[] { AccessToken(true); }};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (google.Hits(TokenEndpoint) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    REQUIRE(google.Hits(TokenEndpoint) == 1);
    std::stop_source stop;
    std::jthread canceller{[&stop] {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        stop.request_stop();
    }};
    const auto start = std::chrono::steady_clock::now();
    REQUIRE_FALSE(AccessToken(false, stop.get_token()).has_value());
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
}

TEST_CASE("CloudSync: the sign-in snapshot doesn't wait for a refresh", "[cloud_sync]") {
    FakeGoogle google;
    REQUIRE(ImportClientSecret(FakeGoogle::ClientSecret("client.id", "client.secret")));
    google.SignIn(0);
    REQUIRE(SignedIn());
    google.Answer(TokenEndpoint, 200, R"({"access_token":"access.new","expires_in":3599})", {},
                  std::chrono::seconds{2});
    {
        std::jthread refresher{[] { AccessToken(true); }};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (google.Hits(TokenEndpoint) == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        REQUIRE(google.Hits(TokenEndpoint) == 1);
        const auto start = std::chrono::steady_clock::now();
        REQUIRE(SignedInSnapshot());
        REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::milliseconds{100});
    }
    google.Answer(RevokeEndpoint, 200, "{}");
    SignOut();
    REQUIRE_FALSE(SignedInSnapshot());
}

#ifndef _WIN32
TEST_CASE("CloudSync: a token that can't be saved", "[cloud_sync]") {
    FakeGoogle google;
    REQUIRE(ImportClientSecret(FakeGoogle::ClientSecret("client.id", "client.secret")));
    const auto folder = google.directory / "cloud_sync";
    SCOPE_EXIT {
        std::filesystem::permissions(folder, std::filesystem::perms::owner_all);
    };
    const auto lock_folder = [&folder] {
        std::filesystem::permissions(folder, std::filesystem::perms::owner_read |
                                                 std::filesystem::perms::owner_exec);
        if (CanWriteIn(folder)) {
            SKIP("Folder permissions don't keep this runner from writing");
        }
    };

    SECTION("fails the sign-in") {
        google.Answer(DeviceCodeEndpoint, 200, Code);
        google.Answer(TokenEndpoint, 200, Granted);
        lock_folder();
        std::vector<DeviceCode> shown;
        const auto result = SignIn([&shown](const DeviceCode& code) { shown.push_back(code); }, {});
        REQUIRE(result.status == SignInStatus::Failed);
        REQUIRE_FALSE(SignedIn());
    }
    SECTION("keeps a refreshed session in memory") {
        google.SignIn(60);
        google.Answer(TokenEndpoint, 200,
                      R"({"access_token":"access.new","refresh_token":"refresh.rotated",)"
                      R"("expires_in":3599})");
        lock_folder();
        REQUIRE(AccessToken() == "access.new");
        REQUIRE(AccessToken() == "access.new");
        REQUIRE(google.Hits(TokenEndpoint) == 1);
        REQUIRE(AccessToken(true) == "access.new");
        REQUIRE(Param(google.Requests(TokenEndpoint).at(1), "refresh_token") == "refresh.rotated");
    }
    std::filesystem::permissions(folder, std::filesystem::perms::owner_all);
    SignOut();
    REQUIRE_FALSE(SignedIn());
}

TEST_CASE("CloudSync: sign-out holds when the token can't be removed", "[cloud_sync]") {
    FakeGoogle google;
    REQUIRE(ImportClientSecret(FakeGoogle::ClientSecret("client.id", "client.secret")));
    const auto folder = google.directory / "cloud_sync";
    const auto token = folder / "token.json";
    SCOPE_EXIT {
        std::filesystem::permissions(folder, std::filesystem::perms::owner_all);
    };
    const auto lock_folder = [&folder] {
        std::filesystem::permissions(folder, std::filesystem::perms::owner_read |
                                                 std::filesystem::perms::owner_exec);
        if (CanWriteIn(folder)) {
            SKIP("Folder permissions don't keep this runner from removing files");
        }
    };

    SECTION("sign-out") {
        google.SignIn(3600);
        google.Answer(RevokeEndpoint, 200, "{}");
        lock_folder();
        REQUIRE_FALSE(SignOut());
    }
    SECTION("invalid_grant") {
        google.SignIn(0);
        google.Answer(TokenEndpoint, 400, R"({"error":"invalid_grant"})");
        lock_folder();
        REQUIRE_FALSE(AccessToken().has_value());
    }
    REQUIRE(std::filesystem::exists(token));
    REQUIRE_FALSE(SignedIn());
    REQUIRE_FALSE(AccessToken().has_value());

    // Removed once it can be.
    std::filesystem::permissions(folder, std::filesystem::perms::owner_all);
    REQUIRE_FALSE(SignedIn());
    REQUIRE_FALSE(std::filesystem::exists(token));
    REQUIRE(google.Hits(TokenEndpoint) <= 1);
}
#endif
