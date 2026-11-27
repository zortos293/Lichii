// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/romm_config.h"
#include "common/string_util.h"

namespace Common::RommConfig {

namespace {

std::mutex g_mutex;

// Strips a single trailing '/', if present, so callers can always do instance_url + "/api/...".
std::string StripTrailingSlash(std::string url) {
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

// Generic "key=value" reader/writer, shared by the master account and every per-profile account:
// they're the same fields, just stored under different paths. Nothing secret is in these files.
// Older files that still carry token / refresh_token lines are read fine; those lines are ignored
// and the account has to be logged in again to get its password into the credential store.
Credentials ReadCredentialsFile(const std::filesystem::path& path) {
    Credentials out;
    if (!FS::Exists(path)) {
        return out;
    }

    const std::string contents = FS::ReadStringFromFile(path, FS::FileType::TextFile);
    std::vector<std::string> lines;
    Common::SplitString(contents, '\n', lines);
    for (const std::string& line : lines) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);

        if (key == "instance_url") {
            out.instance_url = StripTrailingSlash(value);
        } else if (key == "username") {
            out.username = value;
        } else if (key == "scope") {
            out.scope = value;
        }
    }
    return out;
}

void WriteCredentialsFile(const std::filesystem::path& path, const Credentials& credentials) {
    void(FS::CreateParentDirs(path));
    const std::string contents =
        fmt::format("instance_url={}\nusername={}\nscope={}\n",
                    StripTrailingSlash(credentials.instance_url), credentials.username,
                    credentials.scope);
    void(FS::WriteStringToFile(path, FS::FileType::TextFile, contents));
}

std::filesystem::path MasterFilePath() {
    return FS::GetEdenPath(FS::EdenPath::ConfigDir) / "romm_master.txt";
}

// One file per profile, named by its 32-hex-char UUID (RawString() — filesystem-safe, no dashes),
// under its own subdirectory rather than dumped alongside romm_master.txt.
std::filesystem::path ProfileFilePath(const Common::UUID& profile_id) {
    return FS::GetEdenPath(FS::EdenPath::ConfigDir) / "romm_profiles" /
           (profile_id.RawString() + ".txt");
}

// In-memory cache so repeated calls (e.g. from a per-boot sync check) don't hit disk every time;
// invalidated on every Save()/Clear() for that path. Keyed by path rather than by UUID directly
// since the master account shares the exact same read/write logic.
std::map<std::filesystem::path, Credentials> g_cache;

Credentials LoadCached(const std::filesystem::path& path, const std::string& secret_key) {
    if (auto it = g_cache.find(path); it != g_cache.end()) {
        return it->second;
    }
    Credentials creds = ReadCredentialsFile(path);
    creds.secret_key = secret_key;
    g_cache[path] = creds;
    return creds;
}

constexpr const char* MasterSecretKey = "master";

std::string ProfileSecretKey(const Common::UUID& profile_id) {
    return "profile-" + profile_id.RawString();
}

// Secrets backend + in-memory password cache. Own mutex, separate from g_mutex: a credential
// store can block (an unlock prompt), and that must never stall plain config reads.
std::mutex g_secret_mutex;
Secrets::Backend g_backend;
std::map<std::string, std::string> g_password_cache;

// Writes the account file and puts the password in the credential store, in that order of
// importance: if the store refuses, nothing is written.
bool SaveAccount(const std::filesystem::path& path, const std::string& secret_key,
                 Credentials credentials, const std::string& password, std::string& error) {
    {
        std::lock_guard lock{g_secret_mutex};
        if (!g_backend.write) {
            error = "No system credential store is available";
            return false;
        }
        if (!g_backend.write(secret_key, password, error)) {
            if (error.empty()) {
                error = "The system credential store refused the password";
            }
            return false;
        }
        g_password_cache[secret_key] = password;
    }
    credentials.secret_key = secret_key;
    std::lock_guard lock{g_mutex};
    WriteCredentialsFile(path, credentials);
    g_cache[path] = credentials;
    return true;
}

void ClearAccount(const std::filesystem::path& path, const std::string& secret_key) {
    {
        std::lock_guard lock{g_secret_mutex};
        g_password_cache.erase(secret_key);
        std::string ignored;
        if (g_backend.remove) {
            g_backend.remove(secret_key, ignored);
        }
    }
    std::lock_guard lock{g_mutex};
    void(FS::RemoveFile(path));
    g_cache[path] = Credentials{};
}

} // Anonymous namespace

