// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/nextendo.h"
#include "common/settings.h"
#include "core/hle/service/sockets/nextendo_redirect.h"

namespace Nextendo = Service::Sockets::Nextendo;

TEST_CASE("Nextendo: host classification", "[core]") {
    REQUIRE(Nextendo::Classify("nncs2-lp1.n.n.srv.nintendo.net") == Nextendo::Target::Nat);
    REQUIRE(Nextendo::Classify("g2b309e01-lp1.s.n.srv.nintendo.net") == Nextendo::Target::Nat);
    REQUIRE(Nextendo::Classify("nncs1-lp1.n.n.srv.nintendo.net") == Nextendo::Target::Server);
    REQUIRE(Nextendo::Classify("dauth-lp1.ndas.srv.nintendo.net") == Nextendo::Target::Server);
    REQUIRE(Nextendo::Classify("conntest.nintendowifi.net") == Nextendo::Target::Server);
    REQUIRE(Nextendo::Classify("nintendo.com") == Nextendo::Target::Server);
    REQUIRE_FALSE(Nextendo::Classify("notnintendo.com").has_value());
    REQUIRE_FALSE(Nextendo::Classify("example.org").has_value());
    REQUIRE(Nextendo::Classify("stun.us.demonware.net") == Nextendo::Target::Server);

    constexpr u64 Strive = 0x0100AB401F136000;
    constexpr u64 WorldWarZ = 0x010099F013898000;
    REQUIRE(Nextendo::Classify("api.epicgames.dev", Strive) == Nextendo::Target::Server);
    REQUIRE_FALSE(Nextendo::Classify("api.epicgames.dev").has_value());
    REQUIRE_FALSE(Nextendo::Classify("api.epicgames.dev", WorldWarZ).has_value());
    REQUIRE(Nextendo::Classify("wwz-eu.wwzapi.com", WorldWarZ) == Nextendo::Target::Server);
    REQUIRE_FALSE(Nextendo::Classify("wwzapi.com", WorldWarZ).has_value());
}

TEST_CASE("Nextendo: redirects only while online play is on", "[core]") {
    Settings::values.nextendo_enabled = false;
    REQUIRE_FALSE(Nextendo::RedirectAddress("dauth-lp1.ndas.srv.nintendo.net").has_value());

    Settings::values.nextendo_enabled = true;
    Settings::values.nextendo_server_ip = "192.0.2.1";
    Settings::values.nextendo_nat_ip = "192.0.2.2";
    if constexpr (Common::Nextendo::OnlinePlayAvailable) {
        REQUIRE(Nextendo::RedirectAddress("dauth-lp1.ndas.srv.nintendo.net") == "192.0.2.1");
        REQUIRE(Nextendo::RedirectAddress("nncs2-lp1.n.n.srv.nintendo.net") == "192.0.2.2");
    } else {
        REQUIRE_FALSE(Nextendo::RedirectAddress("dauth-lp1.ndas.srv.nintendo.net").has_value());
    }
    REQUIRE_FALSE(Nextendo::RedirectAddress("example.org").has_value());

    Settings::values.nextendo_enabled = false;
    Settings::values.nextendo_server_ip.SetValue(Settings::values.nextendo_server_ip.GetDefault());
    Settings::values.nextendo_nat_ip.SetValue(Settings::values.nextendo_nat_ip.GetDefault());
}

TEST_CASE("Nextendo: NSD host names", "[core]") {
    REQUIRE(Nextendo::ExpandNsdHost("g2b309e01-%.s.n.srv.nintendo.net") ==
            "g2b309e01-lp1.s.n.srv.nintendo.net");
    REQUIRE(Nextendo::ExpandNsdHost("example.org") == "example.org");
}

TEST_CASE("Nextendo: NPLN ALPN and Gamesync route only while online play is on", "[core]") {
    const std::string wire = "\x02h2\x08http/1.1\x03"
                             "foo";
    const std::vector<u8> requested(wire.begin(), wire.end());
    const Network::SockAddrIn gamesync{Network::Domain::INET, {192, 0, 2, 1}, 7575};
    constexpr u64 Scarlet = 0x0100A3D008C5C000;
    Settings::values.nextendo_server_ip = "192.0.2.1";

    Settings::values.nextendo_enabled = false;
    REQUIRE(Nextendo::AlpnProtocols("gamesync.npln.nintendo.net", requested).empty());
    REQUIRE_FALSE(Nextendo::RouteGamesync(Scarlet, gamesync).has_value());

    Settings::values.nextendo_enabled = true;
    if constexpr (Common::Nextendo::OnlinePlayAvailable) {
        REQUIRE(Nextendo::AlpnProtocols("gamesync.npln.nintendo.net", requested) ==
                std::vector<u8>(requested.begin(), requested.begin() + 12));
        REQUIRE(Nextendo::AlpnProtocols("gs.nintendo.net", requested).size() == 12);
        REQUIRE(Nextendo::AlpnProtocols("dauth-lp1.ndas.srv.nintendo.net", requested).empty());
        REQUIRE(Nextendo::RouteGamesync(Scarlet, gamesync)->portno == 8463);
        REQUIRE(Nextendo::RouteGamesync(Scarlet, {Network::Domain::INET, {}, 7575})->ip ==
                gamesync.ip);
        REQUIRE_FALSE(Nextendo::RouteGamesync(0x0100C2500FC20000, gamesync).has_value());
    } else {
        REQUIRE(Nextendo::AlpnProtocols("gamesync.npln.nintendo.net", requested).empty());
        REQUIRE_FALSE(Nextendo::RouteGamesync(Scarlet, gamesync).has_value());
    }

    Settings::values.nextendo_enabled = false;
    Settings::values.nextendo_server_ip.SetValue(Settings::values.nextendo_server_ip.GetDefault());
}
