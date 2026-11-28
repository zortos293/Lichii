// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/sockets/bsd.h"
#include "core/internal_network/network.h"
#include "tests/audio_core/sysmix_test_context.h"

namespace {

struct Reply {
    s32 ret;
    u32 bsd_errno;
};

} // namespace

TEST_CASE("BSD keeps TCP sockets on loopback in airplane mode", "[sockets][ipc]") {
    const bool airplane_mode = Settings::values.airplane_mode.GetValue();
    Settings::values.airplane_mode.SetValue(true);
    SCOPE_EXIT {
        Settings::values.airplane_mode.SetValue(airplane_mode);
    };

    Network::NetworkInstance network_instance;
    SysmixTests::Context context;
    auto& kernel = context.system.Kernel();
    Kernel::KThread client{kernel};
    Kernel::KAutoObject::Create(&client);
    REQUIRE(Kernel::KThread::InitializeDummyThread(context.system, &client, &*context.process)
                .IsSuccess());
    SCOPE_EXIT {
        context.process->UnregisterThread(kernel, &client);
        context.process->Close(kernel);
        kernel.UnregisterKernelObject(&client);
    };
    std::optional<std::jthread> server_thread;
    Service::ServerManager server{context.system};
    // The select notifier runs its host thread in the process that creates it.
    std::optional<Service::Sockets::BSD_USA> service;
    kernel
        .RunOnHostCoreProcess("bsd-airplane-notifier",
                              [&] {
                                  service.emplace(context.system, "bsd:s", false,
                                                  Service::Sockets::BSD_USA::CreateSelectNotifier(
                                                      context.system, server));
                              })
        .join();
    server_thread.emplace(
        kernel.RunOnHostCoreProcess("bsd-airplane-test", [&] { server.LoopProcess(); }));
    auto manager = std::make_shared<Service::SessionRequestManager>(kernel, server);

    constexpr u64 InAddress = SysmixTests::Context::Base;
    constexpr u64 OutAddress = SysmixTests::Context::Base + 0x100;
    u8* const out_bytes = context.backing + 0x100;
    const auto call = [&](u32 command, std::span<const u32> raw, std::span<const u8> in,
                          u32 out_size) {
        std::copy(in.begin(), in.end(), context.backing);
        std::fill_n(out_bytes, 0x100, u8{0xa5});

        std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH> words{};
        IPC::CommandHeader header{};
        header.type.Assign(IPC::CommandType::Request);
        header.enable_handle_descriptor.Assign(1);
        header.num_buf_a_descriptors.Assign(1);
        header.num_buf_b_descriptors.Assign(1);
        // Descriptors end at word 10 and padding at 11; CMIF starts at word 12, raw at 16.
        header.data_size.Assign(5 + static_cast<u32>(raw.size()));
        std::memcpy(words.data(), &header, sizeof(header));
        IPC::HandleDescriptorHeader handles{};
        handles.send_current_pid.Assign(1);
        std::memcpy(words.data() + 2, &handles, sizeof(handles));
        const auto buffer = [&](std::size_t offset, std::size_t size, u64 address) {
            IPC::BufferDescriptorABW descriptor{};
            descriptor.size_bits_0_31 = static_cast<u32>(size);
            descriptor.address_bits_0_31 = static_cast<u32>(address);
            std::memcpy(words.data() + offset, &descriptor, sizeof(descriptor));
        };
        buffer(5, in.size(), InAddress);
        buffer(8, out_size, OutAddress);
        words[12] = 0x49434653;
        words[14] = command;
        std::copy(raw.begin(), raw.end(), words.begin() + 16);

        Service::HLERequestContext request{kernel, context.Memory(), nullptr, &client};
        request.SetSessionRequestManager(manager);
        REQUIRE(request.PopulateFromIncomingCommandBuffer(words.data()).IsSuccess());
        service->InvokeRequest(request);
        REQUIRE(request.CommandBuffer()[4] == 0x4f434653);
        REQUIRE(request.CommandBuffer()[6] == ResultSuccess.raw);
        return Reply{static_cast<s32>(request.CommandBuffer()[8]), request.CommandBuffer()[9]};
    };

    constexpr u32 Inet = 2;
    constexpr u32 Stream = 1;
    constexpr u32 Tcp = 6;
    constexpr u32 AddrNotAvail = 99;
    constexpr u32 NetUnreach = 101;
    const auto open = [&] {
        const Reply socket = call(2, std::array{Inet, Stream, Tcp}, {}, 0);
        REQUIRE(socket.ret >= 0);
        return static_cast<u32>(socket.ret);
    };
    const auto close = [&](u32 fd) { call(26, std::array{fd}, {}, 0); };
    // sockaddr_in: length, family, port and address in network order.
    const std::array<u8, 16> wildcard{16, 2};
    const std::array<u8, 16> outside{16, 2, 0, 80, 192, 0, 2, 1};

    const u32 listener = open();
    SCOPE_EXIT {
        close(listener);
    };
    REQUIRE(call(13, std::array{listener}, wildcard, 0).ret == 0);
    REQUIRE(call(16, std::array{listener}, {}, 0x80).ret == 0);
    std::array<u8, 16> bound{};
    std::copy_n(out_bytes, bound.size(), bound.begin());
    CHECK(std::equal(bound.begin() + 4, bound.begin() + 8, std::array<u8, 4>{127, 0, 0, 1}.begin()));
    REQUIRE((bound[2] | bound[3]) != 0);
    REQUIRE(call(18, std::array{listener, 1u}, {}, 0).ret == 0);

    const u32 peer = open();
    SCOPE_EXIT {
        close(peer);
    };
    const Reply unreachable = call(14, std::array{peer}, outside, 0);
    CHECK(unreachable.ret == -1);
    CHECK(unreachable.bsd_errno == NetUnreach);
    REQUIRE(call(14, std::array{peer}, bound, 0).ret == 0);
    const Reply accepted = call(12, std::array{listener}, {}, 0x80);
    REQUIRE(accepted.ret >= 0);
    close(static_cast<u32>(accepted.ret));

    const u32 other = open();
    SCOPE_EXIT {
        close(other);
    };
    const Reply bind_outside = call(13, std::array{other}, outside, 0);
    CHECK(bind_outside.ret == -1);
    CHECK(bind_outside.bsd_errno == AddrNotAvail);

    // A listener the guest never bound takes a loopback port, not a wildcard one.
    REQUIRE(call(18, std::array{other, 1u}, {}, 0).ret == 0);
    REQUIRE(call(16, std::array{other}, {}, 0x80).ret == 0);
    CHECK(std::equal(out_bytes + 4, out_bytes + 8, std::array<u8, 4>{127, 0, 0, 1}.begin()));
    CHECK((out_bytes[2] | out_bytes[3]) != 0);
}
