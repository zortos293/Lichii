// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>

#include <fmt/format.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/romm_api.h"
#include "common/romm_config.h"
#include "common/string_util.h"

namespace Common::RommApi {

namespace {

using json = nlohmann::json;

constexpr int TimeoutSeconds = 10;
// Content downloads can legitimately take minutes for a Switch base game; give those their own,
// much longer, read timeout instead of penalizing every other call.
constexpr int DownloadTimeoutSeconds = 60 * 30;

// One instance per call rather than a shared/static client: a RomM instance URL can change at any
// point from the Settings dialog, and a stale cached client would silently keep talking to the
// old host. Takes explicit
// credentials rather than reading global config so the same code path serves both the master
// account (catalogue) and whichever profile's account is active (sync) without either one being
// able to accidentally read the other's token.
httplib::Client MakeClient(const Common::RommConfig::Credentials& credentials,
                            int timeout_seconds = TimeoutSeconds) {
    httplib::Client client{credentials.instance_url};
    client.set_connection_timeout(timeout_seconds);
    client.set_read_timeout(timeout_seconds);
    // The access token is only ever attached via the Authorization header built in AuthHeaders(), on
    // requests issued by this client, to the exact host it was constructed with; httplib does not
    // carry that header across a cross-origin redirect, which is the property that matters here
    // (a malicious/misconfigured reverse proxy in front of someone's instance can't trick us into
    // leaking the token to another host the way it could if we manually followed redirects).
    client.set_follow_location(false);
    return client;
}

// ---- OAuth2 --------------------------------------------------------------------------------

s64 NowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// RomM reports "expires" as an integer. Whether that's a duration or an absolute
// Unix time isn't spelled out, so accept both: anything that looks like a timestamp is one.
s64 ToAbsoluteSeconds(s64 value) {
    return value > 1'000'000'000 ? value : NowSeconds() + value;
}

struct Session {
    std::string access_token;
    s64 access_expires_at = 0;
    // Only ever kept in memory. Lets us renew the access token without the password; when it's
    // gone or stops working we log in again with the password from the system credential store.
    std::string refresh_token;
};

std::mutex g_token_mutex;
std::map<std::string, Session> g_sessions;

// The scope is part of the key on purpose: the catalogue account (read-only scope) and a profile's
// cloud-save account (read + write saves and devices) can be the very same RomM user. Keyed by
// user alone they would share one session, and whichever logged in first would hand its narrow
// token to the other: saves then download fine (read) but registering the device and uploading
// fail with 403 Forbidden.
std::string SessionKey(const std::string& instance_url, const std::string& username,
                       const std::string& scope) {
    return instance_url + "|" + username + "|" + scope;
}

struct TokenResponse {
    std::string access_token;
    s64 access_expires_at = 0;
    std::string refresh_token; // Empty on a refresh-grant response.
    std::string error;         // Non-empty on failure.
};

// POSTs the form to /api/token and parses the TokenResponse (or RomM's error "detail").
TokenResponse RequestToken(const Common::RommConfig::Credentials& endpoint,
                           const httplib::Params& form) {
    TokenResponse out;
    auto client = MakeClient(endpoint);
    auto result = client.Post("/api/token", form);
    if (!result) {
        out.error = fmt::format("Connection failed: {}", httplib::to_string(result.error()));
        return out;
    }
    try {
        const json body = json::parse(result->body, nullptr, /*allow_exceptions=*/true);
        if (result->status < 200 || result->status >= 300) {
            std::string detail = body.is_object() && body.contains("detail") &&
                                         body.at("detail").is_string()
                                     ? body.at("detail").get<std::string>()
                                     : std::string{};
            out.error = detail.empty() ? fmt::format("HTTP {}", result->status) : detail;
            return out;
        }
        out.access_token = body.value("access_token", std::string{});
        out.access_expires_at = ToAbsoluteSeconds(body.value("expires", s64{0}));
        if (body.contains("refresh_token") && body.at("refresh_token").is_string()) {
            out.refresh_token = body.at("refresh_token").get<std::string>();
        }
        if (out.access_token.empty()) {
            out.error = "The server didn't return an access token";
        }
    } catch (const json::exception& e) {
        out.error = fmt::format("Unexpected response from the server (HTTP {})", result->status);
        LOG_WARNING(WebService, "RomM /api/token: failed to parse response: {}", e.what());
    }
    return out;
}

// A valid access token for this account. Order of attempts when the cached one is missing or
// about to run out: renew it with the in-memory refresh token; failing that, log in again with the
// password from the system credential store. Empty string if none of that works (not configured,
// password missing from the store, wrong password, instance unreachable) — callers then just get
// a 401 / connection error from the real request.
std::string AccessToken(const Common::RommConfig::Credentials& credentials) {
    if (!credentials.IsSet()) {
        return {};
    }
    const std::string key =
        SessionKey(credentials.instance_url, credentials.username, credentials.scope);

    std::lock_guard lock{g_token_mutex};
    constexpr s64 RefreshMarginSeconds = 30;
    Session& session = g_sessions[key];
    if (!session.access_token.empty() &&
        NowSeconds() + RefreshMarginSeconds < session.access_expires_at) {
        return session.access_token;
    }

    const auto apply = [&session](const TokenResponse& response) {
        session.access_token = response.access_token;
        session.access_expires_at = response.access_expires_at;
        if (!response.refresh_token.empty()) {
            session.refresh_token = response.refresh_token;
        }
    };

    if (!session.refresh_token.empty()) {
        httplib::Params form{
            {"grant_type", "refresh_token"},
            {"refresh_token", session.refresh_token},
        };
        if (!credentials.scope.empty()) {
            form.emplace("scope", credentials.scope);
        }
        const auto response = RequestToken(credentials, form);
        if (response.error.empty()) {
            apply(response);
            return session.access_token;
        }
        LOG_INFO(WebService, "RomM: refresh token no longer works for '{}' ({}), logging in again",
                 credentials.username, response.error);
        session.refresh_token.clear();
    }

    const auto password = Common::RommConfig::Secrets::GetPassword(credentials.secret_key);
    if (!password.has_value()) {
        LOG_WARNING(WebService, "RomM: no password in the credential store for '{}'",
                    credentials.username);
        g_sessions.erase(key);
        return {};
    }
    const auto response = RequestToken(credentials, httplib::Params{
                                                        {"grant_type", "password"},
                                                        {"username", credentials.username},
                                                        {"password", *password},
                                                        {"scope", credentials.scope},
                                                    });
    if (!response.error.empty()) {
        LOG_WARNING(WebService, "RomM: login failed for '{}': {}", credentials.username,
                    response.error);
        g_sessions.erase(key);
        return {};
    }
    apply(response);
    return session.access_token;
}

httplib::Headers AuthHeaders(const Common::RommConfig::Credentials& credentials) {
    return {{"Authorization", fmt::format("Bearer {}", AccessToken(credentials))}};
}

// nlohmann's .value(key, default) throws if the key exists but is JSON null rather than falling
// back to default — several of RomM's own fields are nullable, not just optional, so this is
// needed anywhere one of those is read.
std::string NullableString(const json& j, const char* key) {
    if (!j.contains(key) || j.at(key).is_null()) {
        return {};
    }
    return j.at(key).get<std::string>();
}

RomFileCategory ParseCategory(const std::string& s) {
    // Confirmed against RomM's own API client library (romm-cli's RomFileCategory enum, serialized
    // lowercase): the base-game value is "game", not "main"/"base" as first guessed here — every
    // other category (hack/manual/patch/mod/demo/translation/prototype/cheat) besides update/dlc
    // stays Unknown, since Lichii has nothing useful to do with them.
    if (s == "game") {
        return RomFileCategory::Main;
    }
    if (s == "update") {
        return RomFileCategory::Update;
    }
    if (s == "dlc") {
        return RomFileCategory::Dlc;
    }
    return RomFileCategory::Unknown;
}

SyncOpType ParseOpType(const std::string& s) {
    if (s == "upload") {
        return SyncOpType::Upload;
    }
    if (s == "download") {
        return SyncOpType::Download;
    }
    if (s == "conflict") {
        return SyncOpType::Conflict;
    }
    return SyncOpType::NoOp; // Covers the literal "no_op" too, which is the correct mapping for it.
}

} // Anonymous namespace

TestConnectionResult TestConnection(const Common::RommConfig::Credentials& credentials) {
    TestConnectionResult out;
    if (!credentials.IsSet()) {
        out.error = "No RomM instance configured";
        return out;
    }

    auto client = MakeClient(credentials);
    // GET /api/platforms is used purely as a lightweight "does auth + routing work at all" probe;
    // it's the endpoint most likely to exist unchanged across RomM versions, not because it's
    // what a real caller wants here.
    auto result = client.Get("/api/platforms", AuthHeaders(credentials));
    if (!result) {
        out.error = fmt::format("Connection failed: {}", httplib::to_string(result.error()));
        return out;
    }
    if (result->status == 401 || result->status == 403) {
        out.error = "Instance reachable, but the login was rejected (it may have expired: log in "
                    "again)";
        return out;
    }
    if (result->status < 200 || result->status >= 300) {
        out.error = fmt::format("Unexpected HTTP {}", result->status);
        return out;
    }

    out.ok = true;
    // Best-effort: some RomM versions expose this under a "version" header instead of the body,
    // so absence here isn't itself a failure.
    if (auto it = result->headers.find("X-Romm-Version"); it != result->headers.end()) {
        out.server_version = it->second;
    }
    return out;
}

LoginResult Login(const std::string& instance_url, const std::string& username,
                  const std::string& password, const std::string& scope) {
    LoginResult out;
    Common::RommConfig::Credentials endpoint;
    endpoint.instance_url = instance_url;
    while (!endpoint.instance_url.empty() && endpoint.instance_url.back() == '/') {
        endpoint.instance_url.pop_back();
    }
    if (endpoint.instance_url.empty() || username.empty() || password.empty()) {
        out.error = "Instance URL, username and password are all required";
        return out;
    }

    const auto response = RequestToken(endpoint, httplib::Params{
                                                     {"grant_type", "password"},
                                                     {"username", username},
                                                     {"password", password},
                                                     {"scope", scope},
                                                 });
    if (!response.error.empty()) {
        out.error = response.error;
        return out;
    }

    out.credentials = endpoint;
    out.credentials.username = username;
    out.credentials.scope = scope;
    out.ok = true;

    std::lock_guard lock{g_token_mutex};
    g_sessions[SessionKey(endpoint.instance_url, username, scope)] =
        Session{response.access_token, response.access_expires_at, response.refresh_token};
    return out;
}

void ForgetSession(const Common::RommConfig::Credentials& credentials) {
    std::lock_guard lock{g_token_mutex};
    g_sessions.erase(
        SessionKey(credentials.instance_url, credentials.username, credentials.scope));
}

std::vector<Platform> ListPlatforms() {
    std::vector<Platform> out;
    const auto credentials = Common::RommConfig::GetMasterCredentials();
    if (!credentials.IsSet()) {
        return out;
    }

    auto client = MakeClient(credentials);
    auto result = client.Get("/api/platforms", AuthHeaders(credentials));
    if (!result || result->status < 200 || result->status >= 300) {
        LOG_WARNING(WebService, "RomM ListPlatforms failed (HTTP {})",
                    result ? result->status : -1);
        return out;
    }

    try {
        const json body = json::parse(result->body);
        // Some RomM versions wrap list endpoints as {"items": [...]}, others return a bare array;
        // accept both defensively rather than picking one and breaking on the other.
        const json& items = body.is_array() ? body : body.value("items", json::array());
        for (const auto& p : items) {
            // Match on either slug or fs_slug — confirmed via RomM's own API client library that
            // both exist on a platform and can differ; if "switch" only matches one of them on a
            // given instance, matching just the other would silently find nothing.
            const std::string slug = p.value("slug", std::string{});
            const std::string fs_slug = p.value("fs_slug", std::string{});
            LOG_INFO(WebService, "RomM platform seen: id={} slug='{}' fs_slug='{}' rom_count={}",
                     p.value("id", u64{0}), slug, fs_slug, p.value("rom_count", u64{0}));
            // Only Switch and Switch 2 are relevant to Lichii. Test each slug on its own: "switch"
            // must not swallow "switch-2" (or the other way round) just because one field matches.
            std::string matched;
            for (const char* wanted : {SwitchPlatformSlug, Switch2PlatformSlug}) {
                if (slug == wanted || fs_slug == wanted) {
                    matched = wanted;
                    break;
                }
            }
            if (matched.empty()) {
                continue;
            }
            Platform platform;
            platform.id = p.value("id", u64{0});
            platform.slug = matched;
            platform.name = p.value(
                "name", p.value("display_name", std::string{matched == Switch2PlatformSlug
                                                                ? "Nintendo Switch 2"
                                                                : "Nintendo Switch"}));
            out.push_back(std::move(platform));
        }
    } catch (const json::exception& e) {
        LOG_WARNING(WebService, "RomM ListPlatforms: failed to parse response: {}", e.what());
    }
    return out;
}

namespace {

Rom ParseRom(const json& r) {
    Rom rom;
    rom.id = r.value("id", u64{0});
    rom.name = r.value("name", r.value("fs_name", std::string{}));
    rom.platform_slug = r.value("platform_slug", std::string{});
    const json& files = r.value("files", json::array());
    for (const auto& f : files) {
        RomFile file;
        file.id = f.value("id", u64{0});
        file.filename = f.value("file_name", std::string{});
        if (file.filename.empty()) {
            continue;
        }
        file.category = ParseCategory(f.value("category", std::string{"game"}));
        file.size_bytes = f.value("file_size_bytes", u64{0});
        rom.files.push_back(std::move(file));
    }
    // The list endpoint (SimpleRomSchema) never populates files[] at all — only the single-rom
    // detail endpoint (GetRom) does. Rather than guess at an undocumented query flag to get files
    // out of the list endpoint, callers are expected to call GetRom(id) once a specific title is
    // selected; this fallback just keeps ListRoms() usable standalone (e.g. for a future "download
    // whichever single file this legacy single-file rom has" shortcut) using the rom-level fields
    // that do exist on every schema variant.
    if (rom.files.empty()) {
        const std::string fallback_name = r.value("fs_name", std::string{});
        if (!fallback_name.empty()) {
            rom.files.push_back(RomFile{.filename = fallback_name,
                                        .category = RomFileCategory::Main,
                                        .size_bytes = r.value("fs_size_bytes", u64{0})});
        }
    }
    return rom;
}

} // namespace

std::vector<Rom> ListRoms(u64 platform_id) {
    std::vector<Rom> out;
    const auto credentials = Common::RommConfig::GetMasterCredentials();
    if (!credentials.IsSet()) {
        return out;
    }

    // CONFIRMED (via RomM's own API client library): /api/roms is offset/limit paginated, not
    // cursor-based — the response is {"items": [...], "total": N, "limit": L, "offset": O}. A
    // single request without paging silently truncates at whatever the server's default limit is
    // (this is what produced "50 titles" regardless of platform_id on a much larger library) — so
    // this loops on offset until it has collected `total` items. Capped at 40 pages as a sanity
    // limit against a server that somehow never converges (e.g. a broken/changing total).
    constexpr u64 PageSize = 200;
    constexpr int MaxPages = 40;

    u64 offset = 0;
    std::optional<u64> total;
    auto client = MakeClient(credentials);

    if (platform_id == 0) {
        // A platform id of 0 almost always means FindSwitchPlatforms()'s underlying /api/platforms
        // response didn't have an "id" field where expected for the matched entry — and on a
        // Python/FastAPI backend, a filter written as `if platform_id:` treats 0 the same as "not
        // provided", which would silently turn this into an unfiltered request. Logged loudly
        // because this is the single most likely explanation if filtering still doesn't work.
        LOG_ERROR(WebService,
                  "RomM ListRoms: resolved platform_id is 0 — the platform_id filter will almost "
                  "certainly be ignored server-side; check the platform list logged above");
    }

    for (int page = 0; page < MaxPages; ++page) {
        // Sending both spellings deliberately: this project's own API client library uses the
        // singular `platform_id`, but at least one point in RomM's own history shows a different
        // endpoint's filter being widened from singular to plural (`platform_ids`) — an unknown
        // extra query param is harmless (FastAPI ignores it), so covering both costs nothing.
        const std::string path = fmt::format("/api/roms?platform_id={}&platform_ids={}&limit={}&offset={}",
                                             platform_id, platform_id, PageSize, offset);
        LOG_INFO(WebService, "RomM ListRoms request: {}", path);
        auto result = client.Get(path, AuthHeaders(credentials));
        if (!result || result->status < 200 || result->status >= 300) {
            LOG_WARNING(WebService, "RomM ListRoms failed (HTTP {}) at offset={}",
                        result ? result->status : -1, offset);
            break;
        }

        try {
            const json body = json::parse(result->body);
            const json& items = body.is_array() ? body : body.value("items", json::array());
            for (const auto& r : items) {
                out.push_back(ParseRom(r));
            }
            if (page == 0 && !body.is_array()) {
                // Logged once, up front: if this "total" is obviously the whole library (not just
                // Switch), the filter is being ignored server-side, full stop — independent of
                // whatever platform_id value was actually sent.
                LOG_INFO(WebService, "RomM ListRoms: server reports total={} for platform_id={}",
                         body.value("total", u64{0}), platform_id);
            }
            if (items.empty()) {
                break; // Nothing left, regardless of what `total` claims.
            }
            if (!body.is_array()) {
                total = body.value("total", out.size());
            }
        } catch (const json::exception& e) {
            LOG_WARNING(WebService, "RomM ListRoms: failed to parse response: {}", e.what());
            break;
        }

        offset += PageSize;
        if (total.has_value() && out.size() >= *total) {
            break;
        }
        if (!total.has_value()) {
            break; // Bare-array response (older/simpler instance) — no pagination info, done.
        }
    }
    return out;
}

std::optional<Rom> GetRom(u64 rom_id) {
    const auto credentials = Common::RommConfig::GetMasterCredentials();
    if (!credentials.IsSet()) {
        return std::nullopt;
    }

    auto client = MakeClient(credentials);
    auto result = client.Get(fmt::format("/api/roms/{}", rom_id), AuthHeaders(credentials));
    if (!result || result->status < 200 || result->status >= 300) {
        LOG_WARNING(WebService, "RomM GetRom({}) failed (HTTP {})", rom_id,
                    result ? result->status : -1);
        return std::nullopt;
    }

    try {
        return ParseRom(json::parse(result->body));
    } catch (const json::exception& e) {
        LOG_WARNING(WebService, "RomM GetRom({}): failed to parse response: {}", rom_id, e.what());
        return std::nullopt;
    }
}

namespace {
std::vector<Platform> g_switch_platforms_cache;
}

std::vector<Platform> FindSwitchPlatforms() {
    if (!g_switch_platforms_cache.empty()) {
        // ListPlatforms() already filters to Switch / Switch 2, so the cached ids are enough; we
        // don't need to re-fetch them on every call.
        return g_switch_platforms_cache;
    }
    g_switch_platforms_cache = ListPlatforms();
    return g_switch_platforms_cache;
}

std::vector<Rom> ListSwitchRoms() {
    std::vector<Rom> all;
    for (const auto& platform : FindSwitchPlatforms()) {
        auto roms = ListRoms(platform.id);
        for (auto& rom : roms) {
            if (rom.platform_slug.empty()) {
                rom.platform_slug = platform.slug; // the list endpoint should send it; be safe
            }
            all.push_back(std::move(rom));
        }
    }
    return all;
}

std::optional<std::string> Sha1HexOfFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),
                                                                EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha1(), nullptr) != 1) {
        return std::nullopt;
    }

    std::vector<char> buffer(1024 * 64);
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read = file.gcount();
        if (read > 0 &&
            EVP_DigestUpdate(ctx.get(), buffer.data(), static_cast<size_t>(read)) != 1) {
            return std::nullopt;
        }
    }
    if (EVP_DigestFinal_ex(ctx.get(), digest, &digest_len) != 1) {
        return std::nullopt;
    }

    std::string hex;
    hex.reserve(digest_len * 2);
    for (unsigned int i = 0; i < digest_len; ++i) {
        hex += fmt::format("{:02x}", digest[i]);
    }
    return hex;
}

