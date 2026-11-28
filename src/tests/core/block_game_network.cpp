// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <optional>
#include <string>
#include <thread>
#include <variant>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/internal_network/local_redirect.h"
#include "core/internal_network/network.h"
#include "core/internal_network/sockets.h"

namespace LocalRedirect = Network::LocalRedirect;

namespace {

constexpr Network::IPv4Address Loopback{127, 0, 0, 1};
constexpr Network::SockAddrIn Public{Network::Domain::INET, {192, 0, 2, 7}, 443};
constexpr Network::SockAddrIn LoopbackHttps{Network::Domain::INET, Loopback, 443};

void SetBlocked(bool blocked) {
    Settings::values.block_game_network.SetValue(blocked);
    LocalRedirect::Configure();
}

Network::SockAddrIn SockName(Network::Socket& socket) {
    const auto [name, error] = socket.GetSockName();
    REQUIRE(error == Network::Errno::SUCCESS);
    return name;
}

} // Anonymous namespace

TEST_CASE("Blocked game network: off by default, and what it turns on", "[core][network]") {
    Network::NetworkInstance network_instance;
    REQUIRE_FALSE(Settings::values.block_game_network.GetDefault());
    const bool nextendo = Settings::values.nextendo_enabled.GetValue();
    SCOPE_EXIT {
        Settings::values.nextendo_enabled.SetValue(nextendo);
        SetBlocked(false);
    };
    Settings::values.nextendo_enabled.SetValue(true);

    SetBlocked(false);
    CHECK_FALSE(LocalRedirect::Isolated());
    CHECK_FALSE(LocalRedirect::LoopbackOnly());
    CHECK(LocalRedirect::Allowed(Public));
    CHECK(LocalRedirect::OnlinePlayAllowed());

    // Without the host redirect: loopback only, and no online services for the game.
    SetBlocked(true);
    CHECK(LocalRedirect::Isolated());
    CHECK(LocalRedirect::LoopbackOnly());
    CHECK_FALSE(LocalRedirect::Allowed(Public));
    CHECK(LocalRedirect::Allowed({Network::Domain::INET, Loopback, 80}));
    CHECK_FALSE(LocalRedirect::OnlinePlayAllowed());

    // Host names don't reach the host's resolver; localhost and numeric loopback still resolve.
    CHECK(std::holds_alternative<Network::GetAddrInfoError>(
        Network::GetAddressInfo("example.com", std::nullopt)));
    CHECK(std::holds_alternative<Network::GetAddrInfoError>(
        Network::GetAddressInfo("192.0.2.7", std::nullopt)));
    CHECK_FALSE(std::holds_alternative<Network::GetAddrInfoError>(
        Network::GetAddressInfo("localhost", std::nullopt)));

    SetBlocked(false);
    CHECK_FALSE(LocalRedirect::Isolated());
    CHECK(LocalRedirect::Allowed(Public));

    // BCAT prepares for a game before the snapshot is taken, so Isolated follows the setting.
    Settings::values.block_game_network.SetValue(true);
    CHECK(LocalRedirect::Isolated());
    CHECK_FALSE(LocalRedirect::OnlinePlayAllowed());
}

TEST_CASE("Blocked game network: a redirect server isn't reached", "[core][network]") {
    SCOPE_EXIT {
        Settings::values.online_service_redirect.SetValue(false);
        Settings::values.online_redirect_server.SetValue({});
        SetBlocked(false);
    };
    Settings::values.online_service_redirect.SetValue(true);
    Settings::values.online_redirect_server.SetValue("192.0.2.1:8443");

    SetBlocked(false);
    CHECK(LocalRedirect::ConnectTarget(LoopbackHttps).ip == Network::IPv4Address{192, 0, 2, 1});

    SetBlocked(true);
    const Network::SockAddrIn target = LocalRedirect::ConnectTarget(LoopbackHttps);
    CHECK(target.ip == Loopback);
    CHECK(target.portno == 443);
}

