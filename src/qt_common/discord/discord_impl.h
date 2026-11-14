// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2018 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include <QNetworkAccessManager>

#include "discord.h"

namespace Core {
class System;
}

namespace DiscordRPC {

class DiscordImpl : public DiscordInterface {
public:
    DiscordImpl(Core::System& system_);
    ~DiscordImpl() override;

    void Pause() override;
    void Update(s64 session_start) override;

private:
    void CancelCoverRequest();
    void RequestCover();
    void RequestCommunityCover(std::string title_id);
    void RequestFallbackCover();
    void UpdateGameStatus();

    Core::System& system;
    QNetworkAccessManager network;
    QNetworkReply* cover_reply{};
    std::string game_title;
    std::string cover_title;
    std::string custom_title;
    std::string game_version;
    std::string cover_url;
    u64 game_id{};
    s64 game_start{};
};

} // namespace DiscordRPC