bool DownloadRomContent(u64 rom_id, u64 file_id, const std::string& filename,
                        const std::string& dest_path, const std::function<void(u64, u64)>& on_progress,
                        const std::function<bool()>& should_cancel) {
    const auto credentials = Common::RommConfig::GetMasterCredentials();
    if (!credentials.IsSet()) {
        return false;
    }

    std::ofstream out_file(dest_path, std::ios::binary | std::ios::trunc);
    if (!out_file) {
        LOG_ERROR(WebService, "RomM DownloadRomContent: could not open '{}' for writing",
                  dest_path);
        return false;
    }

    auto client = MakeClient(credentials, DownloadTimeoutSeconds);
    u64 total = 0;
    bool write_failed = false;
    bool cancelled = false;
    bool wrong_content_type = false;

    // file_id == 0 means "this rom only ever had one file total" (the fallback path in ParseRom,
    // for the rare rom whose list/detail response never populated files[] with real ids) — RomM
    // only serves a bare single file without ?file_ids= in that exact case anyway, so omitting it
    // there is correct, not a shortcut.
    const std::string path = file_id != 0
                                 ? fmt::format("/api/roms/{}/content/{}?file_ids={}", rom_id,
                                              filename, file_id)
                                 : fmt::format("/api/roms/{}/content/{}", rom_id, filename);
    auto result = client.Get(
        path, AuthHeaders(credentials),
        [&](const httplib::Response& response) {
            if (auto it = response.headers.find("Content-Length"); it != response.headers.end()) {
                total = std::strtoull(it->second.c_str(), nullptr, 10);
            }
            // Safety net: RomM's whole-rom (zip) response is Content-Type: application/zip with
            // no Content-Length. If ?file_ids= is ever ignored (a server-side quirk, or this rom
            // secretly has more files sharing a name), this catches it before writing gigabytes of
            // wrong data to disk instead of a 150 KiB DLC.
            if (auto ct = response.headers.find("Content-Type");
                ct != response.headers.end() && ct->second.find("application/zip") != std::string::npos) {
                wrong_content_type = true;
                return false; // Abort before the body is even read.
            }
            return true;
        },
        [&](const char* data, size_t length) {
            if (should_cancel && should_cancel()) {
                cancelled = true;
                return false; // Abort the transfer — httplib stops reading immediately.
            }
            out_file.write(data, static_cast<std::streamsize>(length));
            if (!out_file) {
                write_failed = true;
                return false; // Abort the transfer.
            }
            if (on_progress) {
                on_progress(static_cast<u64>(out_file.tellp()), total);
            }
            return true;
        });

    out_file.close();

    if (cancelled) {
        LOG_INFO(WebService, "RomM DownloadRomContent({}, '{}') cancelled by the user", rom_id,
                 filename);
        std::error_code ec;
        std::filesystem::remove(dest_path, ec);
        return false;
    }
    if (wrong_content_type) {
        LOG_ERROR(WebService,
                  "RomM DownloadRomContent({}, '{}'): server sent a zip instead of the single "
                  "file — refusing to download it (file_id={})",
                  rom_id, filename, file_id);
        std::error_code ec;
        std::filesystem::remove(dest_path, ec);
        return false;
    }
    if (!result || result->status < 200 || result->status >= 300 || write_failed) {
        LOG_WARNING(WebService, "RomM DownloadRomContent({}, '{}') failed (HTTP {}, write_ok={})",
                    rom_id, filename, result ? result->status : -1, !write_failed);
        std::error_code ec;
        std::filesystem::remove(dest_path, ec); // Best-effort cleanup of the partial file.
        return false;
    }
    return true;
}

