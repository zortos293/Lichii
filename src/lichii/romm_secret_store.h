// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Connects Common::RommConfig::Secrets to the operating system's credential store through
// QtKeychain: libsecret or KWallet on Linux, the Keychain on macOS, the Credential Manager on
// Windows. Passwords for RomM accounts are kept there instead of in Lichii's config files.
namespace RommSecretStore {

// Installs the backend. Call once at startup, before any RomM account is read or saved.
void Install();

} // namespace RommSecretStore
