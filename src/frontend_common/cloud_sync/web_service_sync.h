// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <stop_token>

#include "frontend_common/cloud_sync/sync_engine.h"
#include "frontend_common/cloud_sync/sync_state.h"

// The web service login on Drive (WebServiceLogin in sync_engine.h).
namespace FrontendCommon::CloudSync {

// For SyncAll, with SyncStateMutex held: syncs local, this device's login. Drive's, when taken,
// goes to result.web_service_login. False when Drive couldn't be reached.
bool SyncWebServiceLogin(const std::optional<WebServiceLogin>& local,
                         const SyncCallbacks& callbacks, SyncState& state, SyncResult& result,
                         std::stop_token stop);

} // namespace FrontendCommon::CloudSync
