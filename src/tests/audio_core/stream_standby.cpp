// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "audio_core/sink/stream_standby.h"
#include "tests/audio_core/sysmix_test_context.h"

namespace {

using namespace std::chrono_literals;
using AudioCore::Sink::SinkBuffer;
using AudioCore::Sink::StreamType;
using Clock = std::chrono::steady_clock;

/// Long enough for slow and sanitized runners: every wait ends as soon as its condition holds.
constexpr auto Deadline = 10000ms;

/// A stream whose backend can be lost and reopened, as a cubeb stream's can. Its backends have no
/// callback, so nothing consumes the stream unless the stand-in does. Too large for the stack.
class LosableStream final : public AudioCore::Sink::StandbySinkStream {
public:
    struct Backend {
        bool running{};
    };

    LosableStream(Core::System& emu_system, StreamType stream_type, int failed_opens,
                  bool open = true)
        : StandbySinkStream{emu_system, stream_type, {50ms, 200ms}} {
        device_channels = 2;
        SetSystemChannels(2);
        SetRingSize(4);
        failures_left = failed_opens;
        // As a cubeb stream does when there is no cubeb context.
        if (open) {
            OpenInitialBackend();
        }
        constructed = true;
    }
    ~LosableStream() override {
        Finalize();
    }

    using StandbySinkStream::IsPublished;
    using StandbySinkStream::RenderFromBackend;
    using StandbySinkStream::IsStandingIn;
    using StandbySinkStream::ReopenAttempts;
    using StandbySinkStream::Reopens;

    /// What a backend's callback reports when it gives up on its device.
    void ReportLost() {
        BackendLost();
    }
    bool Running() {
        std::scoped_lock lock{mutex};
        return std::ranges::any_of(live, [](const Backend* entry) { return entry->running; });
    }
    /// The only backend alive, or null.
    void* Only() {
        std::scoped_lock lock{mutex};
        return live.size() == 1 ? *live.begin() : nullptr;
    }
    std::size_t Live() {
        std::scoped_lock lock{mutex};
        return live.size();
    }
    std::vector<Clock::time_point> AttemptTimes() {
        std::scoped_lock lock{mutex};
        return attempt_times;
    }
    std::vector<u32> AttemptNumbers() {
        std::scoped_lock lock{mutex};
        return attempt_numbers;
    }

    std::atomic<int> failures_left{};
    /// Reopened backends that report a loss as soon as they open, as PipeWire's can.
    std::atomic<int> lose_on_open{};
    std::atomic<int> start_failures_left{};
    std::atomic<u32> destroyed{};
    std::atomic<u32> published{};
    /// While set, reopens block in OpenBackend, as one can while devices come and go.
    std::atomic<bool> hold_open{};
    /// A reopen is blocked on hold_open.
    std::atomic<bool> opening{};
    /// Runs in StartBackend, with the stream's control lock held.
    std::function<void()> on_start;
    /// The backend StartBackend was last called with.
    std::atomic<void*> last_started{};

private:
    void* OpenBackend(u32 attempt) override {
        Backend* opened{};
        {
            std::scoped_lock lock{mutex};
            if (constructed) {
                attempt_times.push_back(Clock::now());
                attempt_numbers.push_back(attempt);
            }
        }
        if (constructed && hold_open) {
            opening = true;
            while (hold_open) {
                std::this_thread::sleep_for(1ms);
            }
            opening = false;
        }
        {
            std::scoped_lock lock{mutex};
            if (failures_left.fetch_sub(1) > 0) {
                return nullptr;
            }
            opened = *live.insert(new Backend{}).first;
        }
        if (constructed && lose_on_open.fetch_sub(1) > 0) {
            BackendLost();
        }
        return opened;
    }
    bool StartBackend(void* backend) override {
        last_started = backend;
        if (start_failures_left.fetch_sub(1) > 0) {
            return false;
        }
        {
            std::scoped_lock lock{mutex};
            static_cast<Backend*>(backend)->running = true;
        }
        if (on_start) {
            on_start();
        }
        return true;
    }
    bool StopBackend(void* backend) override {
        std::scoped_lock lock{mutex};
        static_cast<Backend*>(backend)->running = false;
        return true;
    }
    void DestroyBackend(void* backend) override {
        std::scoped_lock lock{mutex};
        live.erase(static_cast<Backend*>(backend));
        delete static_cast<Backend*>(backend);
        ++destroyed;
    }
    void BackendPublished(void*) override {
        ++published;
    }

