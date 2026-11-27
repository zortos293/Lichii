// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <vector>

#ifdef _WIN32
// Before windows.h, which otherwise brings in winsock.h and clashes with httplib's winsock2.h.
#include <winsock2.h>
#include <windows.h>

#include <sddl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/httplib.h"
#include "common/logging.h"
#include "common/net/ca_store.h"
#include "frontend_common/cloud_sync/build_client.h"
#include "frontend_common/cloud_sync/google_auth.h"

namespace FrontendCommon::CloudSync {

namespace {

constexpr std::string_view DeviceCodeGrant = "urn:ietf:params:oauth:grant-type:device_code";

std::mutex base_mutex;
std::string oauth_base{OAuthBase};
// Held while the token is read, refreshed and written, so one refresh token isn't spent twice.
// Timed, so that a caller with a stop token can stop waiting behind a slow refresh.
std::timed_mutex token_mutex;

struct Token {
    std::string access_token;
    std::string refresh_token;
    // Unix time.
    s64 expires_at{};
    // The client it was issued to. Empty in a token saved before it was kept.
    std::string client_id;
};

// Bumped by each sign-in and sign-out, so a sign-in that a newer one or a sign-out overtook
// doesn't save its token, and again when a sign-in saves it, so a request made with the old token
// isn't retried with the new one.
u64 session_generation = 0;
// A refreshed token the file couldn't take. Kept so a new refresh token isn't lost.
std::optional<Token> unsaved_token;
// Set when a sign-out couldn't remove token.json: the file is then ignored, and its removal
// retried, until a sign-in replaces it.
bool token_file_stale = false;
// The last sign-in state the token showed, for SignedInSnapshot: -1 before any, else 0 or 1.
std::atomic<int> signed_in_snapshot{-1};

std::filesystem::path CloudSyncDir() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "cloud_sync";
}

std::filesystem::path TokenFile() {
    return CloudSyncDir() / "token.json";
}

std::filesystem::path ClientSecretFile() {
    return CloudSyncDir() / "client_secret.json";
}

s64 Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

nlohmann::json ParseObject(std::string_view text) {
    auto json = nlohmann::json::parse(text, nullptr, false);
    return json.is_object() ? json : nlohmann::json::object();
}

std::string StringField(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

s64 IntField(const nlohmann::json& object, const char* key, s64 fallback = 0) {
    const auto it = object.find(key);
    return it != object.end() && it->is_number_integer() ? it->get<s64>() : fallback;
}

std::string ServerError(const nlohmann::json& reply) {
    const std::string description = StringField(reply, "error_description");
    return description.empty() ? StringField(reply, "error") : description;
}

#ifdef _WIN32
// A security descriptor whose DACL lets in the current user alone, without the folder's inherited
// entries. nullptr when it can't be made; else freed with LocalFree.
PSECURITY_DESCRIPTOR OwnerOnlyDescriptor() {
    HANDLE process_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &process_token)) {
        return nullptr;
    }
    DWORD size = 0;
    GetTokenInformation(process_token, TokenUser, nullptr, 0, &size);
    std::vector<u8> user(size);
    LPWSTR sid = nullptr;
    const bool found =
        size > 0 && GetTokenInformation(process_token, TokenUser, user.data(), size, &size) &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid);
    CloseHandle(process_token);
    if (!found) {
        return nullptr;
    }
    const std::wstring sddl = std::wstring{L"D:P(A;;FA;;;"} + sid + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                              &descriptor, nullptr)) {
        return nullptr;
    }
    return descriptor;
}
#endif

// The file holds a refresh token or the client secret, so only its owner may read it. It is
// written beside the old one and renamed over it, so a crash mid-write keeps the previous one.
bool WritePrivateFile(const std::filesystem::path& path, std::string_view text) {
    auto temp = path;
    temp += ".tmp";
    void(Common::FS::CreateParentDirs(path));
    void(Common::FS::RemoveFile(temp));
#ifdef _WIN32
    // Created owner-only rather than narrowed afterwards, so it is never readable by others, even
    // in a portable config folder others can read. The rename keeps the DACL.
    const PSECURITY_DESCRIPTOR descriptor = OwnerOnlyDescriptor();
    if (descriptor == nullptr) {
        return false;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(descriptor);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD done = 0;
    const bool written =
        ::WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &done, nullptr) &&
        done == text.size();
    const bool closed = CloseHandle(file) != 0;
#else
    // Created owner-only rather than narrowed afterwards, so it is never readable by others.
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    std::size_t done = 0;
    while (done < text.size()) {
        const ssize_t count = ::write(fd, text.data() + done, text.size() - done);
        if (count <= 0) {
            break;
        }
        done += static_cast<std::size_t>(count);
    }
    const bool written = done == text.size();
    const bool closed = ::close(fd) == 0;
#endif
    std::error_code ec;
    if (written && closed) {
        std::filesystem::rename(temp, path, ec);
    }
    if (!written || !closed || ec) {
        void(Common::FS::RemoveFile(temp));
        return false;
    }
    return true;
}

