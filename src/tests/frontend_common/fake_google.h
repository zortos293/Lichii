// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/fs/path_util.h"
#include "common/httplib.h"
#include "common/scope_exit.h"
#include "frontend_common/cloud_sync/google_auth.h"
#include "tests/common/restore_eden_path.h"

namespace Tests {

// Answers Google's OAuth and Drive endpoints on 127.0.0.1, with ConfigDir in a temporary folder.
// Each "METHOD path" replies with its queued answers in order and repeats the last one.
class FakeGoogle {
public:
    struct Request {
        std::string body;
        httplib::Params params;
        httplib::Headers headers;
        std::chrono::steady_clock::time_point time;
    };

    FakeGoogle() {
        directory = std::filesystem::temp_directory_path() /
                    fmt::format("lichii-cloud-sync-google-{}", std::random_device{}());
        std::filesystem::create_directories(directory);
        // The destructor doesn't run when the server fails to start.
        auto remove_directory = SCOPE_GUARD {
            std::error_code ec;
            std::filesystem::remove_all(directory, ec);
        };

        const auto handle = [this](const httplib::Request& request, httplib::Response& response) {
            Canned reply{404, "{}", {}, {}, {}};
            {
                std::scoped_lock lock{mutex};
                const std::string key = request.method + " " + request.path;
                requests[key].push_back({request.body, request.params, request.headers,
                                         std::chrono::steady_clock::now()});
                auto& queue = replies[key];
                if (!queue.empty()) {
                    reply = queue.front();
                }
                if (queue.size() > 1) {
                    queue.pop_front();
                }
            }
            std::this_thread::sleep_for(reply.delay);
            // At most 5 s, so that a test whose release never comes still ends.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (reply.release && !reply.release() &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
            response.status = reply.status;
            for (const auto& [name, value] : reply.headers) {
                response.set_header(name, value);
            }
            response.set_content(reply.body, "application/json");
        };
        server.Get(".*", handle);
        server.Post(".*", handle);
        server.Put(".*", handle);
        server.Patch(".*", handle);
        port = server.bind_to_any_port("127.0.0.1");
        REQUIRE(port > 0);
        thread = std::thread{[this] { void(server.listen_after_bind()); }};
        server.wait_until_ready();
        previous_config = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir);
        Common::FS::SetEdenPath(Common::FS::EdenPath::ConfigDir, directory);
        FrontendCommon::CloudSync::SetOAuthBaseForTests(Base());
        FrontendCommon::CloudSync::ResetSignInForTests();
        remove_directory.Cancel();
    }

    ~FakeGoogle() {
        server.stop();
        thread.join();
        FrontendCommon::CloudSync::SetOAuthBaseForTests(
            std::string{FrontendCommon::CloudSync::OAuthBase});
        FrontendCommon::CloudSync::ResetSignInForTests();
        Tests::RestoreEdenPath(Common::FS::EdenPath::ConfigDir, previous_config);
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
    }

    std::string Base() const {
        return fmt::format("http://127.0.0.1:{}", port);
    }

    // release, when set, holds the reply until it returns true.
    void Answer(const std::string& key, int status, std::string body, httplib::Headers headers = {},
                std::chrono::milliseconds delay = {}, std::function<bool()> release = {}) {
        std::scoped_lock lock{mutex};
        replies[key].push_back(
            {status, std::move(body), std::move(headers), delay, std::move(release)});
    }

    std::vector<Request> Requests(const std::string& key) {
        std::scoped_lock lock{mutex};
        return requests[key];
    }

    std::size_t Hits(const std::string& key) {
        return Requests(key).size();
    }

    // A client_secret.json with an "installed" client.
    static std::string ClientSecret(std::string_view id, std::string_view secret) {
        return nlohmann::json{{"installed", {{"client_id", id}, {"client_secret", secret}}}}
            .dump();
    }

    // A signed-in account whose access token expires in expires_in seconds.
    void SignIn(s64 expires_in) const {
        const s64 now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
        std::filesystem::create_directories(directory / "cloud_sync");
        std::ofstream{directory / "cloud_sync" / "token.json"}
            << nlohmann::json{{"access_token", "access.old"},
                              {"refresh_token", "refresh.old"},
                              {"expires_at", now + expires_in}}
                   .dump();
    }

    std::filesystem::path directory;

private:
    struct Canned {
        int status;
        std::string body;
        httplib::Headers headers;
        std::chrono::milliseconds delay;
        std::function<bool()> release;
    };

    std::filesystem::path previous_config;
    httplib::Server server;
    std::thread thread;
    int port{};
    std::mutex mutex;
    std::map<std::string, std::deque<Canned>> replies;
    std::map<std::string, std::vector<Request>> requests;
};

} // namespace Tests
