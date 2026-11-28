// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <boost/asio.hpp>
#include <boost/crc.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "common/input.h"
#include "common/param_package.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "input_common/drivers/udp_client.h"
#include "input_common/helpers/udp_protocol.h"
#include "input_common/input_poller.h"


class FakeCemuhookServer {
public:
    FakeCemuhookServer()
        : socket(io_context, boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::loopback(), 0)) {}

    ~FakeCemuhookServer() {
        boost::system::error_code error_code;
        socket.shutdown(boost::asio::socket_base::shutdown_both, error_code);
        socket.close();
    }

    u16 GetPort() {
        return socket.local_endpoint().port();
    }

    std::string GetHost() {
        return socket.local_endpoint().address().to_string();
    }

    void Run(const std::vector<InputCommon::CemuhookUDP::Response::TouchPad> touch_movement_path) {
        constexpr size_t HeaderSize = sizeof(InputCommon::CemuhookUDP::Header);
        constexpr size_t PadDataSize =
            sizeof(InputCommon::CemuhookUDP::Message<InputCommon::CemuhookUDP::Response::PadData>);

        REQUIRE(touch_movement_path.size() > 0);
        handler = std::jthread([touch_movement_path, this](std::stop_token stoken) {
            auto current_touch_position = touch_movement_path.begin();
            u32 packet_counter = 0;
            while (!stoken.stop_requested()) {
                boost::asio::ip::udp::endpoint sender_endpoint;
                boost::system::error_code error_code;
                auto received_size = socket.receive_from(boost::asio::buffer(receive_buffer),
                                                         sender_endpoint, 0, error_code);

                if (received_size < HeaderSize) {
                    continue;
                }

                InputCommon::CemuhookUDP::Header header{};
                std::memcpy(&header, receive_buffer.data(), HeaderSize);
                switch (header.type) {
                case InputCommon::CemuhookUDP::Type::PadData: {
                    InputCommon::CemuhookUDP::Response::PadData pad_data{};
                    // UDPClient drops a packet that repeats the previous counter as stale
                    pad_data.packet_counter = ++packet_counter;
                    pad_data.touch[0] = *current_touch_position;
                    const auto pad_message = InputCommon::CemuhookUDP::CreateMessage(
                        InputCommon::CemuhookUDP::SERVER_MAGIC, pad_data, 0);
                    std::memcpy(send_buffer.data(), &pad_message, PadDataSize);
                    socket.send_to(boost::asio::buffer(send_buffer, PadDataSize), sender_endpoint,
                                   0, error_code);

                    bool can_advance =
                        std::next(current_touch_position) != touch_movement_path.end();
                    if (can_advance) {
                        std::advance(current_touch_position, 1);
                    }
                    break;
                }
                case InputCommon::CemuhookUDP::Type::PortInfo:
                case InputCommon::CemuhookUDP::Type::Version:
                default:
                    break;
                }
            }
        });
    }

private:
    boost::asio::io_context io_context;
    boost::asio::ip::udp::socket socket;
    std::array<u8, InputCommon::CemuhookUDP::MAX_PACKET_SIZE> send_buffer;
    std::array<u8, InputCommon::CemuhookUDP::MAX_PACKET_SIZE> receive_buffer;
    std::jthread handler;
};

TEST_CASE("CalibrationConfigurationJob completed", "[input_common]") {
    Common::Event complete_event;
    FakeCemuhookServer server;
    server.Run({{
                    .is_active = 1,
                    .x = 0,
                    .y = 0,
                },
                {
                    .is_active = 1,
                    .x = 200,
                    .y = 200,
                }});

    InputCommon::CemuhookUDP::CalibrationConfigurationJob::Status status{};
    u16 min_x{};
    u16 min_y{};
    u16 max_x{};
    u16 max_y{};
    InputCommon::CemuhookUDP::CalibrationConfigurationJob job(
        server.GetHost(), server.GetPort(),
        [&status,
         &complete_event](InputCommon::CemuhookUDP::CalibrationConfigurationJob::Status status_) {
            status = status_;
            if (status ==
                InputCommon::CemuhookUDP::CalibrationConfigurationJob::Status::Completed) {
                complete_event.Set();
            }
        },
        [&](u16 min_x_, u16 min_y_, u16 max_x_, u16 max_y_) {
            min_x = min_x_;
            min_y = min_y_;
            max_x = max_x_;
            max_y = max_y_;
        });

    complete_event.WaitUntil(std::chrono::system_clock::now() + std::chrono::seconds(10));
    REQUIRE(status == InputCommon::CemuhookUDP::CalibrationConfigurationJob::Status::Completed);
    REQUIRE(min_x == 0);
    REQUIRE(min_y == 0);
    REQUIRE(max_x == 200);
    REQUIRE(max_y == 200);
}

TEST_CASE("UDPClient touch reaches the console touchscreen", "[input_common]") {
    FakeCemuhookServer server;
    server.Run({{
        .is_active = 1,
        .x = 950,
        .y = 450,
    }});

    const auto previous_servers = Settings::values.udp_input_servers.GetValue();
    const auto previous_touch_device = Settings::values.touch_device.GetValue();
    SCOPE_EXIT {
        Settings::values.udp_input_servers.SetValue(previous_servers);
        Settings::values.touch_device.SetValue(previous_touch_device);
    };
    Settings::values.udp_input_servers.SetValue(
        fmt::format("{}:{}", server.GetHost(), server.GetPort()));
    Settings::values.touch_device.SetValue("min_x:100,min_y:50,max_x:1800,max_y:850");

    Common::Event touched;
    Common::Input::TouchStatus status{};
    {
        const auto udp_client =
            std::make_shared<InputCommon::CemuhookUDP::UDPClient>("cemuhookudp");
        InputCommon::InputFactory factory(udp_client);
        // Finger 1 as EmulatedConsole::SetTouchParams asks for it
        const auto touch = factory.Create(
            Common::ParamPackage{"engine:cemuhookudp,axis_x:17,axis_y:18,button:65536"});
        touch->SetCallback({.on_change = [&](const Common::Input::CallbackStatus& callback) {
            if (callback.touch_status.pressed.value) {
                status = callback.touch_status;
                touched.Set();
            }
        }});
        REQUIRE(touched.WaitUntil(std::chrono::system_clock::now() + std::chrono::seconds(10)));
    }
    REQUIRE(status.x.raw_value == 0.5f);
    REQUIRE(status.y.raw_value == 0.5f);
}

TEST_CASE("CalibrationConfigurationJob cancellation joins its worker", "[input_common]") {
    FakeCemuhookServer server;
    for (unsigned i = 0; i < 32; ++i) {
        auto lifetime = std::make_shared<int>(0);
        std::weak_ptr<int> observer = lifetime;
        {
            InputCommon::CemuhookUDP::CalibrationConfigurationJob job(
                server.GetHost(), server.GetPort(),
                [lifetime](InputCommon::CemuhookUDP::CalibrationConfigurationJob::Status) {},
                [](u16, u16, u16, u16) {});
            lifetime.reset();
            job.Stop();
        }
        REQUIRE(observer.expired());
    }
}
