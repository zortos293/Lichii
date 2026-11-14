// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2018 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <utility>

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QString>
#include <QUrl>
#include <discord_rpc.h>
#include <fmt/format.h>

#include "common/scm_rev.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/loader/loader.h"
#include "discord_impl.h"
#include "qt_common/util/game.h"

namespace DiscordRPC {
namespace {

constexpr char DEFAULT_DISCORD_TEXT[] = "Lichii is an emulator for the Nintendo Switch";
constexpr char DEFAULT_DISCORD_IMAGE[] =
    "https://raw.githubusercontent.com/zortos293/Lichii/nx2/dist/lichii.png";

QString GetCoverSlug(const std::string& title) {
    static const QRegularExpression invalid_characters(QStringLiteral("[^a-z0-9-]"));
    static const QRegularExpression repeated_dashes(QStringLiteral("-{2,}"));
    static const QRegularExpression outer_dashes(QStringLiteral("^-|-$"));

    return QString::fromStdString(title)
        .normalized(QString::NormalizationForm_D)
        .toLower()
        .replace(QLatin1Char(' '), QLatin1Char('-'))
        .remove(invalid_characters)
        .replace(repeated_dashes, QStringLiteral("-"))
        .remove(outer_dashes);
}

QNetworkRequest MakeRequest(const QUrl& url) {
    QNetworkRequest request(url);
    request.setTransferTimeout(3000);
    return request;
}

bool RequestSucceeded(const QNetworkReply& reply) {
    return reply.error() == QNetworkReply::NoError &&
           reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
}

} // namespace

DiscordImpl::DiscordImpl(Core::System& system_) : system{system_} {
    DiscordEventHandlers handlers{};
    Discord_Initialize("1397286652128264252", &handlers, 1, nullptr);
}

DiscordImpl::~DiscordImpl() {
    Pause();
    Discord_Shutdown();
}

void DiscordImpl::Pause() {
    CancelCoverRequest();
    game_id = 0;
    game_start = 0;
    game_title.clear();
    cover_title.clear();
    custom_title.clear();
    game_version.clear();
    cover_url.clear();
    Discord_ClearPresence();
}

void DiscordImpl::CancelCoverRequest() {
    if (auto* reply = std::exchange(cover_reply, nullptr)) {
        QObject::disconnect(reply, nullptr, &network, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

void DiscordImpl::RequestCover() {
    const auto title_id = fmt::format("{:016X}", game_id);
    const QUrl name_url(
        QStringLiteral("https://raw.githubusercontent.com/mushymeow/eglantine-covers/main/%1.txt")
            .arg(QString::fromStdString(title_id)));
    cover_reply = network.get(MakeRequest(name_url));
    auto* const reply = cover_reply;

    QObject::connect(reply, &QNetworkReply::finished, &network, [this, reply, title_id] {
        if (cover_reply != reply) {
            reply->deleteLater();
            return;
        }
        cover_reply = nullptr;
        if (RequestSucceeded(*reply)) {
            const auto name = QString::fromUtf8(reply->readAll()).trimmed();
            if (!name.isEmpty()) {
                cover_title = name.toStdString();
                if (custom_title.empty()) {
                    game_title = cover_title;
                }
                UpdateGameStatus();
            }
        }
        reply->deleteLater();
        if (game_start != 0) {
            RequestCommunityCover(title_id);
        }
    });
}

void DiscordImpl::RequestCommunityCover(std::string title_id) {
    const QUrl url(
        QStringLiteral("https://raw.githubusercontent.com/mushymeow/eglantine-covers/main/%1.png")
            .arg(QString::fromStdString(title_id)));
    cover_reply = network.head(MakeRequest(url));
    auto* const reply = cover_reply;

    QObject::connect(reply, &QNetworkReply::finished, &network, [this, reply, url] {
        if (cover_reply != reply) {
            reply->deleteLater();
            return;
        }
        cover_reply = nullptr;
        const bool succeeded = RequestSucceeded(*reply);
        reply->deleteLater();
        if (succeeded) {
            cover_url = url.toString().toStdString();
            UpdateGameStatus();
        } else if (game_start != 0) {
            RequestFallbackCover();
        }
    });
}

void DiscordImpl::RequestFallbackCover() {
    const auto slug = GetCoverSlug(cover_title);
    if (slug.isEmpty()) {
        return;
    }

    const QUrl url(
        QStringLiteral("https://raw.githubusercontent.com/eden-emulator/boxart/refs/heads/master/"
                       "img/%1.png")
            .arg(slug));
    cover_reply = network.head(MakeRequest(url));
    auto* const reply = cover_reply;

    QObject::connect(reply, &QNetworkReply::finished, &network, [this, reply, url] {
        if (cover_reply != reply) {
            reply->deleteLater();
            return;
        }
        cover_reply = nullptr;
        if (RequestSucceeded(*reply)) {
            cover_url = url.toString().toStdString();
            UpdateGameStatus();
        }
        reply->deleteLater();
    });
}

void DiscordImpl::UpdateGameStatus() {
    static const auto build = fmt::format("{} {}", Common::g_build_name, Common::g_build_version);
    const auto details =
        game_version.empty() ? build : fmt::format("{} | Version {}", build, game_version);
    DiscordRichPresence presence{};

    presence.largeImageKey = cover_url.empty() ? DEFAULT_DISCORD_IMAGE : cover_url.c_str();
    presence.largeImageText = game_title.c_str();
    presence.smallImageKey = DEFAULT_DISCORD_IMAGE;
    presence.smallImageText = DEFAULT_DISCORD_TEXT;
    presence.state = game_title.c_str();
    presence.status_display_type = DiscordStatusDisplayType_State;
    presence.details = details.c_str();
    presence.startTimestamp = game_start;
    Discord_UpdatePresence(&presence);
}

void DiscordImpl::Update(s64 session_start) {
    if (session_start == 0 || !system.IsPoweredOn()) {
        Pause();
        return;
    }

    const u64 program_id = system.GetApplicationProcessProgramID();
    const auto new_custom_title = QtCommon::Game::ReadCustomTitle(program_id);
    if (game_start == session_start && game_id == program_id &&
        custom_title == new_custom_title) {
        return;
    }

    std::string title;
    if (system.GetGameName(title) != Loader::ResultStatus::Success) {
        title.clear();
    }

    std::string version;
    const FileSys::PatchManager patch_manager(program_id, system.GetFileSystemController(),
                                              system.GetContentProvider());
    const auto metadata = patch_manager.GetControlMetadata();
    if (metadata.first != nullptr) {
        version = metadata.first->GetVersionString();
        if (auto metadata_title = metadata.first->GetApplicationName(); !metadata_title.empty()) {
            title = std::move(metadata_title);
        }
    }
    if (title.empty()) {
        title = fmt::format("Nintendo Switch game ({:016X})", program_id);
    }

    if (version.empty()) {
        system.GetAppLoader().ReadDisplayVersion(version);
    }

    CancelCoverRequest();
    game_id = program_id;
    game_start = session_start;
    cover_title = std::move(title);
    custom_title = new_custom_title;
    game_title = custom_title.empty() ? cover_title : custom_title;
    game_version = std::move(version);
    cover_url.clear();

    UpdateGameStatus();
    RequestCover();
}

} // namespace DiscordRPC
