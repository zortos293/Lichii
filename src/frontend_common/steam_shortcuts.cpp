// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend_common/steam_shortcuts.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <system_error>

#include <boost/crc.hpp>

#include "common/fs/fs_util.h"
#include "common/logging.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Steam {
namespace {

// Binary VDF: each value is a type byte, a NUL-terminated key and its data; a map's values end
// with an End byte, and so does the file's top level.
enum Type : u8 {
    Map = 0x00,
    String = 0x01,
    Int32 = 0x02,
    Float = 0x03,
    Pointer = 0x04,
    WideString = 0x05,
    Color = 0x06,
    UInt64 = 0x07,
    End = 0x08,
    Int64 = 0x0A,
};

struct Node {
    u8 type{};
    std::string key;
    std::string text;     // String
    std::vector<u8> raw;  // Every other scalar, kept as stored
    std::vector<Node> children;

    Node* Find(std::string_view name) {
        const auto it = std::find_if(children.begin(), children.end(),
                                     [name](const Node& child) { return child.key == name; });
        return it == children.end() ? nullptr : &*it;
    }
};

std::size_t FixedSize(u8 type) {
    switch (type) {
    case Int32:
    case Float:
    case Pointer:
    case Color:
        return 4;
    case UInt64:
    case Int64:
        return 8;
    default:
        return 0;
    }
}

class Reader {
public:
    explicit Reader(std::span<const u8> data_) : data{data_} {}

    /// Reads values up to an End byte (or the end of the data at the top level).
    bool ReadChildren(std::vector<Node>& out, bool top_level) {
        while (pos < data.size()) {
            const u8 type = data[pos++];
            if (type == End) {
                return true;
            }
            Node node{.type = type};
            if (!ReadCString(node.key)) {
                return false;
            }
            if (type == Map) {
                if (!ReadChildren(node.children, false)) {
                    return false;
                }
            } else if (type == String) {
                if (!ReadCString(node.text)) {
                    return false;
                }
            } else if (const std::size_t size = FixedSize(type); size != 0) {
                if (data.size() - pos < size) {
                    return false;
                }
                const auto begin = data.begin() + static_cast<std::ptrdiff_t>(pos);
                node.raw.assign(begin, begin + static_cast<std::ptrdiff_t>(size));
                pos += size;
            } else {
                // Wide strings and unknown types have no size we could skip safely.
                return false;
            }
            out.push_back(std::move(node));
        }
        return top_level;
    }

    bool AtEnd() const {
        return pos == data.size();
    }

private:
    bool ReadCString(std::string& out) {
        const auto begin = data.begin() + static_cast<std::ptrdiff_t>(pos);
        const auto nul = std::find(begin, data.end(), u8{0});
        if (nul == data.end()) {
            return false;
        }
        out.assign(begin, nul);
        pos += static_cast<std::size_t>(nul - begin) + 1;
        return true;
    }

