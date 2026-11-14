// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Who this emulator is on each private online server: a friend code, the secret the server ties
// it to, and the friend codes added there. One profile per server, so no server learns the secret
// another server knows. Profiles live in the OS user's data folder, not the emulator's, so a new
// emulator version or folder keeps them: %APPDATA%\Droplet-online\profiles on Windows, else
// $XDG_DATA_HOME (or ~/.local/share)/droplet-online/profiles (the Ounce2dn-online folders of
// builds before the rename move there). EDEN_NX2_ONLINE_PROFILES moves them (test instances each
// keep their own).
namespace Service::Sockets::OnlineProfile {

struct Profile {
    std::string server;               // as entered, e.g. "raiders.example.org:8443"
    std::string code;                 // 12 digits, never all zeros
    std::string secret;               // 32 hex digits
    std::vector<std::string> friends; // codes added on this server
    std::vector<std::string> removed; // codes removed since, so the server forgets them too
};

std::filesystem::path ProfilesDir();

// This server's profile. When it has none: nullopt, or with `create` a new one. The first profile
// takes over the friend code that emulator folders had before profiles (<data folder>/online/
// identity.txt) and the friends' codes from the old setting; until then, that is what a load
// without `create` returns.
std::optional<Profile> LoadProfile(const std::string& server, bool create);

// A profile as text (its file, and the backup players keep) and back.
std::string ProfileText(const Profile& profile);
std::optional<Profile> ParseProfile(std::string_view text);

std::string NewSecret();
// 12-digit codes in text ("SW-1234-5678-9012, 2345 6789 0123"), in order, without duplicates.
std::vector<std::string> ParseCodes(std::string_view text);

// The ID token a game forwards to its online server at login while the host redirect sends it
// to a server (Network::LocalRedirect::Server): this server's profile, made on first use, and the
// game's build (its main module's build ID in hex). The server reads the friend code and secret
// as the player's account and the friend codes as their friend list. Empty without a server.
std::string IdentityToken(std::string_view build);

void AddFriend(Profile& profile, const std::string& code);

} // namespace Service::Sockets::OnlineProfile
