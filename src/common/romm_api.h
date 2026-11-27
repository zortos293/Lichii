// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/romm_config.h"

// Thin client for a self-hosted RomM (https://romm.app) instance: browsing/downloading a Switch
// content catalogue (base game + updates + DLC) and syncing save data through RomM's Device Sync
// Protocol. Authentication is OAuth2: short-lived access tokens are obtained with the password kept
// in the system credential store and renewed with refresh tokens held only in memory; every call
// below that takes Credentials handles that transparently.
//
// Every request/response shape here was checked against a real instance's own openapi.json and
// against live runs. Parsing is still defensive/optional throughout, so a field missing on some
// other RomM version means an empty list or a failed call, never a crash.
namespace Common::RommApi {

struct TestConnectionResult {
    bool ok = false;
    std::string error; // Set when ok is false: network failure, 401, etc.
    std::string server_version; // Best-effort, empty if not present in the response.
};

// Verifies the given instance URL + token actually work — used both for the master account and
// for testing a profile's own account before saving it. Safe to call on every Settings dialog
// open; does not throw on a misconfigured or unreachable instance.
TestConnectionResult TestConnection(const RommConfig::Credentials& credentials);

struct LoginResult {
    bool ok = false;
    std::string error; // Set when ok is false: network failure, wrong password, missing scope...
    // Ready to be passed (with the password) to RommConfig::Save()/Profile::Save(), which is what
    // puts the password in the system credential store.
    RommConfig::Credentials credentials;
};

// OAuth2 password grant against RomM's /api/token: checks the username and password and primes the
// in-memory session (access + refresh token) so the next call doesn't need to log in again. The
// password isn't kept by this call; persisting it is RommConfig::Save()'s job. `scope` is one of
// RommConfig::CatalogScope / SaveSyncScope. Blocks on the network, so don't call it on the UI
// thread.
LoginResult Login(const std::string& instance_url, const std::string& username,
                  const std::string& password, const std::string& scope);

// Drops the in-memory access token for this account (call when the account is removed).
void ForgetSession(const RommConfig::Credentials& credentials);

// RomM's internal slugs for the two consoles Lichii plays: Nintendo Switch and Nintendo Switch 2.
inline constexpr const char* SwitchPlatformSlug = "switch";
inline constexpr const char* Switch2PlatformSlug = "switch-2";

struct Platform {
    u64 id = 0;
    std::string slug; // e.g. "switch" or "switch-2"
    std::string name; // e.g. "Nintendo Switch"
};

// Returns the Nintendo Switch and Nintendo Switch 2 platforms the instance has (none of the
// instance's other platforms are relevant to Lichii). Empty vector on any failure (network, auth,
// parse) rather than throwing.
std::vector<Platform> ListPlatforms();

enum class RomFileCategory {
    Main,
    Update,
    Dlc,
    Unknown,
};

struct RomFile {
    u64 id = 0; // RomM's own per-file id — required as ?file_ids= to get just this file (see
                // DownloadRomContent's comment for why the filename alone isn't enough).
    std::string filename; // Pass verbatim to DownloadContent().
    RomFileCategory category = RomFileCategory::Unknown;
    u64 size_bytes = 0;
};

struct Rom {
    u64 id = 0;
    std::string name;
    std::string platform_slug; // "switch", "switch-2", ...; empty if the server didn't say
    std::vector<RomFile> files;
};

// Empty vector on failure. `platform_id` comes from ListPlatforms().
std::vector<Rom> ListRoms(u64 platform_id);

// Every rom on the instance's Switch and Switch 2 platforms, Switch first. Empty if the instance
// has neither platform or on failure.
std::vector<Rom> ListSwitchRoms();

std::optional<Rom> GetRom(u64 rom_id);

// Cached in-process after the first successful call (the platform ids on a given RomM instance
// never change at runtime). Empty if the instance has neither a "switch" nor a "switch-2"
// platform.
std::vector<Platform> FindSwitchPlatforms();

// Streams GET /api/roms/{rom_id}/content/{filename}?file_ids={file_id} straight to `dest_path`,
// calling `on_progress(bytes_received, total_bytes)` as data arrives (total_bytes is 0 if the
// server didn't send Content-Length). The `?file_ids=` is not optional decoration: RomM's content
// route only serves a single bare file when the rom has exactly one file total — for any
// multi-file rom (an update or DLC living alongside the base game and its other DLC, which is the
// common case) the same URL without it streams a zip of every file the rom owns instead, with no
// Content-Length at all. `file_id` is RomFile::id from the same detailed Rom this filename came
// from. If `should_cancel` is set and returns true, the transfer is aborted at the next chunk
// boundary. Returns false on any failure or cancellation; partial output is deleted either way.
bool DownloadRomContent(u64 rom_id, u64 file_id, const std::string& filename,
                        const std::string& dest_path, const std::function<void(u64, u64)>& on_progress,
                        const std::function<bool()>& should_cancel = {});

// ---- Device Sync Protocol (cloud save backend) -----------------------------------------------
// Every field/type/endpoint shape below is transcribed from a real RomM instance's own
// openapi.json (SyncNegotiatePayload, ClientSaveState, SyncNegotiateResponse, SyncOperationSchema,
// DeviceCreatePayload/Response, Body_add_save_api_saves_post, SyncCompletePayload,
// SyncPlaySessionEntry) rather than from the docs prose: device_id and session_id are different
// JSON types (see their comments below).
//
// Unlike the catalogue functions above (which always use the one master account), every function
// here takes an explicit Common::RommConfig::Credentials rather than reading global config: sync
// is per-profile (see romm_config.h), and the caller — not this module — decides which profile is
// active and whether it has an account configured at all. If RommConfig::Profile::HasAccount()
// is false for the active profile, the caller should simply not call any of these; there is
// deliberately no "credentials.IsSet() == false" fallback behavior baked in here beyond returning
// a clean failure, so a missing account can never silently fall back to someone else's token.

struct SaveFileMeta {
    std::string filename;
    std::string mtime_rfc3339; // Sent as ClientSaveState.updated_at.
    std::string sha1_hex;      // Sent as ClientSaveState.content_hash — an opaque string as far as
                               // the server's concerned, only ever compared against itself.
    u64 size_bytes = 0;        // ClientSaveState.file_size_bytes — required by the schema.
};

// Reads `path` in chunks and returns its SHA-1 as 40 lowercase hex chars, or nullopt if it
// couldn't be opened. Pure I/O + hashing, no network. Only ever used on the zipped save archive
// (small, cheap), never on game files.
std::optional<std::string> Sha1HexOfFile(const std::string& path);

enum class SyncOpType { Upload, Download, Conflict, NoOp };

struct SyncOperation {
    SyncOpType type = SyncOpType::NoOp;
    u64 rom_id = 0;
    std::string file_name;
    // Present (non-nullopt) for Download and Conflict — the server-side save to fetch via
    // DownloadSave(). Always nullopt for Upload: there is nothing to download yet in that case.
    std::optional<u64> save_id;
    // Echoes the slot this operation applies to (see NegotiateSync's own comment on why a stable
    // slot matters); not currently branched on, but kept since it's free to carry and any future
    // multi-slot support would need it.
    std::string slot;
    std::string reason; // Human-readable, from the server — worth logging on an unexpected result.
};

struct NegotiateResult {
    bool ok = false;
    std::string error;
    u64 session_id = 0; // SyncNegotiateResponse.session_id is a JSON integer, not a string.
    std::vector<SyncOperation> operations;
};

// Registers this Lichii install as a device the first time it's needed for this profile's account;
// caches the returned id keyed by (instance_url, token) for subsequent calls. `hostname` is
// informational only (shown in the RomM web UI's device list). `device_name` should identify the
// profile, e.g. "Lichii - <profile name>", since one RomM instance may see several Lichii profiles as
// separate devices if each has its own account. Returns the device's id as a *string*
// (DeviceCreateResponse.device_id — a UUID-shaped string on the server, not a numeric id).
std::optional<std::string> RegisterDeviceIfNeeded(const RommConfig::Credentials& credentials,
                                                   const std::string& device_name,
                                                   const std::string& hostname);

// One rom's worth of negotiation is enough for a single-title Pull/Push; SyncNegotiatePayload
// supports batching several roms' saves in one `saves` array, but nothing here needs that yet.
// `slot` should be a stable, consistent name across every call for the same logical save (e.g.
// always "default") — RomM pairs client and server saves on (rom_id, slot); a save sent with no
// slot is treated as an archival, one-off upload and can never be matched back for a Download the
// way a slotted save can, which would silently turn every "sync" into "upload and never pull".
NegotiateResult NegotiateSync(const RommConfig::Credentials& credentials,
                              const std::string& device_id, u64 rom_id, const std::string& slot,
                              const SaveFileMeta& save);

// POSTs to /api/saves (multipart, field name "saveFile" — confirmed via openapi.json, not the
// generic "file" an earlier version of this guessed) with the raw save bytes. `slot` must match
// whatever was negotiated, for the same pairing reason NegotiateSync's own comment explains.
bool UploadSave(const RommConfig::Credentials& credentials, u64 rom_id, const std::string& slot,
                const std::string& device_id, u64 session_id, const std::string& filename,
                const std::vector<u8>& data);

// GETs /api/saves/{save_id}/content; returns the raw bytes on success. `save_id` comes from a
// Download or Conflict SyncOperation's save_id field — there's no separately-provided URL for
// this in the protocol, unlike an earlier, incorrect version of this comment assumed.
std::optional<std::vector<u8>> DownloadSave(const RommConfig::Credentials& credentials,
                                            u64 save_id);

// A save stored on the server for a rom, as listed by GET /api/saves — regardless of which client
// uploaded it.
struct RemoteSave {
    u64 save_id = 0;
    std::string file_name;
    std::string slot;       // Empty if the save has no slot.
    std::string emulator;   // Empty if the uploader didn't say (manual upload, other tools).
    std::string updated_at; // As sent by the server (ISO 8601), empty if missing.
    u64 size_bytes = 0;

    // True for saves Lichii's own sync uploaded (see NegotiateSync's "emulator" field).
    bool UploadedByLichii() const {
        return emulator == "lichii";
    }
};

// Every save the authenticated user has for `rom_id`, newest first. nullopt if the request or the
// parsing failed (an empty vector means the request worked and there simply are none).
std::optional<std::vector<RemoteSave>> ListSaves(const RommConfig::Credentials& credentials,
                                                 u64 rom_id);

struct PlaySession {
    u64 rom_id = 0;
    std::string start_time_rfc3339;
    std::string end_time_rfc3339;
    u64 duration_ms = 0;
};

// session_id is the integer NegotiateResult::session_id from the negotiation this session
// completes — not a string, matching SyncNegotiateResponse.session_id's actual type.
bool CompleteSession(const RommConfig::Credentials& credentials, u64 session_id,
                     u32 operations_completed, u32 operations_failed,
                     const std::vector<PlaySession>& play_sessions);

} // namespace Common::RommApi