// ---- Device Sync Protocol ----------------------------------------------------------------

namespace {

// Keyed by "instance_url|username" rather than by profile UUID: the cache belongs to this
// process/session, not to config, and two different profiles pointed at the same account should
// share one registration rather than creating a duplicate device on the server.
std::map<std::string, std::string> g_device_id_cache;

std::string CredentialsCacheKey(const Common::RommConfig::Credentials& credentials) {
    // One registration per account, whatever the scope of the token that made it.
    return credentials.instance_url + "|" + credentials.username;
}

} // Anonymous namespace

std::optional<std::string> RegisterDeviceIfNeeded(const Common::RommConfig::Credentials& credentials,
                                                  const std::string& device_name,
                                                  const std::string& hostname) {
    if (!credentials.IsSet()) {
        return std::nullopt;
    }

    const std::string cache_key = CredentialsCacheKey(credentials);
    if (auto it = g_device_id_cache.find(cache_key); it != g_device_id_cache.end()) {
        return it->second;
    }

    auto client = MakeClient(credentials);
    const json body{
        {"name", device_name},
        {"platform", "lichii"},
        {"hostname", hostname},
        {"sync_mode", "push_pull"},
    };
    auto result =
        client.Post("/api/devices", AuthHeaders(credentials), body.dump(), "application/json");
    if (!result || result->status < 200 || result->status >= 300) {
        LOG_WARNING(WebService,
                    "RomM RegisterDeviceIfNeeded failed (HTTP {}, token scope requested: '{}'): {}",
                    result ? result->status : -1, credentials.scope,
                    result ? result->body : std::string{});
        return std::nullopt;
    }

    try {
        const json response = json::parse(result->body);
        // CONFIRMED via a real instance's openapi.json (DeviceCreateResponse): the field is
        // "device_id", a string — not "id" as an integer, which an earlier version of this
        // guessed and which silently produced device_id=0 for every call.
        const std::string device_id = response.value("device_id", std::string{});
        if (device_id.empty()) {
            LOG_WARNING(WebService, "RomM RegisterDeviceIfNeeded: response had no device_id: {}",
                        result->body);
            return std::nullopt;
        }
        g_device_id_cache[cache_key] = device_id;
        return device_id;
    } catch (const json::exception& e) {
        LOG_WARNING(WebService, "RomM RegisterDeviceIfNeeded: failed to parse response: {}",
                    e.what());
        return std::nullopt;
    }
}