    std::mutex mutex;
    std::set<Backend*> live;
    std::vector<Clock::time_point> attempt_times;
    std::vector<u32> attempt_numbers;
    std::atomic<bool> constructed{};
};

/// Lets blocked opens through when it goes, before the stream is closed, which waits for them.
struct OpenGate {
    LosableStream& stream;
    ~OpenGate() {
        stream.hold_open = false;
    }
};

std::unique_ptr<LosableStream> MakeStream(SysmixTests::Context& context, StreamType type,
                                          int failed_opens = 0) {
    return std::make_unique<LosableStream>(context.system, type, failed_opens);
}

/// Queue blocks as the audio renderer does, waiting for free space after each. Returns whether it
/// finished within the deadline; a renderer left waiting is released before returning.
bool Render(LosableStream& stream, u32 blocks) {
    std::atomic<bool> done{};
    std::jthread renderer([&](std::stop_token stop_token) {
        SinkBuffer block{240, 0, 0, false};
        std::array<s16, 480> samples{};
        for (u32 i = 0; i < blocks && !stop_token.stop_requested(); ++i) {
            stream.AppendBuffer(block, samples);
            stream.WaitFreeSpace(stop_token);
        }
        done = !stop_token.stop_requested();
    });
    const auto until = Clock::now() + Deadline;
    while (!done && Clock::now() < until) {
        std::this_thread::sleep_for(1ms);
    }
    renderer.request_stop();
    renderer.join();
    return done;
}

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

/// Queue blocks as the audio renderer does, paced by whatever consumes the stream, until the stream
/// is paused; then a fixed batch: enough to fill the queue and wait, were nothing to release it,
/// and far fewer than the queue holds. Paused, nothing paces the renderer, so it must not keep
/// queueing: a full queue blocks AppendBuffer with no way to stop it.
/// `started` is set once a block has been queued while the stream played: close only after it.
std::jthread RenderUntilPausedThenBatch(LosableStream& stream, std::atomic<bool>& started,
                                        std::atomic<bool>& finished) {
    return std::jthread([&stream, &started, &finished](std::stop_token stop_token) {
        SinkBuffer block{240, 0, 0, false};
        std::array<s16, 480> samples{};
        int after_pause = 0;
        while (after_pause < 32 && !stop_token.stop_requested()) {
            stream.AppendBuffer(block, samples);
            stream.WaitFreeSpace(stop_token);
            if (stream.IsPaused()) {
                ++after_pause;
            } else {
                started = true;
            }
        }
        finished = !stop_token.stop_requested();
    });
}

/// Run an owner call on a thread of its own. False if it is still waiting after a deadline, as it
/// would on a blocked open: blocked opens are then let through, so that it and the test end.
bool ReturnsWhileOpenBlocks(LosableStream& stream, const std::function<void()>& call) {
    std::atomic<bool> done{};
    std::jthread caller([&] {
        call();
        done = true;
    });
    const bool returned = WaitFor([&] { return done.load(); });
    if (!returned) {
        stream.hold_open = false;
    }
    return returned;
}

} // Anonymous namespace

TEST_CASE("A stream that lost its device plays into nothing and reopens with backoff",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    auto owned = MakeStream(context, StreamType::Render);
    auto& stream = *owned;
    stream.Start();
    REQUIRE(stream.Running());
    stream.failures_left = 2;
    const auto lost = Clock::now();
    stream.ReportLost();
    REQUIRE(stream.IsReady());

    // The renderer keeps its pace and is never left waiting for queue space no device frees.
    REQUIRE(Render(stream, 40));

    REQUIRE(WaitFor([&] { return stream.Reopens() == 1; }));
    CHECK(stream.destroyed == 1);
    CHECK(stream.Live() == 1);
    CHECK(stream.Running());
    CHECK_FALSE(stream.IsStandingIn());
    CHECK(stream.IsReady());
    CHECK(stream.AttemptNumbers() == std::vector<u32>{1, 2, 3});
    const auto attempts = stream.AttemptTimes();
    REQUIRE(attempts.size() == 3);
    CHECK(attempts[0] - lost >= 50ms);
    CHECK(attempts[1] - attempts[0] >= 100ms);
    CHECK(attempts[2] - attempts[1] >= 200ms);

    // Once reopened, only the device consumes the stream.
    const u32 queued = stream.GetQueueSize();
    SinkBuffer block{240, 0, 0, false};
    std::array<s16, 480> samples{};
    stream.AppendBuffer(block, samples);
    std::this_thread::sleep_for(30ms);
    CHECK(stream.GetQueueSize() == queued + 1);
}

