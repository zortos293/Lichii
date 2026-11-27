// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <string>

#include "common/common_types.h"
#include "common/uuid.h"

// Persisted configuration for a self-hosted RomM instance (https://romm.app), used both as a
// content source (downloading owned base games/updates/DLC) and as an optional cloud-save
// backend:
//
//   - One "master" account, instance-wide, used for browsing/downloading the catalogue. There's
//     only ever one of these, the same way there's only one physical RomM login a household
//     shares — it doesn't make sense to ask "which profile" before listing games.
//   - Zero or more per-profile accounts (keyed by the emulated Switch profile's UUID, the same
//     Common::UUID GetProfileID() already deals in), used only for that profile's own cloud save
//     sync. A profile with no account configured simply never gets a sync attempt: RegisterDevice
//     is never called, no dialog, no error — see the note in romm_api.h's sync functions.
//
// There is no "correct" host to lock to: the instance URL is whatever the user's own self-hosted
// (or LAN) RomM deployment happens to be, so the security model here is redirect safety — see
// MakeClient() in romm_api.cpp.
namespace Common::RommConfig {

// OAuth scopes requested at login (RomM's /api/token, password grant). The catalogue account only
// needs to read; a profile's cloud-save account additionally reads/writes saves and devices.
// Keeping them separate lets a read-only ("viewer") RomM user still be the master account.
inline constexpr const char* CatalogScope = "platforms.read roms.read";
inline constexpr const char* SaveSyncScope =
    "platforms.read roms.read assets.read assets.write devices.read devices.write";

// What is persisted for an account, on disk: where it lives and who logs in. The password is NOT
// in this file: it goes to the system credential store (see Secrets below) and is used to obtain
// short-lived OAuth access tokens on demand, which only ever live in memory (romm_api.cpp).
struct Credentials {
    std::string instance_url; // No trailing slash. May be plain http:// (self-hosted/LAN).
    std::string username;
    std::string scope;        // Space-separated OAuth scopes to request at login.

    // Not persisted: which credential-store entry holds this account's password. Filled in by the
    // loaders below ("master" / "profile-<uuid>"); empty on credentials that haven't been saved.
    std::string secret_key;

    // True once an account has been configured (that doesn't guarantee its password is still in
    // the credential store; see Secrets::GetPassword).
    bool IsSet() const {
        return !instance_url.empty() && !username.empty();
    }
};

// ---- System credential store ------------------------------------------------------------------
// Common has no Qt dependency, so the frontend installs the real backend (QtKeychain: libsecret or
// KWallet on Linux, Keychain on macOS, Credential Manager on Windows) at startup. Without one
// installed, saving an account fails rather than falling back to plain text.
namespace Secrets {

struct Backend {
    // true + `value` set if the entry exists. false with an empty `error` if it simply doesn't
    // exist; false with a message if the store itself failed (locked, unavailable, denied...).
    std::function<bool(const std::string& key, std::string& value, std::string& error)> read;
    std::function<bool(const std::string& key, const std::string& value, std::string& error)>
        write;
    std::function<bool(const std::string& key, std::string& error)> remove;
};

void SetBackend(Backend backend);

// The stored password for an account, or nullopt if there is none / the store can't be reached.
// Cached in memory after the first successful read, so the store (and any unlock prompt it
// shows) is only hit once per run. Thread-safe; may block while the store answers.
std::optional<std::string> GetPassword(const std::string& secret_key);

} // namespace Secrets

// ---- Master account (content browsing/downloads) ----------------------------------------------

bool IsConfigured();
Credentials GetMasterCredentials();

// Writes the account file and puts `password` in the credential store. Fails (writing nothing,
// `error` says why) if the store can't take it: the password is never kept anywhere else.
bool Save(const Credentials& credentials, const std::string& password, std::string& error);

// Removes the account file and its stored password.
void Clear();

// ---- Per-profile accounts (cloud save sync) ----------------------------------------------------

namespace Profile {

bool HasAccount(const Common::UUID& profile_id);
Credentials GetCredentials(const Common::UUID& profile_id);

// Independent of the master account and of any other profile's account: a fully separate
// login, even though in practice most people will point every profile at the same instance URL as
// the master and just use a second RomM user there.
bool Save(const Common::UUID& profile_id, const Credentials& credentials,
          const std::string& password, std::string& error);
void Clear(const Common::UUID& profile_id);

} // namespace Profile

// ---- Manual title <-> rom_id overrides ---------------------------------------------------------
// RomM has no title_id field to match an installed title against, and hashing multi-GB game files
// to look one up froze the emulator, so matching is manual-only, via RommTitleMatchDialog. This
// is where the person's pick gets remembered per title (program_id), so it only has to happen
// once. rom_id 0 means "never sync this title".
namespace TitleOverride {

std::optional<u64> GetRomId(u64 program_id);
void SetRomId(u64 program_id, u64 rom_id);
void Clear(u64 program_id);

} // namespace TitleOverride

} // namespace Common::RommConfig
