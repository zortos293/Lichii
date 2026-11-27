// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/httplib.h"
#include "frontend_common/cloud_sync/drive_client.h"
#include "tests/frontend_common/fake_google.h"

namespace Tests {

// A Drive on 127.0.0.1 that keeps its files, for the sync engine: the files.list queries
// drive_client makes (trashed, parent and appProperties terms), folder creation, resumable
// uploads and updates, downloads and renames. Signed in through FakeGoogle, whose ConfigDir it
// shares.
class FakeDriveServer {
public:
    struct File {
        std::string id;
        std::string name;
        std::string mime_type;
        std::vector<std::string> parents;
        std::map<std::string, std::string> app_properties;
        std::string data;
        std::string created_time;
        std::string modified_time;
        // Times the contents were replaced in place.
        int revisions{};
        bool trashed{};
        // Left out of searches, as Drive may for a moment after a change.
        bool hidden{};
    };

    FakeDriveServer() {
        REQUIRE(FrontendCommon::CloudSync::ImportClientSecret(
            FakeGoogle::ClientSecret("id", "secret")));
        google.SignIn(3600);

        server.Get("/drive/v3/files", [this](const auto& request, auto& response) {
            ++list_requests;
            std::this_thread::sleep_for(list_delay.load());
            // Refused, not asserted: this is the server's thread, and the sync reports the error.
            const auto query = ParseQuery(request.get_param_value("q"));
            if (!query) {
                response.status = 400;
                return;
            }
            std::scoped_lock lock{mutex};
            nlohmann::json list = nlohmann::json::array();
            for (const auto& [id, file] : files) {
                if (!file.hidden && Matches(file, *query)) {
                    list.push_back(Json(file));
                }
            }
            Reply(response, {{"files", list}});
        });
        server.Get("/drive/v3/files/generateIds", [this](const auto&, auto& response) {
            std::scoped_lock lock{mutex};
            Reply(response, {{"ids", {fmt::format("generated.{}", ++generated)}}});
        });
        server.Post("/drive/v3/files", [this](const auto& request, auto& response) {
            std::scoped_lock lock{mutex};
            const auto metadata = nlohmann::json::parse(request.body);
            if (files.contains(metadata.value("id", ""))) {
                response.status = 409;
                return;
            }
            Reply(response, Json(Create(metadata, {})));
        });
        server.Post("/upload/drive/v3/files", [this](const auto& request, auto& response) {
            std::scoped_lock lock{mutex};
            StartSession(response, {{}, nlohmann::json::parse(request.body)});
        });
        server.Patch(
            R"(/upload/drive/v3/files/([^/]+))", [this](const auto& request, auto& response) {
                if (const int status = update_status.load()) {
                    response.status = status;
                    return;
                }
                std::scoped_lock lock{mutex};
                StartSession(response, {request.matches[1], nlohmann::json::parse(request.body)});
            });
        server.Put(R"(/upload/session/(\d+))", [this](const auto& request, auto& response) {
            std::scoped_lock lock{mutex};
            const Session session = sessions.at(std::stoi(request.matches[1]));
            if (session.file_id.empty()) {
                Reply(response, Json(Create(session.metadata, request.body)));
                return;
            }
            File& file = files.at(session.file_id);
            for (const auto& [key, value] : session.metadata["appProperties"].items()) {
                file.app_properties[key] = value.template get<std::string>();
            }
            file.data = request.body;
            file.modified_time = NextTime();
            ++file.revisions;
            Reply(response, Json(file));
        });
        server.Get(R"(/drive/v3/files/([^/]+))", [this](const auto& request, auto& response) {
            const std::string id = request.matches[1];
            if (request.get_param_value("alt") == "media") {
                std::scoped_lock lock{mutex};
                response.set_content(files.at(id).data, "application/octet-stream");
                return;
            }
            if (before_get) {
                before_get(id);
            }
            std::scoped_lock lock{mutex};
            if (!files.contains(id)) {
                response.status = 404;
                return;
            }
            Reply(response, Json(files.at(id)));
        });
        server.Patch(R"(/drive/v3/files/([^/]+))", [this](const auto& request, auto& response) {
            std::scoped_lock lock{mutex};
            File& file = files.at(request.matches[1]);
            file.name = nlohmann::json::parse(request.body).at("name");
            Reply(response, Json(file));
        });

        port = server.bind_to_any_port("127.0.0.1");
        REQUIRE(port > 0);
        thread = std::thread{[this] { void(server.listen_after_bind()); }};
        server.wait_until_ready();
        FrontendCommon::CloudSync::Drive::SetDriveBaseForTests(Base());
    }

    ~FakeDriveServer() {
        server.stop();
        thread.join();
        FrontendCommon::CloudSync::Drive::SetDriveBaseForTests(
            std::string{FrontendCommon::CloudSync::Drive::DriveBase});
    }

    std::string Base() const {
        return fmt::format("http://127.0.0.1:{}", port);
    }

    // The files not in the trash with all of the properties.
    std::vector<File> Find(const std::map<std::string, std::string>& properties) {
        std::scoped_lock lock{mutex};
        std::vector<File> found;
        for (const auto& [id, file] : files) {
            if (!file.trashed && std::ranges::all_of(properties, [&](const auto& property) {
                    const auto it = file.app_properties.find(property.first);
                    return it != file.app_properties.end() && it->second == property.second;
                })) {
                found.push_back(file);
            }
        }
        return found;
    }

