// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/httplib.h"
#include "common/logging.h"
#include "frontend_common/cloud_sync/drive_client.h"
#include "frontend_common/cloud_sync/google_auth.h"

namespace FrontendCommon::CloudSync::Drive {

namespace {

constexpr std::string_view FileFields =
    "id,name,parents,appProperties,createdTime,modifiedTime,md5Checksum,headRevisionId,trashed";
constexpr std::string_view JsonType = "application/json; charset=UTF-8";

std::mutex base_mutex;
std::string drive_base{DriveBase};
thread_local std::string last_error;

std::string Base() {
    std::scoped_lock lock{base_mutex};
    return drive_base;
}

std::string StringField(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

// A string literal in a files.list query.
std::string QueryString(std::string_view text) {
    std::string out = "'";
    for (const char c : text) {
        if (c == '\\' || c == '\'') {
            out += '\\';
        }
        out += c;
    }
    return out + "'";
}

std::string Query(const AppProperties& properties, std::string_view parent) {
    std::string query = "trashed = false";
    if (!parent.empty()) {
        query += fmt::format(" and {} in parents", QueryString(parent));
    }
    for (const auto& [key, value] : properties) {
        query += fmt::format(" and appProperties has {{ key={} and value={} }}", QueryString(key),
                             QueryString(value));
    }
    return query;
}

File ParseFile(const nlohmann::json& json) {
    File file{
        .id = StringField(json, "id"),
        .name = StringField(json, "name"),
        .created_time = StringField(json, "createdTime"),
        .modified_time = StringField(json, "modifiedTime"),
        .md5_checksum = StringField(json, "md5Checksum"),
        .head_revision_id = StringField(json, "headRevisionId"),
    };
    if (const auto trashed = json.find("trashed"); trashed != json.end() && trashed->is_boolean()) {
        file.trashed = trashed->get<bool>();
    }
    if (const auto parents = json.find("parents"); parents != json.end() && parents->is_array()) {
        for (const auto& parent : *parents) {
            if (parent.is_string()) {
                file.parents.push_back(parent.get<std::string>());
            }
        }
    }
    if (const auto properties = json.find("appProperties");
        properties != json.end() && properties->is_object()) {
        for (const auto& [key, value] : properties->items()) {
            if (value.is_string()) {
                file.app_properties.emplace(key, value.get<std::string>());
            }
        }
    }
    return file;
}

std::optional<File> ParseFileReply(const std::optional<httplib::Response>& reply) {
    if (!reply) {
        return std::nullopt;
    }
    const auto json = nlohmann::json::parse(reply->body, nullptr, false);
    if (!json.is_object() || StringField(json, "id").empty()) {
        LOG_ERROR(Frontend, "Drive sent a file without an id");
        last_error = "Drive sent a file without an id";
        return std::nullopt;
    }
    return ParseFile(json);
}

std::string DriveError(const std::string& body) {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_object()) {
        if (const auto error = json.find("error"); error != json.end() && error->is_object()) {
            return StringField(*error, "message");
        }
    }
    return {};
}

// A 403 for a rate limit, which passes like a 429.
bool RateLimited(const std::string& body) {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (!json.is_object()) {
        return false;
    }
    const auto error = json.find("error");
    if (error == json.end() || !error->is_object()) {
        return false;
    }
    const auto errors = error->find("errors");
    if (errors == error->end() || !errors->is_array()) {
        return false;
    }
    return std::ranges::any_of(*errors, [](const nlohmann::json& entry) {
        if (!entry.is_object()) {
            return false;
        }
        const std::string reason = StringField(entry, "reason");
        return reason == "userRateLimitExceeded" || reason == "rateLimitExceeded";
    });
}

using Request = std::function<httplib::Result(httplib::Client&, const httplib::Headers&)>;

// Waits for the retry. False when stop was requested.
bool WaitForRetry(std::chrono::seconds delay, std::stop_token stop) {
    std::mutex mutex;
    std::condition_variable_any wake;
    std::unique_lock lock{mutex};
    void(wake.wait_for(lock, stop, delay, [] { return false; }));
    return !stop.stop_requested();
}

// Made in the sign-in session when it is set, else in the current one, which it is then set to:
// a retry or the next page of a list never goes to another account.
std::optional<httplib::Response> Send(std::string_view what, const Request& request,
                                      std::stop_token stop, std::optional<u64>& session) {
    last_error.clear();
    const auto fail = [what](std::string error) -> std::optional<httplib::Response> {
        LOG_ERROR(Frontend, "Drive {} failed: {}", what, error);
        last_error = std::move(error);
        return std::nullopt;
    };
    auto token = SessionAccessToken(false, session, stop);
    if (!token.access_token) {
        return fail(stop.stop_requested() ? "Cancelled" : token.error);
    }
    session = token.session;
    bool refreshed = false;
    auto delay = std::chrono::seconds{1};
    for (int retry = 0;;) {
        if (stop.stop_requested()) {
            return fail("Cancelled");
        }
        httplib::Client client{Base()};
        ConfigureClient(client);
        // Aborts a request in flight. A connection still being made isn't interrupted.
        const std::stop_callback abort{stop, [&client] { client.stop(); }};
        const auto result = request(
            client, {{"Authorization", "Bearer " + *token.access_token}, {"User-Agent", "Lichii"}});
        if (stop.stop_requested()) {
            return fail("Cancelled");
        }
        if (!result) {
            return fail(httplib::to_string(result.error()));
        }
        if (result->status == 401 && !refreshed) {
            refreshed = true;
            token = SessionAccessToken(true, session, stop);
            if (!token.access_token) {
                return fail(stop.stop_requested() ? "Cancelled" : token.error);
            }
            continue;
        }
        if ((result->status == 429 || result->status >= 500 ||
             (result->status == 403 && RateLimited(result->body))) &&
            retry < Retries) {
            ++retry;
            if (!WaitForRetry(delay, stop)) {
                return fail("Cancelled");
            }
            delay *= 2;
            // A sign-out during the wait ends the retry.
            token = SessionAccessToken(false, session, stop);
            if (!token.access_token) {
                return fail(stop.stop_requested() ? "Cancelled" : token.error);
            }
            continue;
        }
        if (result->status < 200 || result->status >= 300) {
            const std::string message = DriveError(result->body);
            return fail(message.empty() ? fmt::format("HTTP {}", result->status)
                                        : fmt::format("HTTP {}: {}", result->status, message));
        }
        return *result;
    }
}

std::optional<httplib::Response> Send(std::string_view what, const Request& request,
                                      std::stop_token stop) {
    std::optional<u64> session;
    return Send(what, request, stop, session);
}

// A resumable upload, sent in one request after the one that starts it. Unlike a multipart
// upload, it has no 5 MB limit. A failed upload starts over.
httplib::Result UploadMedia(httplib::Client& client, const httplib::Headers& headers, bool update,
                            const std::string& path, const nlohmann::json& metadata,
                            std::string_view mime_type, std::span<const u8> data,
                            std::stop_token stop) {
    auto start_headers = headers;
    start_headers.emplace("X-Upload-Content-Type", std::string{mime_type});
    start_headers.emplace("X-Upload-Content-Length", std::to_string(data.size()));
    const std::string body = metadata.dump();
    auto started = update ? client.Patch(path, start_headers, body, std::string{JsonType})
                          : client.Post(path, start_headers, body, std::string{JsonType});
    if (!started || started->status != 200) {
        return started;
    }
    // The session URL is absolute, normally on the same host.
    const std::string location = started->get_header_value("Location");
    const auto host_start = location.find("://");
    const auto path_start =
        location.find('/', host_start == std::string::npos ? 0 : host_start + 3);
    if (location.empty() || path_start == std::string::npos) {
        LOG_ERROR(Frontend, "Drive started an upload without a session URL");
        return httplib::Result{nullptr, httplib::Error::Unknown};
    }
    httplib::Client session{location.substr(0, path_start)};
    ConfigureClient(session);
    const std::stop_callback abort{stop, [&session] { session.stop(); }};
    return session.Put(location.substr(path_start), headers,
                       reinterpret_cast<const char*>(data.data()), data.size(),
                       std::string{mime_type});
}

std::optional<File> LookUpFile(std::string_view id, std::stop_token stop,
                               std::optional<u64>& session) {
    return ParseFileReply(Send(
        "file lookup",
        [&](auto& client, const auto& headers) {
            return client.Get(fmt::format("/drive/v3/files/{}?fields={}", id, FileFields), headers);
        },
        stop, session));
}

nlohmann::json PropertiesJson(const AppProperties& properties) {
    nlohmann::json json = nlohmann::json::object();
    for (const auto& [key, value] : properties) {
        json[key] = value;
    }
    return json;
}

} // namespace

void SetDriveBaseForTests(std::string base) {
    std::scoped_lock lock{base_mutex};
    drive_base = std::move(base);
}

std::string LastError() {
    return last_error;
}

std::optional<std::vector<File>> FindFiles(const AppProperties& properties, std::string_view parent,
                                           std::stop_token stop) {
    const std::string query = Query(properties, parent);
    std::vector<File> files;
    std::string page_token;
    std::optional<u64> session;
    do {
        httplib::Params params{{"q", query},
                               {"fields", fmt::format("nextPageToken,files({})", FileFields)},
                               {"pageSize", "1000"}};
        if (!page_token.empty()) {
            params.emplace("pageToken", page_token);
        }
        const auto reply = Send(
            "file search",
            [&](auto& client, const auto& headers) {
                return client.Get("/drive/v3/files", params, headers);
            },
            stop, session);
        if (!reply) {
            return std::nullopt;
        }
        const auto json = nlohmann::json::parse(reply->body, nullptr, false);
        if (!json.is_object()) {
            LOG_ERROR(Frontend, "Drive sent an unreadable file list");
            last_error = "Drive sent an unreadable file list";
            return std::nullopt;
        }
        if (const auto list = json.find("files"); list != json.end() && list->is_array()) {
            for (const auto& file : *list) {
                files.push_back(ParseFile(file));
            }
        }
        page_token = StringField(json, "nextPageToken");
    } while (!page_token.empty());
    return files;
}

std::optional<File> CreateFolder(std::string_view name, std::string_view parent,
                                 const AppProperties& properties, std::stop_token stop) {
    // With an id of its own, so that a retry after a 5xx Drive made it anyway can't make another.
    std::optional<u64> session;
    const auto ids = Send(
        "folder id",
        [&](auto& client, const auto& headers) {
            return client.Get("/drive/v3/files/generateIds",
                              httplib::Params{{"count", "1"}, {"space", "drive"}}, headers);
        },
        stop, session);
    if (!ids) {
        return std::nullopt;
    }
    const auto ids_json = nlohmann::json::parse(ids->body, nullptr, false);
    const auto id_list = ids_json.is_object() ? ids_json.find("ids") : ids_json.end();
    if (id_list == ids_json.end() || !id_list->is_array() || id_list->empty() ||
        !id_list->front().is_string()) {
        LOG_ERROR(Frontend, "Drive sent no file id");
        last_error = "Drive sent no file id";
        return std::nullopt;
    }
    const std::string id = id_list->front().get<std::string>();
    nlohmann::json metadata{{"id", id},
                            {"name", name},
                            {"mimeType", FolderMimeType},
                            {"appProperties", PropertiesJson(properties)}};
    if (!parent.empty()) {
        metadata["parents"] = {parent};
    }
    auto folder = ParseFileReply(Send(
        "folder creation",
        [&](auto& client, const auto& headers) {
            return client.Post(fmt::format("/drive/v3/files?fields={}", FileFields), headers,
                               metadata.dump(), std::string{JsonType});
        },
        stop, session));
    if (folder || stop.stop_requested()) {
        return folder;
    }
    // A retry is refused when an attempt Drive answered with a 5xx made the folder: it is there.
    const std::string error = last_error;
    if (auto made = LookUpFile(id, stop, session); made && !made->trashed) {
        return made;
    }
    last_error = error;
    return std::nullopt;
}

std::optional<File> UploadFile(std::string_view name, std::string_view parent,
                               const AppProperties& properties, std::string_view mime_type,
                               std::span<const u8> data, std::stop_token stop) {
    nlohmann::json metadata{
        {"name", name}, {"mimeType", mime_type}, {"appProperties", PropertiesJson(properties)}};
    if (!parent.empty()) {
        metadata["parents"] = {parent};
    }
    return ParseFileReply(Send(
        "upload",
        [&](auto& client, const auto& headers) {
            return UploadMedia(
                client, headers, false,
                fmt::format("/upload/drive/v3/files?uploadType=resumable&fields={}", FileFields),
                metadata, mime_type, data, stop);
        },
        stop));
}

std::optional<File> UpdateFile(std::string_view id, const AppProperties& properties,
                               std::string_view mime_type, std::span<const u8> data,
                               std::stop_token stop) {
    const nlohmann::json metadata{{"appProperties", PropertiesJson(properties)}};
    return ParseFileReply(Send(
        "update",
        [&](auto& client, const auto& headers) {
            return UploadMedia(
                client, headers, true,
                fmt::format("/upload/drive/v3/files/{}?uploadType=resumable&fields={}", id,
                            FileFields),
                metadata, mime_type, data, stop);
        },
        stop));
}

std::optional<File> GetFile(std::string_view id, std::stop_token stop) {
    std::optional<u64> session;
    return LookUpFile(id, stop, session);
}

std::optional<std::vector<u8>> DownloadFile(std::string_view id, std::stop_token stop) {
    // Received in parts: httplib refuses a response body over 100 MiB unless a receiver takes it.
    // An error reply's body is kept for its message instead.
    std::vector<u8> data;
    const auto reply = Send(
        "download",
        [&](auto& client, const auto& headers) {
            data.clear();
            std::string error_body;
            bool succeeded = false;
            auto result = client.Get(
                fmt::format("/drive/v3/files/{}?alt=media", id), headers,
                [&succeeded](const httplib::Response& response) {
                    succeeded = response.status >= 200 && response.status < 300;
                    return true;
                },
                [&](const char* part, std::size_t size) {
                    if (succeeded) {
                        data.insert(data.end(), part, part + size);
                    } else {
                        error_body.append(part, size);
                    }
                    return true;
                });
            if (result && !succeeded) {
                result->body = std::move(error_body);
            }
            return result;
        },
        stop);
    if (!reply) {
        return std::nullopt;
    }
    return data;
}

std::optional<File> RenameFile(std::string_view id, std::string_view name, std::stop_token stop) {
    const nlohmann::json metadata{{"name", name}};
    return ParseFileReply(Send(
        "rename",
        [&](auto& client, const auto& headers) {
            return client.Patch(fmt::format("/drive/v3/files/{}?fields={}", id, FileFields),
                                headers, metadata.dump(), std::string{JsonType});
        },
        stop));
}

} // namespace FrontendCommon::CloudSync::Drive