std::optional<Token> ReadTokenFile() {
    const auto json = ParseObject(
        Common::FS::ReadStringFromFile(TokenFile(), Common::FS::FileType::TextFile));
    Token token{
        .access_token = StringField(json, "access_token"),
        .refresh_token = StringField(json, "refresh_token"),
        .expires_at = IntField(json, "expires_at"),
        .client_id = StringField(json, "client_id"),
    };
    if (token.refresh_token.empty()) {
        return std::nullopt;
    }
    return token;
}

std::optional<Token> LoadTokenLocked() {
    std::optional<Token> token;
    if (unsaved_token) {
        token = unsaved_token;
    } else if (token_file_stale) {
        token_file_stale = !Common::FS::RemoveFile(TokenFile());
    } else {
        token = ReadTokenFile();
    }
    signed_in_snapshot = token.has_value();
    return token;
}

bool SaveTokenLocked(const Token& token) {
    const std::string text = nlohmann::json{
        {"access_token", token.access_token},
        {"refresh_token", token.refresh_token},
        {"expires_at", token.expires_at},
        {"client_id", token.client_id},
    }.dump();
    if (!WritePrivateFile(TokenFile(), text)) {
        LOG_ERROR(Frontend, "Could not save the Google Drive sign-in");
        return false;
    }
    unsaved_token.reset();
    token_file_stale = false;
    signed_in_snapshot = 1;
    return true;
}

// Signed out from now on, even when token.json can't be removed. False in that case.
bool ClearTokenLocked() {
    signed_in_snapshot = 0;
    unsaved_token.reset();
    token_file_stale = !Common::FS::RemoveFile(TokenFile());
    if (token_file_stale) {
        LOG_ERROR(Frontend, "Could not remove the Google Drive sign-in; it is ignored until the "
                            "next sign-in, but read again after a restart");
    }
    return !token_file_stale;
}

httplib::Result PostForm(std::string_view path, const httplib::Params& form,
                         std::stop_token stop = {}) {
    std::string base;
    {
        std::scoped_lock lock{base_mutex};
        base = oauth_base;
    }
    httplib::Client client{base};
    ConfigureClient(client);
    // Aborts the request in flight. A connection still being made isn't interrupted.
    const std::stop_callback abort{stop, [&client] { client.stop(); }};
    return client.Post(std::string{path}, httplib::Headers{{"User-Agent", "Lichii"}}, form);
}

bool CurrentSession(u64 generation) {
    std::scoped_lock lock{token_mutex};
    return generation == session_generation;
}

// Waits until the next poll. False when stop was requested.
bool WaitForPoll(std::chrono::seconds interval, std::stop_token stop) {
    std::mutex mutex;
    std::condition_variable_any wake;
    std::unique_lock lock{mutex};
    void(wake.wait_for(lock, stop, interval, [] { return false; }));
    return !stop.stop_requested();
}

std::optional<ClientCredentials> LoadImportedClient() {
    return ParseClientSecret(
        Common::FS::ReadStringFromFile(ClientSecretFile(), Common::FS::FileType::TextFile));
}

} // namespace

void SetOAuthBaseForTests(std::string base) {
    std::scoped_lock lock{base_mutex};
    oauth_base = std::move(base);
}

void ResetSignInForTests() {
    std::scoped_lock lock{token_mutex};
    unsaved_token.reset();
    token_file_stale = false;
    signed_in_snapshot = -1;
}

void ConfigureClient(httplib::Client& client) {
    constexpr time_t TimeoutSeconds = 30;
    client.set_connection_timeout(TimeoutSeconds);
    client.set_read_timeout(TimeoutSeconds);
    client.set_write_timeout(TimeoutSeconds);
    client.set_follow_location(true);
    Common::Net::ConfigureCaStore(client);
}

std::optional<ClientCredentials> ParseClientSecret(std::string_view json) {
    const auto root = ParseObject(json);
    const auto installed = root.find("installed");
    if (installed == root.end() || !installed->is_object()) {
        return std::nullopt;
    }
    ClientCredentials client{
        .client_id = StringField(*installed, "client_id"),
        .client_secret = StringField(*installed, "client_secret"),
    };
    if (client.client_id.empty() || client.client_secret.empty()) {
        return std::nullopt;
    }
    return client;
}