    File Get(const std::string& id) {
        std::scoped_lock lock{mutex};
        return files.at(id);
    }

    // Adds a file as another machine would; returns its id.
    std::string Add(File file) {
        std::scoped_lock lock{mutex};
        file.id = fmt::format("file.{}", files.size() + 1);
        if (file.created_time.empty()) {
            file.created_time = NextTime();
        }
        file.modified_time = NextTime();
        const std::string id = file.id;
        files[id] = std::move(file);
        return id;
    }

    // Changes a file as another machine or the user on drive.google.com would.
    template <typename Change>
    void Edit(const std::string& id, Change&& change) {
        std::scoped_lock lock{mutex};
        File& file = files.at(id);
        change(file);
        file.modified_time = NextTime();
    }

    std::size_t Count() {
        std::scoped_lock lock{mutex};
        return files.size();
    }

    FakeGoogle google;
    // How long file searches take to answer.
    std::atomic<std::chrono::milliseconds> list_delay{};
    // File searches received, counted before list_delay.
    std::atomic<int> list_requests{};
    // When not 0, the status every update (UpdateFile) is refused with.
    std::atomic<int> update_status{};
    // Called with the id before a metadata lookup (GetFile) is answered.
    std::function<void(const std::string&)> before_get;

private:
    struct Session {
        std::string file_id;
        nlohmann::json metadata;
    };

    static std::string Unescape(const std::string& text) {
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) {
                ++i;
            }
            out += text[i];
        }
        return out;
    }

    struct Query {
        std::vector<std::string> parents;
        std::vector<std::pair<std::string, std::string>> properties;
    };

    // The terms of a files.list query, nullopt when any of it isn't one the fake knows.
    static std::optional<Query> ParseQuery(const std::string& query) {
        static const std::regex term{
            R"( and (?:'((?:[^'\\]|\\.)*)' in parents|)"
            R"(appProperties has \{ key='((?:[^'\\]|\\.)*)' and value='((?:[^'\\]|\\.)*)' \}))"};
        constexpr std::string_view trashed = "trashed = false";
        if (!query.starts_with(trashed)) {
            return std::nullopt;
        }
        Query parsed;
        std::smatch match;
        for (auto it = query.begin() + trashed.size(); it != query.end(); it = match[0].second) {
            if (!std::regex_search(it, query.end(), match, term,
                                   std::regex_constants::match_continuous)) {
                return std::nullopt;
            }
            if (match[1].matched) {
                parsed.parents.push_back(Unescape(match[1]));
            } else {
                parsed.properties.emplace_back(Unescape(match[2]), Unescape(match[3]));
            }
        }
        return parsed;
    }

    static bool Matches(const File& file, const Query& query) {
        if (file.trashed) {
            return false;
        }
        for (const auto& parent : query.parents) {
            if (std::ranges::find(file.parents, parent) == file.parents.end()) {
                return false;
            }
        }
        for (const auto& [key, value] : query.properties) {
            const auto it = file.app_properties.find(key);
            if (it == file.app_properties.end() || it->second != value) {
                return false;
            }
        }
        return true;
    }

    static nlohmann::json Json(const File& file) {
        return {{"id", file.id},
                {"name", file.name},
                {"parents", file.parents},
                {"appProperties", file.app_properties},
                {"createdTime", file.created_time},
                {"modifiedTime", file.modified_time},
                {"headRevisionId", fmt::format("rev.{}", file.revisions)},
                {"trashed", file.trashed}};
    }

    static void Reply(httplib::Response& response, const nlohmann::json& json) {
        response.set_content(json.dump(), "application/json");
    }

    // A minute apart, so that conflicts show distinct times.
    std::string NextTime() {
        ++clock;
        return fmt::format("2026-11-20T{:02}:{:02}:00.000Z", clock / 60 % 24, clock % 60);
    }

    const File& Create(const nlohmann::json& metadata, std::string data) {
        File file{
            .id = metadata.value("id", fmt::format("file.{}", files.size() + 1)),
            .name = metadata.at("name"),
            .mime_type = metadata.at("mimeType"),
            .data = std::move(data),
            .created_time = NextTime(),
        };
        file.modified_time = file.created_time;
        if (metadata.contains("parents")) {
            file.parents = metadata["parents"].get<std::vector<std::string>>();
        }
        for (const auto& [key, value] : metadata["appProperties"].items()) {
            file.app_properties[key] = value.get<std::string>();
        }
        return files[file.id] = std::move(file);
    }

    void StartSession(httplib::Response& response, Session session) {
        sessions.push_back(std::move(session));
        response.set_header("Location",
                            fmt::format("{}/upload/session/{}", Base(), sessions.size() - 1));
    }

    httplib::Server server;
    std::thread thread;
    int port{};
    std::mutex mutex;
    std::map<std::string, File> files;
    std::vector<Session> sessions;
    int clock{};
    // File ids handed out by generateIds.
    int generated{};
};

} // namespace Tests