    std::span<const u8> data;
    std::size_t pos{};
};

void Write(std::vector<u8>& out, const std::vector<Node>& nodes) {
    for (const Node& node : nodes) {
        out.push_back(node.type);
        out.insert(out.end(), node.key.begin(), node.key.end());
        out.push_back(0);
        if (node.type == Map) {
            Write(out, node.children);
            out.push_back(End);
        } else if (node.type == String) {
            out.insert(out.end(), node.text.begin(), node.text.end());
            out.push_back(0);
        } else {
            out.insert(out.end(), node.raw.begin(), node.raw.end());
        }
    }
}

Node MakeString(std::string key, std::string value) {
    return Node{.type = String, .key = std::move(key), .text = std::move(value)};
}

Node MakeInt(std::string key, u32 value) {
    Node node{.type = Int32, .key = std::move(key)};
    for (int shift = 0; shift < 32; shift += 8) {
        node.raw.push_back(static_cast<u8>(value >> shift));
    }
    return node;
}

std::string Quote(std::string_view value) {
    return "\"" + std::string{value} + "\"";
}

/// Points the fields Lichii owns at the shortcut, leaving the rest (play time, tags, Steam's own
/// settings) as they are.
void Fill(Node& entry, const Shortcut& shortcut) {
    const std::string exe = Quote(shortcut.exe);
    const std::pair<std::string_view, Node> owned[] = {
        {"appid", MakeInt("appid", ShortcutAppId(exe, shortcut.app_name))},
        {"AppName", MakeString("AppName", shortcut.app_name)},
        {"Exe", MakeString("Exe", exe)},
        {"StartDir", MakeString("StartDir", Quote(shortcut.start_dir))},
        {"icon", MakeString("icon", shortcut.icon)},
        {"LaunchOptions", MakeString("LaunchOptions", shortcut.launch_options)},
    };
    for (const auto& [key, value] : owned) {
        if (Node* existing = entry.Find(key)) {
            *existing = value;
        } else {
            entry.children.push_back(value);
        }
    }
}

Node NewEntry(std::string index, const Shortcut& shortcut) {
    // The fields and order Steam writes for a shortcut it adds itself.
    Node entry{.type = Map, .key = std::move(index)};
    entry.children = {
        MakeInt("appid", 0),
        MakeString("AppName", {}),
        MakeString("Exe", {}),
        MakeString("StartDir", {}),
        MakeString("icon", {}),
        MakeString("ShortcutPath", {}),
        MakeString("LaunchOptions", {}),
        MakeInt("IsHidden", 0),
        MakeInt("AllowDesktopConfig", 1),
        MakeInt("AllowOverlay", 1),
        MakeInt("OpenVR", 0),
        MakeInt("Devkit", 0),
        MakeString("DevkitGameID", {}),
        MakeInt("DevkitOverrideAppID", 0),
        MakeInt("LastPlayTime", 0),
        MakeString("FlatpakAppID", {}),
        MakeString("sortas", {}),
        Node{.type = Map, .key = "tags"},
    };
    Fill(entry, shortcut);
    return entry;
}

std::string TextOf(Node& entry, std::string_view key) {
    const Node* node = entry.Find(key);
    return node != nullptr && node->type == String ? node->text : std::string{};
}

#ifdef _WIN32
std::optional<std::wstring> RegistryString(const wchar_t* key, const wchar_t* name) {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) !=
            ERROR_SUCCESS ||
        size == 0) {
        return std::nullopt;
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, nullptr, value.data(),
                     &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    value.resize(wcsnlen(value.c_str(), value.size()));
    return value;
}
#endif

std::optional<std::string> ReadText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    // libstdc++ throws when a read fails after the file opened, e.g. /proc/<pid>/comm of a
    // process that exited in between.
    try {
        return std::string{std::istreambuf_iterator<char>(file), {}};
    } catch (const std::ios_base::failure&) {
        return std::nullopt;
    }
}

/// The Steam account id (SteamID64 minus its base) that loginusers.vdf marks MostRecent.
std::optional<u64> MostRecentAccount(const std::filesystem::path& steam_root) {
    const auto text = ReadText(steam_root / "config" / "loginusers.vdf");
    if (!text) {
        return std::nullopt;
    }
    // Text VDF: "users" { "<SteamID64>" { "MostRecent" "1" ... } ... }
    int depth = 0;
    std::string current_user;
    std::string pending_key;
    for (std::size_t i = 0; i < text->size(); ++i) {
        const char c = (*text)[i];
        if (c == '{') {
            if (depth == 1) {
                current_user = pending_key;
            }
            ++depth;
            pending_key.clear();
        } else if (c == '}') {
            --depth;
            pending_key.clear();
        } else if (c == '"') {
            // Values escape quotes and backslashes (a display name can hold either).
            std::string token;
            std::size_t end = i + 1;
            for (; end < text->size() && (*text)[end] != '"'; ++end) {
                if ((*text)[end] == '\\' && end + 1 < text->size()) {
                    ++end;
                }
                token.push_back((*text)[end]);
            }
            if (end >= text->size()) {
                break;
            }
            i = end;
            if (pending_key.empty()) {
                pending_key = std::move(token);
                continue;
            }
            if (depth == 2 && pending_key.size() == 10 &&
                std::equal(pending_key.begin(), pending_key.end(), "MostRecent",
                           [](char a, char b) {
                               return std::tolower(static_cast<unsigned char>(a)) ==
                                      std::tolower(static_cast<unsigned char>(b));
                           }) &&
                token == "1") {
                u64 steam_id = 0;
                const auto [ptr, error] = std::from_chars(
                    current_user.data(), current_user.data() + current_user.size(), steam_id);
                constexpr u64 IndividualBase = 76561197960265728ULL;
                if (error == std::errc{} && steam_id > IndividualBase) {
                    return steam_id - IndividualBase;
                }
            }
            pending_key.clear();
        }
    }
    return std::nullopt;
}

} // Anonymous namespace

