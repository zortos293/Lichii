// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

#include "common/common_types.h"

namespace httplib {
class Client;
}

// Google sign-in for Drive sync with the OAuth device flow for TVs and limited-input devices
// (developers.google.com/identity/protocols/oauth2/limited-input-device).
namespace FrontendCommon::CloudSync {

constexpr std::string_view OAuthBase = "https://oauth2.googleapis.com";
constexpr std::string_view DeviceCodePath = "/device/code";
constexpr std::string_view TokenPath = "/token";
constexpr std::string_view RevokePath = "/revoke";
// Only the files and folders Lichii made.
constexpr std::string_view DriveScope = "https://www.googleapis.com/auth/drive.file";
// The access token (an hour) is refreshed this long before it expires.
constexpr s64 RefreshAheadSeconds = 5 * 60;

// Sends the OAuth requests to base instead of oauth2.googleapis.com. For tests.
void SetOAuthBaseForTests(std::string base);

// Forgets the token kept in memory, a stale token.json and the sign-in snapshot, without revoking
// anything. For tests.
void ResetSignInForTests();

// Timeouts, redirects and the CA store for Google's servers.
void ConfigureClient(httplib::Client& client);

struct ClientCredentials {
    std::string client_id;
    std::string client_secret;

    bool operator==(const ClientCredentials&) const = default;
};

// The client in a client_secret.json Google Cloud console gives out: the "installed" one, the
// kind a "TVs and Limited Input devices" client is saved as.
std::optional<ClientCredentials> ParseClientSecret(std::string_view json);

// Saves a client_secret.json as ConfigDir/cloud_sync/client_secret.json, which then takes the
// place of the build's client. False when it has no client.
bool ImportClientSecret(std::string_view json);

// The imported client, else the one the build has. nullopt when there is neither.
std::optional<ClientCredentials> LoadClientCredentials();

// True when an imported client_secret.json is in effect instead of the build's client.
bool ClientImported();

// True when ConfigDir/cloud_sync/token.json holds a refresh token.
bool SignedIn();

// The sign-in state as last seen by a sign-in, sign-out, refresh or token read, without waiting:
// SignedIn waits for a refresh in flight, so a GUI thread asks this instead. A sync reports a
// sign-out that this doesn't show yet.
bool SignedInSnapshot();

struct DeviceCode {
    std::string user_code;
    std::string verification_url;
    s64 expires_in{};
};

enum class SignInStatus {
    SignedIn,
    // The user refused access.
    Denied,
    // The user didn't answer before the code expired.
    Expired,
    Cancelled,
    // No client, Google can't be reached or refused the request.
    Failed,
};

struct SignInResult {
    SignInStatus status{};
    // For Failed: what went wrong, for the user.
    std::string error;
};

// Asks Google for a code, hands it to show_code for the user to enter at its URL, then polls
// until they answer, the code expires or stop is requested, which also aborts a request in flight.
// Saves the token when they allow it. Cancelled also when SignOut or another SignIn comes first,
// even with a grant in flight. Failed when the token can't be saved.
SignInResult SignIn(const std::function<void(const DeviceCode&)>& show_code, std::stop_token stop);

// The access token, refreshed first when it expires within RefreshAheadSeconds or force_refresh
// is set (after a 401). nullopt when signed out or the refresh failed. Google refusing the refresh
// token (invalid_grant) signs out, as SignOut does. A token from another client than the current
// one (an imported client_secret.json) isn't refreshed: that needs a new sign-in. stop ends the
// wait for another caller's refresh and aborts this one's request in flight. A refreshed token
// the file can't take is kept in memory.
std::optional<std::string> AccessToken(bool force_refresh = false, std::stop_token stop = {});

struct SessionToken {
    // nullopt when there is none: error says why, for the user.
    std::optional<std::string> access_token;
    // The sign-in it is from: each sign-in and sign-out starts another.
    u64 session{};
    std::string error;
};

// AccessToken, also saying why there is none. With session set, none when the current sign-in is
// another, so that a request isn't retried under another account.
SessionToken SessionAccessToken(bool force_refresh, std::optional<u64> session,
                                std::stop_token stop = {});

// Forgets the token and revokes it on Google. Blocks on the request. Signed out even when
// token.json can't be removed: it is then ignored, and removed when possible, until the next
// sign-in, but read again after a restart (unless revoked). False in that case, for the user to
// know a copy is left on disk.
bool SignOut();

} // namespace FrontendCommon::CloudSync