NegotiateResult NegotiateSync(const Common::RommConfig::Credentials& credentials,
                              const std::string& device_id, u64 rom_id, const std::string& slot,
                              const SaveFileMeta& save) {
    NegotiateResult out;
    if (!credentials.IsSet()) {
        out.error = "No account configured for this profile";
        return out;
    }

    // Matches SyncNegotiatePayload/ClientSaveState exactly (confirmed against a real instance's
    // openapi.json) — a flat "saves" array, each entry carrying its own rom_id, not the
    // {"roms": [{"rom_id":..., "saves": [...]}]} nesting an earlier, docs-based version of this
    // guessed. device_id is a bare string field here, not wrapped or typed as a number.
    const json body{
        {"device_id", device_id},
        {"saves", json::array({{
                      {"rom_id", rom_id},
                      {"file_name", save.filename},
                      {"slot", slot},
                      {"emulator", "lichii"},
                      {"content_hash", save.sha1_hex},
                      {"updated_at", save.mtime_rfc3339},
                      {"file_size_bytes", save.size_bytes},
                  }})},
    };

    auto client = MakeClient(credentials);
    auto result = client.Post("/api/sync/negotiate", AuthHeaders(credentials), body.dump(),
                              "application/json");
    if (!result || result->status < 200 || result->status >= 300) {
        out.error = fmt::format("HTTP {}: {}", result ? result->status : -1,
                                result ? result->body : std::string{});
        LOG_WARNING(WebService, "RomM NegotiateSync failed ({})", out.error);
        return out;
    }

    try {
        const json response = json::parse(result->body);
        // session_id is a JSON integer (SyncNegotiateResponse) — reading it as a string here, as
        // an earlier version of this did, would throw and be silently swallowed by the catch
        // below on every single call.
        out.session_id = response.value("session_id", u64{0});
        for (const auto& op : response.value("operations", json::array())) {
            SyncOperation o;
            // The field is "action", not "type" — another earlier guess this corrects.
            o.type = ParseOpType(op.value("action", std::string{}));
            o.rom_id = op.value("rom_id", u64{0});
            o.file_name = op.value("file_name", std::string{});
            o.slot = NullableString(op, "slot");
            if (!op.contains("save_id") || op.at("save_id").is_null()) {
                o.save_id = std::nullopt;
            } else {
                o.save_id = op.at("save_id").get<u64>();
            }
            o.reason = op.value("reason", std::string{});
            out.operations.push_back(std::move(o));
        }
        out.ok = true;
    } catch (const json::exception& e) {
        out.error = fmt::format("Failed to parse response: {}", e.what());
        LOG_WARNING(WebService, "RomM NegotiateSync: {}", out.error);
    }
    return out;
}