TEST_CASE("A lost stream keeps its owner's pause and resume through the reopen",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    auto owned = MakeStream(context, StreamType::Render);
    auto& stream = *owned;
    stream.failures_left = 1000;
    stream.ReportLost();

    // Paused, it is not played, as a stopped device would not be.
    SinkBuffer block{240, 0, 0, false};
    std::array<s16, 480> samples{};
    stream.AppendBuffer(block, samples);
    stream.AppendBuffer(block, samples);
    std::this_thread::sleep_for(30ms);
    CHECK(stream.GetQueueSize() == 2);

    // A resume while the device is lost is kept, and it plays into nothing.
    stream.Start();
    CHECK_FALSE(stream.IsPaused());
    REQUIRE(WaitFor([&] { return stream.GetQueueSize() == 0; }));

    // Paused again before the device returns, it reopens paused, and starts on the next resume.
    stream.Stop();
    stream.failures_left = 0;
    REQUIRE(WaitFor([&] { return stream.Reopens() == 1; }));
    CHECK_FALSE(stream.Running());
    CHECK(stream.IsPaused());
    stream.Start();
    CHECK(stream.Running());
}

TEST_CASE("A stop while a reopen starts the new backend stops it", "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    // Before the stream, whose worker runs on_start: they outlive it however the test ends.
    std::atomic<bool> armed{};
    std::atomic<bool> starting{};
    auto owned = MakeStream(context, StreamType::Render);
    auto& stream = *owned;
    for (u64 cycle = 1; cycle <= 5; ++cycle) {
        INFO("cycle " << cycle);
        stream.Start();
        // The emulation stops the stream while the reopen starts the new backend: the stop waits
        // on the control lock for the reopen to publish it, and then has to stop it.
        armed = true;
        starting = false;
        stream.on_start = [&] {
            if (armed.exchange(false)) {
                starting = true;
                std::this_thread::sleep_for(20ms);
            }
        };
        stream.ReportLost();
        REQUIRE(WaitFor([&] { return starting.load(); }));
        stream.Stop();
        stream.on_start = nullptr;
        CHECK(stream.Reopens() == cycle);
        CHECK_FALSE(stream.Running());
        CHECK(stream.IsPaused());
    }
}

TEST_CASE("A loss the new backend reports before it is published drops that backend",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    auto owned = MakeStream(context, StreamType::Render);
    auto& stream = *owned;
    stream.Start();
    stream.lose_on_open = 1;
    stream.ReportLost();
    REQUIRE(WaitFor([&] { return stream.Reopens() == 1; }));
    CHECK(stream.AttemptNumbers() == std::vector<u32>{1, 2});
    // The lost one, and the one that reported a loss as it opened.
    CHECK(stream.destroyed == 2);
    CHECK(stream.Live() == 1);
    CHECK(stream.Running());
    CHECK_FALSE(stream.IsStandingIn());
}

TEST_CASE("A stream stays ready while reopened backends fail to start",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    auto owned = MakeStream(context, StreamType::Render);
    auto& stream = *owned;
    stream.Start();
    stream.start_failures_left = 2;
    std::atomic<bool> watching{true};
    std::atomic<bool> was_not_ready{};
    std::jthread watcher([&] {
        while (watching) {
            was_not_ready = was_not_ready || !stream.IsReady();
        }
    });
    stream.ReportLost();
    const bool reopened = WaitFor([&] { return stream.Reopens() == 1; });
    watching = false;
    watcher.join();
    REQUIRE(reopened);
    CHECK_FALSE(was_not_ready);
    CHECK(stream.AttemptNumbers() == std::vector<u32>{1, 2, 3});
    CHECK(stream.Running());
}