bool ImportClientSecret(std::string_view json) {
    if (!ParseClientSecret(json)) {
        return false;
    }
    if (!WritePrivateFile(ClientSecretFile(), json)) {
        LOG_ERROR(Frontend, "Could not save the Google OAuth client");
        return false;
    }
    return true;
}

std::optional<ClientCredentials> LoadClientCredentials() {
    if (auto imported = LoadImportedClient()) {
        return imported;
    }
    if (BuildClientId.empty() || BuildClientSecret.empty()) {
        return std::nullopt;
    }
    return ClientCredentials{std::string{BuildClientId}, std::string{BuildClientSecret}};
}

bool ClientImported() {
    return LoadImportedClient().has_value();
}

bool SignedIn() {
    std::scoped_lock lock{token_mutex};
    return LoadTokenLocked().has_value();
}

bool SignedInSnapshot() {
    if (const int known = signed_in_snapshot.load(); known >= 0) {
        return known == 1;
    }
    // Nothing has looked at the token yet: the file says, read without the lock.
    const bool signed_in = ReadTokenFile().has_value();
    int unknown = -1;
    signed_in_snapshot.compare_exchange_strong(unknown, signed_in);
    return signed_in_snapshot.load() == 1;
}

SignInResult SignIn(const std::function<void(const DeviceCode&)>& show_code, std::stop_token stop) {
    u64 generation = 0;
    {
        std::scoped_lock lock{token_mutex};
        generation = ++session_generation;
    }
    const auto client = LoadClientCredentials();
    if (!client) {
        return {SignInStatus::Failed, "This build has no Google OAuth client. Import a "
                                      "client_secret.json to sign in."};
    }
    const auto code_result =
        PostForm(DeviceCodePath,
                 {{"client_id", client->client_id}, {"scope", std::string{DriveScope}}}, stop);
    // Also when signed out or in again meanwhile: the code is then no one's to show.
    if (stop.stop_requested() || !CurrentSession(generation)) {
        return {SignInStatus::Cancelled, {}};
    }
    if (!code_result) {
        const std::string error = httplib::to_string(code_result.error());
        LOG_ERROR(Frontend, "Google device code request failed: {}", error);
        return {SignInStatus::Failed, fmt::format("Could not reach Google ({}).", error)};
    }
    const auto code = ParseObject(code_result->body);
    const std::string device_code = StringField(code, "device_code");
    if (code_result->status != 200 || device_code.empty()) {
        return {SignInStatus::Failed,
                fmt::format("Google refused the sign-in (HTTP {}): {}", code_result->status,
                            ServerError(code))};
    }
    const s64 expires_in = IntField(code, "expires_in");
    // From before show_code, which may take a while: the code expires meanwhile too.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{expires_in};
    show_code({.user_code = StringField(code, "user_code"),
               .verification_url = StringField(code, "verification_url"),
               .expires_in = expires_in});
    if (std::chrono::steady_clock::now() >= deadline) {
        return {SignInStatus::Expired, {}};
    }

    auto interval = std::chrono::seconds{IntField(code, "interval", 5)};
    const httplib::Params poll_form{{"client_id", client->client_id},
                                    {"client_secret", client->client_secret},
                                    {"device_code", device_code},
                                    {"grant_type", std::string{DeviceCodeGrant}}};
    while (true) {
        const auto left =
            std::chrono::ceil<std::chrono::seconds>(deadline - std::chrono::steady_clock::now());
        if (!WaitForPoll(std::min(interval, left), stop) || !CurrentSession(generation)) {
            return {SignInStatus::Cancelled, {}};
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return {SignInStatus::Expired, {}};
        }
        const auto poll = PostForm(TokenPath, poll_form, stop);
        if (stop.stop_requested() || !CurrentSession(generation)) {
            return {SignInStatus::Cancelled, {}};
        }
        if (!poll) {
            // Try again, less often (RFC 8628): the code is still good until the deadline.
            interval *= 2;
            continue;
        }
        const auto reply = ParseObject(poll->body);
        if (poll->status == 200) {
            Token token{
                .access_token = StringField(reply, "access_token"),
                .refresh_token = StringField(reply, "refresh_token"),
                .expires_at = Now() + IntField(reply, "expires_in"),
                .client_id = client->client_id,
            };
            if (token.refresh_token.empty()) {
                return {SignInStatus::Failed, "Google sent no refresh token."};
            }
            std::scoped_lock lock{token_mutex};
            // Cancelled, signed out or signed in again while the request was out.
            if (stop.stop_requested() || generation != session_generation) {
                return {SignInStatus::Cancelled, {}};
            }
            if (!SaveTokenLocked(token)) {
                return {SignInStatus::Failed, "Could not save the sign-in."};
            }
            ++session_generation;
            return {SignInStatus::SignedIn, {}};
        }
        const std::string error = StringField(reply, "error");
        if (error == "authorization_pending") {
            continue;
        }
        if (error == "slow_down") {
            interval += std::chrono::seconds{5};
            continue;
        }
        if (error == "access_denied") {
            return {SignInStatus::Denied, {}};
        }
        if (error == "expired_token") {
            return {SignInStatus::Expired, {}};
        }
        return {SignInStatus::Failed, fmt::format("Google refused the sign-in (HTTP {}): {}",
                                                  poll->status, ServerError(reply))};
    }
}