u32 ShortcutAppId(std::string_view quoted_exe, std::string_view app_name) {
    boost::crc_32_type crc;
    crc.process_bytes(quoted_exe.data(), quoted_exe.size());
    crc.process_bytes(app_name.data(), app_name.size());
    return crc.checksum() | 0x80000000U;
}

std::optional<std::vector<u8>> AddToShortcutsFile(std::span<const u8> file,
                                                  const Shortcut& shortcut, bool* replaced) {
    std::vector<Node> top;
    if (!file.empty()) {
        Reader reader{file};
        if (!reader.ReadChildren(top, true) || !reader.AtEnd()) {
            return std::nullopt;
        }
    }
    auto shortcuts = std::find_if(top.begin(), top.end(), [](const Node& node) {
        return node.type == Map && node.key == "shortcuts";
    });
    if (shortcuts == top.end()) {
        if (!top.empty()) {
            return std::nullopt;
        }
        top.push_back(Node{.type = Map, .key = "shortcuts"});
        shortcuts = top.begin();
    }

    const std::string exe = Quote(shortcut.exe);
    Node* match = nullptr;
    u32 next_index = 0;
    for (Node& entry : shortcuts->children) {
        if (entry.type != Map) {
            continue;
        }
        u32 index = 0;
        const auto [ptr, error] =
            std::from_chars(entry.key.data(), entry.key.data() + entry.key.size(), index);
        if (error == std::errc{}) {
            next_index = (std::max)(next_index, index + 1);
        }
        if (match == nullptr && TextOf(entry, "Exe") == exe &&
            TextOf(entry, "LaunchOptions") == shortcut.launch_options) {
            match = &entry;
        }
    }
    if (replaced != nullptr) {
        *replaced = match != nullptr;
    }
    if (match != nullptr) {
        Fill(*match, shortcut);
    } else {
        shortcuts->children.push_back(NewEntry(std::to_string(next_index), shortcut));
    }

    std::vector<u8> out;
    Write(out, top);
    out.push_back(End);
    return out;
}

std::optional<std::filesystem::path> FindSteamRoot() {
    std::vector<std::filesystem::path> candidates;
#ifdef _WIN32
    if (const auto path = RegistryString(L"Software\\Valve\\Steam", L"SteamPath")) {
        candidates.emplace_back(*path);
    }
#else
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        const std::filesystem::path base{home};
        candidates = {
            base / ".steam" / "steam",
            base / ".local" / "share" / "Steam",
            base / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam",
        };
    }
#endif
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_directory(candidate / "userdata", error)) {
            return std::filesystem::weakly_canonical(candidate, error);
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> FindAccountConfigDir(const std::filesystem::path& steam_root) {
    std::vector<std::filesystem::path> accounts;
    std::error_code error;
    for (const auto& entry :
         std::filesystem::directory_iterator(steam_root / "userdata", error)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_directory() && !name.empty() && name != "0" &&
            std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            accounts.push_back(entry.path());
        }
    }
    if (accounts.empty()) {
        return std::nullopt;
    }
    if (accounts.size() > 1) {
        if (const auto recent = MostRecentAccount(steam_root)) {
            const auto it = std::find_if(accounts.begin(), accounts.end(), [&](const auto& path) {
                return path.filename().string() == std::to_string(*recent);
            });
            if (it != accounts.end()) {
                return *it / "config";
            }
        }
        // Without a marked account, the one Steam saved settings for last.
        const auto last_used = [](const std::filesystem::path& account) {
            std::error_code time_error;
            return std::filesystem::last_write_time(account / "config" / "localconfig.vdf",
                                                    time_error);
        };
        std::sort(accounts.begin(), accounts.end(),
                  [&](const auto& a, const auto& b) { return last_used(a) > last_used(b); });
    }
    return accounts.front() / "config";
}