namespace Secrets {

void SetBackend(Backend backend) {
    std::lock_guard lock{g_secret_mutex};
    g_backend = std::move(backend);
    g_password_cache.clear();
}

std::optional<std::string> GetPassword(const std::string& secret_key) {
    if (secret_key.empty()) {
        return std::nullopt;
    }
    std::lock_guard lock{g_secret_mutex};
    if (const auto it = g_password_cache.find(secret_key); it != g_password_cache.end()) {
        return it->second;
    }
    if (!g_backend.read) {
        return std::nullopt;
    }
    std::string value;
    std::string error;
    if (!g_backend.read(secret_key, value, error)) {
        if (!error.empty()) {
            LOG_WARNING(WebService, "RomM: couldn't read '{}' from the credential store: {}",
                        secret_key, error);
        }
        return std::nullopt;
    }
    g_password_cache[secret_key] = value;
    return value;
}

} // namespace Secrets

bool IsConfigured() {
    std::lock_guard lock{g_mutex};
    return LoadCached(MasterFilePath(), MasterSecretKey).IsSet();
}

Credentials GetMasterCredentials() {
    std::lock_guard lock{g_mutex};
    return LoadCached(MasterFilePath(), MasterSecretKey);
}

bool Save(const Credentials& credentials, const std::string& password, std::string& error) {
    return SaveAccount(MasterFilePath(), MasterSecretKey, credentials, password, error);
}

void Clear() {
    ClearAccount(MasterFilePath(), MasterSecretKey);
}

namespace Profile {

bool HasAccount(const Common::UUID& profile_id) {
    std::lock_guard lock{g_mutex};
    return LoadCached(ProfileFilePath(profile_id), ProfileSecretKey(profile_id)).IsSet();
}

Credentials GetCredentials(const Common::UUID& profile_id) {
    std::lock_guard lock{g_mutex};
    return LoadCached(ProfileFilePath(profile_id), ProfileSecretKey(profile_id));
}

bool Save(const Common::UUID& profile_id, const Credentials& credentials,
          const std::string& password, std::string& error) {
    return SaveAccount(ProfileFilePath(profile_id), ProfileSecretKey(profile_id), credentials,
                       password, error);
}

void Clear(const Common::UUID& profile_id) {
    ClearAccount(ProfileFilePath(profile_id), ProfileSecretKey(profile_id));
}

} // namespace Profile

namespace TitleOverride {

namespace {

std::filesystem::path OverrideFilePath() {
    return FS::GetEdenPath(FS::EdenPath::ConfigDir) / "romm_title_overrides.txt";
}

// One line per title, "program_id_hex=rom_id" — small enough (at most a handful of manually
// resolved titles per person) that reading/rewriting the whole file each time is simpler than
// anything more clever, same reasoning as the master/profile credential files.
std::map<u64, u64> LoadAll() {
    std::map<u64, u64> out;
    const auto path = OverrideFilePath();
    if (!FS::Exists(path)) {
        return out;
    }
    std::vector<std::string> lines;
    Common::SplitString(FS::ReadStringFromFile(path, FS::FileType::TextFile), '\n', lines);
    for (const auto& line : lines) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        try {
            const u64 program_id = std::stoull(line.substr(0, eq), nullptr, 16);
            const u64 rom_id = std::stoull(line.substr(eq + 1), nullptr, 10);
            out[program_id] = rom_id;
        } catch (const std::exception&) {
            continue; // Malformed line — skip rather than fail the whole load.
        }
    }
    return out;
}

void SaveAll(const std::map<u64, u64>& entries) {
    std::string contents;
    for (const auto& [program_id, rom_id] : entries) {
        contents += fmt::format("{:016X}={}\n", program_id, rom_id);
    }
    const auto path = OverrideFilePath();
    void(FS::CreateParentDirs(path));
    void(FS::WriteStringToFile(path, FS::FileType::TextFile, contents));
}

} // Anonymous namespace

std::optional<u64> GetRomId(u64 program_id) {
    std::lock_guard lock{g_mutex};
    const auto entries = LoadAll();
    if (const auto it = entries.find(program_id); it != entries.end()) {
        return it->second;
    }
    return std::nullopt;
}

void SetRomId(u64 program_id, u64 rom_id) {
    std::lock_guard lock{g_mutex};
    auto entries = LoadAll();
    entries[program_id] = rom_id;
    SaveAll(entries);
}

void Clear(u64 program_id) {
    std::lock_guard lock{g_mutex};
    auto entries = LoadAll();
    entries.erase(program_id);
    SaveAll(entries);
}

} // namespace TitleOverride

} // namespace Common::RommConfig