TEST_CASE("Blocked game network: sockets stay on loopback", "[core][network]") {
    Network::NetworkInstance network_instance;
    SetBlocked(true);
    SCOPE_EXIT {
        SetBlocked(false);
    };

    SECTION("a wildcard bind lands on loopback, another address is refused") {
        Network::Socket socket;
        REQUIRE(socket.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                                  Network::Protocol::UDP) == Network::Errno::SUCCESS);
        // ADDRNOTAVAIL itself: the IPv4 OTHER it used to be reaches the guest as success.
        CHECK(socket.Bind({Network::Domain::INET, {192, 0, 2, 7}, 0}) ==
              Network::Errno::ADDRNOTAVAIL);
        REQUIRE(socket.Bind({Network::Domain::INET, {0, 0, 0, 0}, 0}) == Network::Errno::SUCCESS);
        CHECK(SockName(socket).ip == Loopback);
    }

    SECTION("an unbound datagram socket is bound to loopback by its first send") {
        Network::Socket receiver;
        REQUIRE(receiver.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                                    Network::Protocol::UDP) == Network::Errno::SUCCESS);
        REQUIRE(receiver.Bind({Network::Domain::INET, Loopback, 0}) == Network::Errno::SUCCESS);
        const Network::SockAddrIn receiver_addr = SockName(receiver);

        Network::Socket sender;
        REQUIRE(sender.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                                  Network::Protocol::UDP) == Network::Errno::SUCCESS);
        constexpr std::array<u8, 4> message{'p', 'i', 'n', 'g'};
        CHECK(sender.SendTo(0, message, &Public).second == Network::Errno::NETUNREACH);
        const auto [sent, send_error] = sender.SendTo(0, message, &receiver_addr);
        REQUIRE(send_error == Network::Errno::SUCCESS);
        CHECK(sent == static_cast<s32>(message.size()));
        const Network::SockAddrIn sender_addr = SockName(sender);
        CHECK(sender_addr.ip == Loopback);
        CHECK(sender_addr.portno != 0);

        std::array<u8, 16> buffer{};
        Network::SockAddrIn from{};
        const auto [received, receive_error] = receiver.RecvFrom(0, buffer, &from);
        REQUIRE(receive_error == Network::Errno::SUCCESS);
        CHECK(received == static_cast<s32>(message.size()));
        CHECK(from.ip == Loopback);
    }

    SECTION("an unbound listener listens on loopback, and outside connections are refused") {
        Network::Socket listener;
        REQUIRE(listener.Initialize(Network::Domain::INET, Network::Type::STREAM,
                                    Network::Protocol::TCP) == Network::Errno::SUCCESS);
        REQUIRE(listener.Listen(1) == Network::Errno::SUCCESS);
        CHECK(SockName(listener).ip == Loopback);

        Network::Socket client;
        REQUIRE(client.Initialize(Network::Domain::INET, Network::Type::STREAM,
                                  Network::Protocol::TCP) == Network::Errno::SUCCESS);
        CHECK(client.Connect(Public) != Network::Errno::SUCCESS);
    }

    SECTION("two threads' first sends on one socket both go through") {
        Network::Socket receiver;
        REQUIRE(receiver.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                                    Network::Protocol::UDP) == Network::Errno::SUCCESS);
        REQUIRE(receiver.Bind({Network::Domain::INET, Loopback, 0}) == Network::Errno::SUCCESS);
        const Network::SockAddrIn receiver_addr = SockName(receiver);
        constexpr std::array<u8, 1> message{'x'};
        for (int round = 0; round < 32; ++round) {
            Network::Socket sender;
            REQUIRE(sender.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                                      Network::Protocol::UDP) == Network::Errno::SUCCESS);
            std::array<Network::Errno, 2> errors{};
            const auto send = [&](size_t index) {
                errors[index] = sender.SendTo(0, message, &receiver_addr).second;
            };
            {
                std::jthread first{send, 0};
                std::jthread second{send, 1};
            }
            CHECK(errors[0] == Network::Errno::SUCCESS);
            CHECK(errors[1] == Network::Errno::SUCCESS);
        }
    }
}