TEST_CASE("A renderer waiting on a lost stream is released at shutdown",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    auto owned = MakeStream(context, StreamType::Render);
    auto& stream = *owned;
    stream.Start();
    stream.failures_left = 1000;
    stream.ReportLost();
    context.system.SetShuttingDown(true);
    const bool released = Render(stream, 20);
    context.system.SetShuttingDown(false);
    REQUIRE(released);
}

TEST_CASE("Closing a stream that stands in releases a renderer waiting on it",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    auto owned = MakeStream(context, StreamType::Render);
    auto& stream = *owned;
    stream.Start();
    stream.failures_left = 1000;
    stream.ReportLost();
    std::atomic<bool> started{};
    std::atomic<bool> finished{};
    std::jthread renderer = RenderUntilPausedThenBatch(stream, started, finished);
    REQUIRE(WaitFor([&] { return started.load(); }));
    // With the stand-in stopped nothing frees queue space: only the pause lets the renderer on.
    stream.Finalize();
    const bool released = WaitFor([&] { return finished.load(); });
    renderer.request_stop();
    renderer.join();
    CHECK(released);
    CHECK_FALSE(stream.IsReady());
}

TEST_CASE("A loss reported after the stream is closed is ignored", "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    auto owned = MakeStream(context, StreamType::Render);
    auto& stream = *owned;
    stream.Start();
    stream.Finalize();
    // As a backend can when it is destroyed.
    stream.ReportLost();
    CHECK_FALSE(stream.IsStandingIn());
    CHECK_FALSE(stream.IsReady());
    std::this_thread::sleep_for(100ms);
    CHECK(stream.ReopenAttempts() == 0);
}

TEST_CASE("A stream its owner faulted stays faulted when its device is lost",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    SECTION("faulted before the loss") {
        auto owned = MakeStream(context, StreamType::Render);
        auto& stream = *owned;
        stream.MarkFaulted();
        stream.ReportLost();
        CHECK_FALSE(stream.IsReady());
        CHECK_FALSE(stream.IsStandingIn());
        stream.Start();
        CHECK(stream.IsPaused());
        std::this_thread::sleep_for(150ms);
        CHECK(stream.ReopenAttempts() == 0);
    }
    SECTION("faulted while standing in") {
        auto owned = MakeStream(context, StreamType::Render);
        auto& stream = *owned;
        stream.Start();
        stream.failures_left = 1000;
        stream.ReportLost();
        REQUIRE(WaitFor([&] { return stream.ReopenAttempts() >= 1; }));
        stream.MarkFaulted();
        stream.failures_left = 0;
        CHECK_FALSE(stream.IsStandingIn());
        CHECK_FALSE(stream.IsReady());
        // Standing down, the worker pauses the stream: no renderer waits on it for good.
        CHECK(WaitFor([&] { return stream.IsPaused(); }));
        CHECK(Render(stream, 20));
        // The worker stands down at its next check; then nothing reopens.
        std::this_thread::sleep_for(300ms);
        const u64 attempts = stream.ReopenAttempts();
        std::this_thread::sleep_for(300ms);
        CHECK(stream.ReopenAttempts() == attempts);
        CHECK(stream.Reopens() == 0);
        CHECK(stream.Live() == 0);
    }
}

TEST_CASE("Only renderer and audio out streams stand in when their first open fails",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    for (const StreamType type : {StreamType::Render, StreamType::Out}) {
        auto owned = MakeStream(context, type, 2);
        auto& stream = *owned;
        INFO("type " << static_cast<int>(type));
        CHECK(stream.IsReady());
        CHECK(stream.IsStandingIn());
        // Two failures: the initial open and the first reopen.
        REQUIRE(WaitFor([&] { return stream.Reopens() == 1; }));
        CHECK(stream.AttemptNumbers() == std::vector<u32>{1, 2});
    }
    // Their owners handle it: sysmix fails the guest's open, input streams capture silence.
    for (const StreamType type : {StreamType::Sysmix, StreamType::In}) {
        auto owned = MakeStream(context, type, 1);
        auto& stream = *owned;
        INFO("type " << static_cast<int>(type));
        CHECK_FALSE(stream.IsReady());
        CHECK_FALSE(stream.IsStandingIn());
        std::this_thread::sleep_for(100ms);
        CHECK(stream.ReopenAttempts() == 0);
    }
}