bool UploadSave(const Common::RommConfig::Credentials& credentials, u64 rom_id,
                const std::string& slot, const std::string& device_id, u64 session_id,
                const std::string& filename, const std::vector<u8>& data) {
    if (!credentials.IsSet()) {
        return false;
    }

    // rom_id/slot/device_id/session_id are query parameters here, not JSON body fields (confirmed
    // via openapi.json — POST /api/saves takes them as Query(...), with only the file itself in
    // the multipart body). overwrite=true: a save already exists for this (rom_id, slot) exactly
    // when NegotiateSync said "upload" because the local copy is newer, so this should replace it,
    // not create a second one.
    const std::string path = fmt::format(
        "/api/saves?rom_id={}&slot={}&device_id={}&session_id={}&emulator=eden&overwrite=true",
        rom_id, slot, device_id, session_id);

    // The multipart field name is "saveFile" (confirmed via Body_add_save_api_saves_post) — an
    // earlier version of this used the generic "file", which a strict multipart-field-name schema
    // would reject outright.
    httplib::UploadFormDataItems items{{
        "saveFile",
        std::string(reinterpret_cast<const char*>(data.data()), data.size()),
        filename,
        "application/octet-stream",
    }};

    auto client = MakeClient(credentials, DownloadTimeoutSeconds);
    auto result = client.Post(path, AuthHeaders(credentials), items);
    if (!result || result->status < 200 || result->status >= 300) {
        LOG_WARNING(WebService, "RomM UploadSave('{}') failed (HTTP {}): {}", filename,
                    result ? result->status : -1, result ? result->body : std::string{});
        return false;
    }
    return true;
}