bool IsSteamRunning() {
#ifdef _WIN32
    DWORD pid = 0;
    DWORD size = sizeof(pid);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"pid",
                     RRF_RT_REG_DWORD, nullptr, &pid, &size) != ERROR_SUCCESS ||
        pid == 0) {
        return false;
    }
    // Steam leaves its last pid behind when it exits: check that the process is still Steam.
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return false;
    }
    wchar_t image[MAX_PATH];
    DWORD image_size = MAX_PATH;
    DWORD exit_code = 0;
    const bool alive = GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
    const bool named = QueryFullProcessImageNameW(process, 0, image, &image_size);
    CloseHandle(process);
    if (!alive || !named) {
        return false;
    }
    std::wstring file = std::filesystem::path{std::wstring{image, image_size}}.filename().wstring();
    std::transform(file.begin(), file.end(), file.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return file == L"steam.exe";
#elif defined(__linux__)
    // Look for the client itself rather than ~/.steam/steam.pid: Flatpak Steam keeps its pid file
    // in its own home, and a sandboxed pid isn't the host's. Every install runs as "steam".
    // Only this user's processes count: another user's Steam doesn't hold this user's files.
    const uid_t user = getuid();
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
        const std::string name = entry.path().filename().string();
        if (name.empty() ||
            !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        struct stat info {};
        if (stat(entry.path().c_str(), &info) != 0 || info.st_uid != user) {
            continue;
        }
        const auto comm = ReadText(entry.path() / "comm");
        if (comm && (*comm == "steam\n" || *comm == "steam")) {
            return true;
        }
    }
    return false;
#else
    return false;
#endif
}

AddResult AddShortcut(const Shortcut& shortcut) {
    const auto root = FindSteamRoot();
    if (!root) {
        return AddResult::SteamNotFound;
    }
    if (IsSteamRunning()) {
        return AddResult::SteamRunning;
    }
    const auto config = FindAccountConfigDir(*root);
    if (!config) {
        return AddResult::AccountNotFound;
    }

    const auto path = *config / "shortcuts.vdf";
    std::error_code error;
    const bool existed = std::filesystem::exists(path, error);
    std::vector<u8> current;
    if (existed) {
        const auto text = ReadText(path);
        if (!text) {
            return AddResult::UnreadableFile;
        }
        current.assign(text->begin(), text->end());
    }

    bool replaced = false;
    const auto updated = AddToShortcutsFile(current, shortcut, &replaced);
    if (!updated) {
        LOG_ERROR(Frontend, "Steam shortcuts file is not in the expected format: {}",
                  Common::FS::PathToUTF8String(path));
        return AddResult::UnreadableFile;
    }

    std::filesystem::create_directories(*config, error);
    if (existed) {
        auto backup = path;
        backup += ".bak";
        std::filesystem::copy_file(path, backup, std::filesystem::copy_options::overwrite_existing,
                                   error);
        if (error) {
            LOG_ERROR(Frontend, "Could not back up {}: {}", Common::FS::PathToUTF8String(path),
                      error.message());
            return AddResult::WriteFailed;
        }
    }
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(updated->data()),
                  static_cast<std::streamsize>(updated->size()));
        // Closing writes what is still buffered, and can fail too.
        out.close();
        if (!out) {
            LOG_ERROR(Frontend, "Could not write {}", Common::FS::PathToUTF8String(temporary));
            std::filesystem::remove(temporary, error);
            return AddResult::WriteFailed;
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        LOG_ERROR(Frontend, "Could not replace {}: {}", Common::FS::PathToUTF8String(path),
                  error.message());
        std::filesystem::remove(temporary, error);
        return AddResult::WriteFailed;
    }
    LOG_INFO(Frontend, "{} Steam shortcut \"{}\"", replaced ? "Updated" : "Added",
             shortcut.app_name);
    return replaced ? AddResult::Updated : AddResult::Added;
}

} // namespace Steam