TEST_CASE("A lost input or sysmix stream is not ready and is not reopened, as before",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    for (const StreamType type : {StreamType::Sysmix, StreamType::In}) {
        auto owned = MakeStream(context, type);
        auto& stream = *owned;
        INFO("type " << static_cast<int>(type));
        stream.Start();
        REQUIRE(stream.IsReady());
        stream.ReportLost();
        CHECK_FALSE(stream.IsReady());
        CHECK_FALSE(stream.IsStandingIn());
        stream.Stop();
        stream.Start();
        CHECK(stream.IsPaused());
        std::this_thread::sleep_for(100ms);
        CHECK(stream.ReopenAttempts() == 0);
    }
}

TEST_CASE("A stream that never opened is not ready and stays paused",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    for (const StreamType type : {StreamType::Render, StreamType::Out, StreamType::In}) {
        auto owned = std::make_unique<LosableStream>(context.system, type, 0, false);
        auto& stream = *owned;
        INFO("type " << static_cast<int>(type));
        CHECK_FALSE(stream.IsReady());
        CHECK_FALSE(stream.IsStandingIn());
        stream.Start();
        CHECK(stream.IsPaused());
        // Nothing would consume it: a renderer must not be left waiting for queue space.
        CHECK(Render(stream, 20));
        stream.ReportLost();
        CHECK(stream.ReopenAttempts() == 0);
    }
}

TEST_CASE("A stream stands in while a reopen is still opening", "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    auto owned = MakeStream(context, StreamType::Render);
    auto& stream = *owned;
    OpenGate gate{stream};
    stream.Start();
    stream.hold_open = true;
    stream.ReportLost();
    REQUIRE(WaitFor([&] { return stream.opening.load(); }));

    // Neither the renderer nor the owner's pause and resume wait for the open.
    REQUIRE(Render(stream, 40));
    REQUIRE(ReturnsWhileOpenBlocks(stream, [&] { stream.Stop(); }));
    CHECK(stream.IsPaused());
    REQUIRE(ReturnsWhileOpenBlocks(stream, [&] { stream.Start(); }));
    CHECK_FALSE(stream.IsPaused());
    CHECK(stream.opening);
    CHECK(stream.IsStandingIn());
    CHECK(stream.IsReady());
    CHECK(stream.ReopenAttempts() == 1);

    stream.hold_open = false;
    REQUIRE(WaitFor([&] { return stream.Reopens() == 1; }));
    CHECK(stream.Running());
    CHECK(stream.Live() == 1);
    CHECK(stream.published == 2);
}

TEST_CASE("Closing a stream while a reopen is opening releases its renderer before the open ends",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    auto owned = MakeStream(context, StreamType::Render);
    auto& stream = *owned;
    OpenGate gate{stream};
    stream.Start();
    stream.hold_open = true;
    stream.ReportLost();
    REQUIRE(WaitFor([&] { return stream.opening.load(); }));
    // As in the close test above: once paused, a batch of blocks that only the pause lets through.
    std::atomic<bool> started{};
    std::atomic<bool> finished{};
    std::jthread renderer = RenderUntilPausedThenBatch(stream, started, finished);
    REQUIRE(WaitFor([&] { return started.load(); }));
    // The close waits for the open, which cannot be cancelled; the renderer must not. It is
    // paused while the open still blocks, and then renders its last blocks: the stand-in,
    // stopped by the close, frees no queue space for them.
    std::atomic<bool> closed{};
    std::jthread closer([&] {
        stream.Finalize();
        closed = true;
    });
    const bool released = WaitFor([&] { return finished.load(); });
    const bool closed_before_open_ended = closed.load();
    const bool still_opening = stream.opening.load();
    stream.hold_open = false;
    closer.join();
    renderer.request_stop();
    renderer.join();
    CHECK(released);
    CHECK(still_opening);
    CHECK_FALSE(closed_before_open_ended);
    // The backend it opened was dropped, not published.
    CHECK(stream.Live() == 0);
    CHECK(stream.Reopens() == 0);
    CHECK(stream.destroyed == 2);
    CHECK_FALSE(stream.IsReady());
}

