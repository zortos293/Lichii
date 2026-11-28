// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "audio_core/adsp/apps/audio_renderer/audio_renderer.h"
#include "audio_core/renderer/command/command_list_header.h"
#include "tests/audio_core/sysmix_test_context.h"

namespace {

using namespace std::chrono_literals;
using AudioCore::ADSP::AudioRenderer::AudioRenderer;
using Clock = std::chrono::steady_clock;

/// Long enough for slow and sanitized runners: every wait ends as soon as its condition holds.
constexpr auto Deadline = 10000ms;

/// A renderer stream whose backend never asks for audio, as a stream does whose callback stopped
/// without an error. Counts the renders that start to wait for its queue space.
class StalledStream final : public AudioCore::Sink::SinkStream {
public:
    StalledStream(Core::System& emu_system, AudioCore::Sink::StreamType stream_type)
        : SinkStream{emu_system, stream_type} {
        device_channels = 2;
    }
    bool IsReady() const override {
        return true;
    }
    void Start(bool = false) override {
        paused = false;
    }
    /// Releases a render waiting for its space, as pausing a stream does.
    void Stop() override {
        SignalPause();
    }
    std::atomic<u32> parked{};

private:
    void WaitingForSpace() override {
        ++parked;
    }
};

/// Closing a stream keeps it, so that a watchdog can still release it.
class StalledSink final : public AudioCore::Sink::Sink {
public:
    AudioCore::Sink::SinkStream* AcquireSinkStream(Core::System& system, u32 channels,
        const std::string&, AudioCore::Sink::StreamType type) override {
        auto& stream = streams.emplace_back(std::make_unique<StalledStream>(system, type));
        stream->SetSystemChannels(channels);
        return stream.get();
    }
    void CloseStream(AudioCore::Sink::SinkStream*) override {}
    void CloseStreams() override {}
    f32 GetDeviceVolume() const override { return 1.0f; }
    void SetDeviceVolume(f32) override {}
    void SetSystemVolume(f32) override {}

    std::vector<std::unique_ptr<StalledStream>> streams;
};

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds deadline = Deadline) {
    const auto until = Clock::now() + deadline;
    while (!predicate()) {
        if (Clock::now() >= until) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

/// A command list with `commands` commands, which the processor refuses (they have no magic).
struct alignas(16) CommandList {
    explicit CommandList(u32 commands) {
        header.command_count = commands;
    }
    AudioCore::Renderer::CommandListHeader header{};
    std::array<u8, 0x40> commands{};
};

/// Have session 0 render `list` into a started renderer's first stream, whose queue is full past
/// where a render waits for space, and wait until that render is parked there. Returns the stream.
StalledStream& ParkRender(SysmixTests::Context& context, StalledSink& sink,
                          AudioRenderer& renderer, CommandList& list) {
    REQUIRE(!sink.streams.empty());
    auto& stream = *sink.streams[0];
    stream.Start();
    AudioCore::Sink::SinkBuffer block{240, 0, 0, false};
    std::array<s16, 480> samples{};
    for (int i = 0; i < 8; ++i) {
        stream.AppendBuffer(block, samples);
    }
    const u32 parked = stream.parked;
    renderer.SetCommandBuffer(0, reinterpret_cast<AudioCore::CpuAddr>(&list), sizeof(list),
                              2'000'000, 0, &*context.process, false);
    renderer.Signal();
    REQUIRE(WaitFor([&] { return stream.parked > parked; }));
    return stream;
}

/// Stop the renderer on a thread of its own. False if it is still stopping after the deadline:
/// a watchdog then releases the render, so that Stop, and the test, end.
bool StopsPromptly(AudioRenderer& renderer, StalledStream& stream) {
    std::atomic<bool> stopped{};
    std::jthread stopper([&] {
        renderer.Stop();
        stopped = true;
    });
    const bool prompt = WaitFor([&] { return stopped.load(); });
    if (!prompt) {
        stream.Stop();
    }
    return prompt;
}

} // Anonymous namespace

TEST_CASE("Stopping the audio renderer releases a render waiting on a stalled stream",
          "[audio_core][renderer_stalled_stream]") {
    SysmixTests::Context context;
    StalledSink sink;
    AudioRenderer renderer{context.system, sink};
    renderer.Start();
    // An empty command list: the render waits for the stream's space before processing it.
    CommandList list{0};
    auto& stream = ParkRender(context, sink, renderer, list);
    CHECK(StopsPromptly(renderer, stream));
}

TEST_CASE("A wait for a render on a stalled stream ends when it is stopped",
          "[audio_core][renderer_stalled_stream]") {
    SysmixTests::Context context;
    StalledSink sink;
    AudioRenderer renderer{context.system, sink};
    renderer.Start();
    CommandList list{0};
    auto& stream = ParkRender(context, sink, renderer, list);

    // As the system manager's thread waits for each render: the render is parked, so no response
    // can come until the wait is stopped.
    std::atomic<bool> returned{};
    std::jthread waiter([&](std::stop_token stop_token) {
        renderer.Wait(stop_token);
        returned = true;
    });
    CHECK_FALSE(returned);
    waiter.request_stop();
    const bool prompt = WaitFor([&] { return returned.load(); });
    if (!prompt) {
        // The watchdog: release the render so that the wait, and the test, end.
        stream.Stop();
    }
    waiter.join();
    CHECK(prompt);
    CHECK(StopsPromptly(renderer, stream));
}

TEST_CASE("Nothing of a render the stop ended reaches the next start",
          "[audio_core][renderer_stalled_stream]") {
    SysmixTests::Context context;
    StalledSink sink;
    AudioRenderer renderer{context.system, sink};
    renderer.Start();
    // Session 0's list has one command, which the processor refuses: once the stop releases the
    // parked render, it is left with that command remaining.
    CommandList old_list{1};
    auto& stream = ParkRender(context, sink, renderer, old_list);
    REQUIRE(StopsPromptly(renderer, stream));
    CHECK(renderer.GetRemainCommandCount(0) == 0);

    // Started again, with only session 1 set: session 0's list, whose memory a new system would
    // not own, is not processed again, nor resumed.
    renderer.Start();
    CommandList new_list{0};
    renderer.SetCommandBuffer(1, reinterpret_cast<AudioCore::CpuAddr>(&new_list),
                              sizeof(new_list), 2'000'000, 0, &*context.process, false);
    renderer.Signal();
    std::atomic<bool> rendered{};
    std::jthread waiter([&](std::stop_token stop_token) {
        renderer.Wait(stop_token);
        rendered = true;
    });
    const bool responded = WaitFor([&] { return rendered.load(); });
    waiter.request_stop();
    waiter.join();
    CHECK(responded);
    CHECK(renderer.GetRemainCommandCount(0) == 0);
    renderer.Stop();
}