std::optional<std::vector<RemoteSave>> ListSaves(const Common::RommConfig::Credentials& credentials,
                                                 u64 rom_id) {
    if (!credentials.IsSet()) {
        return std::nullopt;
    }

    auto client = MakeClient(credentials);
    auto result = client.Get(fmt::format("/api/saves?rom_id={}", rom_id), AuthHeaders(credentials));
    if (!result || result->status < 200 || result->status >= 300) {
        LOG_WARNING(WebService, "RomM ListSaves(rom_id={}) failed (HTTP {})", rom_id,
                    result ? result->status : -1);
        return std::nullopt;
    }

    try {
        const json body = json::parse(result->body);
        // A plain array on current RomM versions; tolerate a paginated {"items": [...]} wrapper.
        const json& items = body.is_array() ? body : body.value("items", json::array());
        std::vector<RemoteSave> saves;
        for (const auto& s : items) {
            RemoteSave save;
            save.save_id = s.value("id", u64{0});
            if (save.save_id == 0) {
                continue;
            }
            // Don't rely on the server honoring the rom_id filter: never show another title's save.
            if (s.contains("rom_id") && s.at("rom_id").is_number_unsigned() &&
                s.at("rom_id").get<u64>() != rom_id) {
                continue;
            }
            save.file_name = s.value("file_name", std::string{});
            save.slot = NullableString(s, "slot");
            save.emulator = NullableString(s, "emulator");
            save.updated_at = NullableString(s, "updated_at");
            save.size_bytes = s.value("file_size_bytes", u64{0});
            saves.push_back(std::move(save));
        }
        // ISO 8601 strings sort chronologically; newest first.
        std::ranges::sort(saves, [](const RemoteSave& a, const RemoteSave& b) {
            return a.updated_at > b.updated_at;
        });
        return saves;
    } catch (const json::exception& e) {
        LOG_WARNING(WebService, "RomM ListSaves: failed to parse response: {}", e.what());
        return std::nullopt;
    }
}