TEST_CASE("A fault while a reopened backend starts drops that backend",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    std::atomic<bool> armed{true};
    auto owned = MakeStream(context, StreamType::Render);
    auto& stream = *owned;
    stream.Start();
    // The owner faults the stream while the worker starts the new backend.
    stream.on_start = [&armed, faulted = owned.get()] {
        if (armed.exchange(false)) {
            faulted->MarkFaulted();
        }
    };
    stream.ReportLost();
    REQUIRE(WaitFor([&] { return stream.destroyed == 2; }));
    CHECK(WaitFor([&] { return stream.IsPaused(); }));
    std::this_thread::sleep_for(150ms);
    CHECK(stream.Reopens() == 0);
    CHECK(stream.Live() == 0);
    CHECK(stream.published == 1);
    CHECK_FALSE(stream.IsReady());
}

TEST_CASE("A stream that stands in plays nothing after Stop returns",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    auto owned = MakeStream(context, StreamType::Render);
    auto& stream = *owned;
    stream.failures_left = 1000;
    stream.ReportLost();
    SinkBuffer block{240, 0, 0, false};
    std::array<s16, 480> samples{};
    // The owner clears the queue once Stop returns, as audio out does: nothing may play it then.
    int played_after_stop = 0;
    for (int cycle = 0; cycle < 50; ++cycle) {
        stream.Start();
        for (int i = 0; i < 3; ++i) {
            stream.AppendBuffer(block, samples);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{cycle % 6});
        stream.Stop();
        const u32 queued = stream.GetQueueSize();
        std::this_thread::sleep_for(12ms);
        played_after_stop += stream.GetQueueSize() != queued;
        stream.ClearQueue();
    }
    CHECK(played_after_stop == 0);
}

TEST_CASE("A stream with a configured device chooses it, waits for it, or takes the default",
          "[audio_core][stream_standby]") {
    using AudioCore::Sink::StandbySinkStream;
    using Choice = StandbySinkStream::DeviceChoice;
    constexpr u32 waits = StandbySinkStream::ConfiguredDeviceAttempts;
    const auto choose = &StandbySinkStream::ChooseDevice;

    // The first open: the configured device, or the default one.
    CHECK(choose(0, true, false, false) == Choice::ConfiguredThenDefault);
    CHECK(choose(0, false, false, false) == Choice::Default);

    // Published on it, a stream waits for it a few reopens, even for one that fails to start.
    for (u32 attempt = 1; attempt <= waits; ++attempt) {
        INFO("attempt " << attempt);
        CHECK(choose(attempt, false, true, false) == Choice::Wait);
        CHECK(choose(attempt, true, true, false) == Choice::Configured);
        CHECK(choose(attempt, true, true, true) == Choice::Configured);
    }
    // Then it takes the default one too, and instead of one that opened but did not start.
    CHECK(choose(waits + 1, false, true, false) == Choice::Default);
    CHECK(choose(waits + 1, true, true, false) == Choice::ConfiguredThenDefault);
    CHECK(choose(waits + 1, true, true, true) == Choice::Default);

    // Published on the default one, nothing waits for the configured device: one that came back
    // but did not start, and left again, does not keep it from the default.
    for (const u32 attempt : {1U, waits, waits + 1}) {
        INFO("attempt " << attempt);
        CHECK(choose(attempt, false, false, false) == Choice::Default);
        CHECK(choose(attempt, true, false, false) == Choice::ConfiguredThenDefault);
        CHECK(choose(attempt, true, false, true) == Choice::Default);
    }
}