std::optional<std::string> AccessToken(bool force_refresh, std::stop_token stop) {
    return SessionAccessToken(force_refresh, std::nullopt, stop).access_token;
}

SessionToken SessionAccessToken(bool force_refresh, std::optional<u64> session,
                                std::stop_token stop) {
    std::unique_lock lock{token_mutex, std::defer_lock};
    while (!lock.try_lock_for(std::chrono::milliseconds{50})) {
        if (stop.stop_requested()) {
            return {.error = "Cancelled"};
        }
    }
    if (stop.stop_requested()) {
        return {.error = "Cancelled"};
    }
    if (session && *session != session_generation) {
        return {.error = "The Google Drive sign-in changed during the request"};
    }
    SessionToken reply{.session = session_generation};
    auto token = LoadTokenLocked();
    if (!token) {
        reply.error = "Not signed in to Google Drive";
        return reply;
    }
    if (!force_refresh && !token->access_token.empty() &&
        token->expires_at - RefreshAheadSeconds > Now()) {
        reply.access_token = token->access_token;
        return reply;
    }
    const auto client = LoadClientCredentials();
    if (!client) {
        reply.error = "No Google OAuth client to refresh the sign-in with";
        return reply;
    }
    // Google refuses another client's refresh token, and may take that as invalid_grant.
    if (!token->client_id.empty() && token->client_id != client->client_id) {
        LOG_WARNING(Frontend,
                    "The Google sign-in is from another OAuth client than the one in use");
        reply.error = "The sign-in is from another OAuth client; sign in again to use this one";
        return reply;
    }
    const auto result = PostForm(TokenPath,
                                 {{"client_id", client->client_id},
                                  {"client_secret", client->client_secret},
                                  {"refresh_token", token->refresh_token},
                                  {"grant_type", "refresh_token"}},
                                 stop);
    if (!result) {
        const std::string error = httplib::to_string(result.error());
        LOG_WARNING(Frontend, "Could not refresh the Google access token: {}", error);
        reply.error = fmt::format("Could not refresh the Google sign-in ({}); try again", error);
        return reply;
    }
    const auto json = ParseObject(result->body);
    if (result->status != 200) {
        if (StringField(json, "error") == "invalid_grant") {
            LOG_WARNING(Frontend, "Google refused the refresh token; signing out of Drive sync");
            ClearTokenLocked();
            reply.error = "Google refused the sign-in; sign in again";
        } else {
            LOG_WARNING(Frontend, "Google refused the access token refresh (HTTP {}): {}",
                        result->status, ServerError(json));
            reply.error = fmt::format("Could not refresh the Google sign-in (HTTP {}); try again",
                                      result->status);
        }
        return reply;
    }
    token->access_token = StringField(json, "access_token");
    if (token->access_token.empty()) {
        LOG_WARNING(Frontend, "Google sent no access token");
        reply.error = "Google sent no access token; try again";
        return reply;
    }
    token->expires_at = Now() + IntField(json, "expires_in");
    // Google keeps the refresh token unless it sends a new one.
    if (auto refresh_token = StringField(json, "refresh_token"); !refresh_token.empty()) {
        token->refresh_token = std::move(refresh_token);
    }
    // A token saved before the client was kept is now known to be this client's.
    token->client_id = client->client_id;
    if (!SaveTokenLocked(*token)) {
        unsaved_token = token;
    }
    reply.access_token = token->access_token;
    return reply;
}

bool SignOut() {
    std::optional<Token> token;
    bool removed = false;
    {
        std::scoped_lock lock{token_mutex};
        token = LoadTokenLocked();
        ++session_generation;
        removed = ClearTokenLocked();
    }
    if (token) {
        const auto result = PostForm(RevokePath, {{"token", token->refresh_token}});
        if (!result || result->status != 200) {
            LOG_WARNING(Frontend, "Could not revoke the Google Drive sign-in");
        }
    }
    return removed;
}

} // namespace FrontendCommon::CloudSync