std::optional<std::vector<u8>> DownloadSave(const Common::RommConfig::Credentials& credentials,
                                            u64 save_id) {
    if (!credentials.IsSet()) {
        return std::nullopt;
    }

    auto client = MakeClient(credentials, DownloadTimeoutSeconds);
    auto result =
        client.Get(fmt::format("/api/saves/{}/content", save_id), AuthHeaders(credentials));
    if (!result || result->status < 200 || result->status >= 300) {
        LOG_WARNING(WebService, "RomM DownloadSave({}) failed (HTTP {})", save_id,
                    result ? result->status : -1);
        return std::nullopt;
    }
    return std::vector<u8>(result->body.begin(), result->body.end());
}

bool CompleteSession(const Common::RommConfig::Credentials& credentials, u64 session_id,
                     u32 operations_completed, u32 operations_failed,
                     const std::vector<PlaySession>& play_sessions) {
    if (!credentials.IsSet()) {
        return false;
    }

    json sessions_json = json::array();
    for (const auto& p : play_sessions) {
        sessions_json.push_back({
            {"rom_id", p.rom_id},
            {"start_time", p.start_time_rfc3339},
            {"end_time", p.end_time_rfc3339},
            {"duration_ms", p.duration_ms},
        });
    }
    const json body{
        {"operations_completed", operations_completed},
        {"operations_failed", operations_failed},
        {"play_sessions", sessions_json},
    };

    auto client = MakeClient(credentials);
    // session_id is a bare integer in the path (SyncNegotiateResponse.session_id's real type) —
    // not the string an earlier version of this assumed.
    auto result = client.Post(fmt::format("/api/sync/sessions/{}/complete", session_id),
                              AuthHeaders(credentials), body.dump(), "application/json");
    if (!result || result->status < 200 || result->status >= 300) {
        LOG_WARNING(WebService, "RomM CompleteSession({}) failed (HTTP {})", session_id,
                    result ? result->status : -1);
        return false;
    }
    return true;
}

} // namespace Common::RommApi