TEST_CASE("A backend is the stream's only once its open or reopen publishes it",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    std::atomic<int> starts{};
    std::atomic<bool> published_while_starting{true};
    auto owned = MakeStream(context, StreamType::Render);
    auto& stream = *owned;
    // The first backend is published as it opens.
    CHECK(stream.IsPublished(stream.Only()));
    CHECK_FALSE(stream.IsPublished(nullptr));
    stream.Start();
    // A reopened one is started before it is published: until then its callbacks must not
    // consume the stream, which the stand-in plays.
    stream.on_start = [&starts, &published_while_starting, reopened = owned.get()] {
        published_while_starting = reopened->IsPublished(reopened->last_started.load());
        ++starts;
    };
    stream.ReportLost();
    REQUIRE(WaitFor([&] { return stream.Reopens() == 1; }));
    CHECK(starts == 1);
    CHECK_FALSE(published_while_starting);
    CHECK(stream.IsPublished(stream.Only()));
    CHECK(stream.IsPublished(stream.last_started.load()));
}

TEST_CASE("A backend's callback plays the stream only once the backend is published",
          "[audio_core][stream_standby]") {
    SysmixTests::Context context;
    std::atomic<bool> played_while_starting{true};
    std::atomic<bool> silent_while_starting{};
    std::atomic<u32> queued_while_starting{};
    auto owned = MakeStream(context, StreamType::Render);
    auto& stream = *owned;
    stream.Start();
    // As a data callback could call while the reopen starts the backend, before it is published:
    // only the worker, here, consumes the stream then.
    stream.on_start = [&played_while_starting, &silent_while_starting, &queued_while_starting,
                       reopened = owned.get()] {
        SinkBuffer block{240, 0, 0, false};
        std::array<s16, 480> samples{};
        samples.fill(1000);
        reopened->AppendBuffer(block, samples);
        reopened->AppendBuffer(block, samples);
        std::array<s16, 480> output{};
        output.fill(123);
        played_while_starting =
            reopened->RenderFromBackend(reopened->last_started.load(), output, 240);
        silent_while_starting = std::ranges::all_of(output, [](s16 sample) { return sample == 0; });
        queued_while_starting = reopened->GetQueueSize();
    };
    stream.ReportLost();
    REQUIRE(WaitFor([&] { return stream.Reopens() == 1; }));
    CHECK_FALSE(played_while_starting);
    CHECK(silent_while_starting);
    CHECK(queued_while_starting == 2);
    // Published, it plays the stream.
    const u32 queued = stream.GetQueueSize();
    std::array<s16, 480> output{};
    CHECK(stream.RenderFromBackend(stream.Only(), output, 240));
    CHECK(stream.GetQueueSize() == queued - 1);
}

TEST_CASE("An error during a backend's open is handled by exactly one of the open and the callback",
          "[audio_core][stream_standby]") {
    using AudioCore::Sink::BackendOpenState;
    SECTION("before the open returns") {
        BackendOpenState state;
        CHECK_FALSE(state.ReportError());
        // A second error of the same open, as one from the failed open's cleanup, is its too.
        CHECK_FALSE(state.ReportError());
        CHECK_FALSE(state.FinishOpen());
        CHECK_FALSE(state.ReportError());
        CHECK_FALSE(state.Lost());
    }
    SECTION("after the open returned") {
        BackendOpenState state;
        CHECK(state.FinishOpen());
        CHECK(state.ReportError());
        CHECK(state.Lost());
        // A later error is a loss again.
        CHECK(state.ReportError());
    }
    SECTION("racing") {
        // Whichever comes first, the open drops the backend exactly when the callback does not
        // report a loss.
        int handled_by_open = 0;
        int handled_by_callback = 0;
        // Few enough rounds, and waits that yield, for a loaded two-core runner: a pure spin waits
        // a whole time slice for the other side whenever both are not on a core.
        for (int i = 0; i < 200; ++i) {
            BackendOpenState state;
            // Both sides wait for each other, then go at once.
            std::atomic<int> ready{};
            bool reported{};
            std::thread callback([&] {
                ++ready;
                while (ready.load() < 2) {
                    std::this_thread::yield();
                }
                reported = state.ReportError();
            });
            ++ready;
            while (ready.load() < 2) {
                std::this_thread::yield();
            }
            const bool opened = state.FinishOpen();
            callback.join();
            INFO("iteration " << i);
            REQUIRE(opened == reported);
            (opened ? handled_by_callback : handled_by_open) += 1;
        }
        // Which side wins depends on the scheduler: reported, not required.
        UNSCOPED_INFO("handled by the open " << handled_by_open << ", by the callback "
                                             << handled_by_callback);
    }
}
