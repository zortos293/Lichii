// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <system_error>
#include <utility>

#include "audio_core/common/common.h"
#include "audio_core/sink/stream_standby.h"
#include "common/assert.h"
#include "common/logging.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/core.h"

#ifdef _WIN32
#include <objbase.h>
#endif

namespace AudioCore::Sink {

namespace {
constexpr std::chrono::microseconds BlockPeriod{TargetSampleCount * 1'000'000ULL /
                                                TargetSampleRate};

/// For the backend calls made on the stream's own threads.
struct ComScope {
#ifdef _WIN32
    ComScope() : result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)} {}
    ~ComScope() {
        if (SUCCEEDED(result)) {
            CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    HRESULT result;
#endif
};
} // Anonymous namespace

StandbySinkStream::DeviceChoice StandbySinkStream::ChooseDevice(StreamType type, u32 attempt,
                                                                bool listed,
                                                                bool published_on_configured,
                                                                bool start_failed) {
    if (!StandsIn(type)) {
        // A microphone the user chose that does not open is not replaced by another one, nor a
        // sysmix output by another output: their owners capture silence or fail the guest's open.
        return listed ? DeviceChoice::Configured : DeviceChoice::Default;
    }
    const bool may_take_default =
        attempt == 0 || attempt > ConfiguredDeviceAttempts || !published_on_configured;
    if (!listed) {
        return may_take_default ? DeviceChoice::Default : DeviceChoice::Wait;
    }
    if (!may_take_default) {
        return DeviceChoice::Configured;
    }
    return start_failed ? DeviceChoice::Default : DeviceChoice::ConfiguredThenDefault;
}

StandbySinkStream::StandbySinkStream(Core::System& system_, StreamType type_, Timing timing_)
    : SinkStream{system_, type_}, timing{timing_} {}

StandbySinkStream::~StandbySinkStream() {
    ASSERT_MSG(!worker.joinable(), "Stream {}: the derived destructor must call Finalize", name);
    StopWorker();
}

bool StandbySinkStream::IsReady() const {
    if (failed) {
        return false;
    }
    switch (state.load()) {
    case State::Live:
        return true;
    case State::Lost:
    case State::Reopening:
        return StandsIn();
    case State::Stopped:
        return false;
    }
    return false;
}

bool StandbySinkStream::RenderFromBackend(const void* backend, std::span<s16> output,
                                          std::size_t num_frames) {
    if (!IsPublished(backend)) {
        // Opening or starting during a reopen, while the stand-in may play the stream.
        std::fill(output.begin(), output.end(), s16{0});
        return false;
    }
    ProcessAudioOutAndRender(output, num_frames);
    return true;
}

bool StandbySinkStream::IsStandingIn() const {
    const State current = state.load();
    return StandsIn() && !failed && (current == State::Lost || current == State::Reopening);
}

void StandbySinkStream::OpenInitialBackend() {
    void* const opened = OpenBackend(0);
    {
        // As a reopen publishes: a loss reported once it is live has the worker release it, which
        // waits for this lock, so it is not destroyed before BackendPublished.
        std::scoped_lock control{control_mutex};
        bool published{};
        {
            std::scoped_lock lock{state_mutex};
            published = opened && losses == 0;
            if (published) {
                live_backend = opened;
                state = State::Live;
            } else {
                state = State::Lost;
            }
        }
        if (published) {
            BackendPublished(opened);
            return;
        }
        // It did not open, or reported a loss while it did.
        if (opened) {
            DestroyBackend(opened);
        }
    }
    if (!StandsIn()) {
        // Not ready, as the owners of input and sysmix streams expect of one that did not open.
        return;
    }
    LOG_WARNING(Audio_Sink, "Stream {} has no device yet: playing it into nothing until one opens",
                name);
    {
        std::scoped_lock lock{state_mutex};
        if (state == State::Lost && !worker.joinable()) {
            worker = std::jthread([this](std::stop_token stop_token) { Run(stop_token); });
        }
    }
    wake.notify_all();
}

void StandbySinkStream::BackendLost() {
    {
        std::scoped_lock lock{state_mutex};
        ++losses;
        if (state != State::Live) {
            // Already lost, finalized, or reported by a backend being reopened, which the reopen
            // then drops.
            return;
        }
        state = State::Lost;
        if (!StandsIn()) {
            LOG_WARNING(Audio_Sink, "Stream {} lost its device", name);
            return;
        }
        LOG_WARNING(Audio_Sink,
                    "Stream {} lost its device: playing it into nothing until it reopens", name);
        if (!worker.joinable()) {
            worker = std::jthread([this](std::stop_token stop_token) { Run(stop_token); });
        }
    }
    wake.notify_all();
}

void StandbySinkStream::Start(bool) {
    std::scoped_lock control{control_mutex};
    if (failed || !paused) {
        return;
    }
    void* current{};
    {
        std::scoped_lock lock{state_mutex};
        if (state != State::Live) {
            if (state == State::Stopped || !StandsIn()) {
                return;
            }
            // Standing in: play into nothing, and on the device once a backend reopens.
            paused = false;
            current = nullptr;
        } else {
            current = live_backend;
        }
    }
    if (!current) {
        wake.notify_all();
        return;
    }
    // Not once a close has marked the stream stopped and paused it during StartBackend: a renderer
    // would wait for queue space nothing frees.
    const auto unpause = [this] {
        std::scoped_lock lock{state_mutex};
        if (state != State::Stopped) {
            paused = false;
        }
    };
    if (!StartBackend(current)) {
        LOG_ERROR(Audio_Sink, "Error starting stream {}", name);
        // A stream that stands in plays into nothing until it reopens; others stay not ready
        // and paused, as their owners expect of one that did not start.
        if (StandsIn()) {
            unpause();
        }
        BackendLost();
        return;
    }
    unpause();
}

void StandbySinkStream::Stop() {
    std::scoped_lock control{control_mutex};
    if (paused) {
        return;
    }
    SignalPause();
    State current_state;
    void* current{};
    {
        std::scoped_lock lock{state_mutex};
        current_state = state;
        current = live_backend;
    }
    if (current_state == State::Live && current && !StopBackend(current)) {
        LOG_ERROR(Audio_Sink, "Error stopping stream {}", name);
        BackendLost();
    }
}

void StandbySinkStream::Finalize() {
    const bool playing = !paused;
    // The worker releases and reopens the backend: join it before destroying that. It pauses the
    // stream first, so a renderer is not left waiting for an open under way.
    const State before = StopWorker();
    std::scoped_lock control{control_mutex};
    void* current{};
    {
        std::scoped_lock lock{state_mutex};
        current = live_backend.exchange(nullptr);
    }
    if (current) {
        if (playing && before == State::Live) {
            StopBackend(current);
        }
        DestroyBackend(current);
    }
}

StandbySinkStream::State StandbySinkStream::StopWorker() {
    std::jthread joining;
    State before;
    {
        std::scoped_lock lock{state_mutex};
        before = state;
        state = State::Stopped;
        joining = std::move(worker);
    }
    SignalPause();
    if (joining.joinable()) {
        joining.request_stop();
        wake.notify_all();
        joining.join();
    }
    return before;
}

void StandbySinkStream::Run(std::stop_token stop_token) {
    Common::SetCurrentThreadName("AudioStandby");
    [[maybe_unused]] ComScope com;
    stand_in_block.resize(TargetSampleCount * GetDeviceChannels());
    bool running = true;
    while (running) {
        {
            std::unique_lock lock{state_mutex};
            if (!wake.wait(lock, stop_token, [this] { return state == State::Lost; })) {
                break;
            }
        }
        if (failed) {
            // The owner's fault is final: neither stand in nor reopen, and pause the stream so
            // that nothing waits for queue space only the stand-in would free.
            std::scoped_lock control{control_mutex};
            SignalPause();
            break;
        }
        // Destroying the lost backend stops its callbacks: from here on only this thread
        // consumes the stream, until a new backend is started.
        ReleaseBackend();
        next_block = std::chrono::steady_clock::now();
        auto delay = timing.first_retry;
        for (u32 attempt = 1;; ++attempt) {
            if (!StandIn(stop_token, std::chrono::steady_clock::now() + delay)) {
                running = false;
                break;
            }
            if (failed) {
                // Faulted while standing in: stand down, and pause the stream so that nothing is
                // left waiting for queue space only the stand-in freed.
                std::scoped_lock control{control_mutex};
                SignalPause();
                running = false;
                break;
            }
            const Reopen result = TryReopen(stop_token, attempt);
            if (result == Reopen::Opened) {
                break;
            }
            if (result == Reopen::Stopped) {
                running = false;
                break;
            }
            delay = (std::min)(delay * 2, timing.max_retry);
        }
    }
}

void StandbySinkStream::ReleaseBackend() {
    std::scoped_lock control{control_mutex};
    void* current{};
    {
        std::scoped_lock lock{state_mutex};
        current = live_backend.exchange(nullptr);
    }
    if (current) {
        DestroyBackend(current);
    }
}

StandbySinkStream::Reopen StandbySinkStream::TryReopen(std::stop_token stop_token, u32 attempt) {
    u64 seen{};
    {
        std::scoped_lock lock{state_mutex};
        if (state == State::Stopped) {
            return Reopen::Stopped;
        }
        state = State::Reopening;
        seen = losses;
    }
    ++attempts;
    void* opened{};
    std::atomic<bool> open_done{};
    std::jthread opener;
    try {
        // On a thread of its own, without any lock: opening may block while the device comes and
        // goes, and the stand-in plays on meanwhile. The backend consumes the stream only once it
        // is published.
        opener = std::jthread([this, attempt, &opened, &open_done] {
            Common::SetCurrentThreadName("AudioOpener");
            [[maybe_unused]] ComScope com;
            void* const result = system.IsShuttingDown() ? nullptr : OpenBackend(attempt);
            {
                std::scoped_lock lock{state_mutex};
                opened = result;
                open_done = true;
            }
            wake.notify_all();
        });
    } catch (const std::system_error& error) {
        // No thread for the open: try again later, standing in meanwhile.
        LOG_ERROR(Audio_Sink, "Stream {} could not start a thread to reopen: {}", name,
                  error.what());
    }
    if (opener.joinable()) {
        StandIn(stop_token, std::chrono::steady_clock::time_point::max(), &open_done);
        if (!open_done && failed) {
            // Faulted with the open under way: nothing frees queue space while the opener is
            // joined. A close has paused the stream already.
            std::scoped_lock control{control_mutex};
            SignalPause();
        }
        opener.join();
    }
    std::scoped_lock control{control_mutex};
    if (opened) {
        // Not finalized, faulted, or lost again by the new backend. Checked again once it starts.
        const auto usable = [this, seen] {
            std::scoped_lock lock{state_mutex};
            return state == State::Reopening && losses == seen && !failed;
        };
        // Started before it is published, still standing in: the stream stays ready throughout.
        if (usable() && !system.IsShuttingDown() && (paused || StartBackend(opened))) {
            bool published{};
            {
                std::scoped_lock lock{state_mutex};
                published = state == State::Reopening && losses == seen && !failed;
                if (published) {
                    live_backend = opened;
                    state = State::Live;
                }
            }
            if (published) {
                BackendPublished(opened);
                ++reopens;
                LOG_INFO(Audio_Sink, "Stream {} reopened after {} attempts{}", name, attempt,
                         paused ? ", paused" : "");
                return Reopen::Opened;
            }
        }
        // Closing, shutting down, faulted, not started, or the new backend reported a loss: drop
        // it.
        DestroyBackend(opened);
    }
    std::scoped_lock lock{state_mutex};
    if (state == State::Reopening) {
        state = State::Lost;
    }
    return state == State::Stopped ? Reopen::Stopped : Reopen::Failed;
}

bool StandbySinkStream::StandIn(std::stop_token stop_token,
                                std::chrono::steady_clock::time_point until,
                                const std::atomic<bool>* done) {
    const auto is_done = [done] { return done && done->load(); };
    while (true) {
        if (stop_token.stop_requested()) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= until || failed || is_done()) {
            return true;
        }
        if (paused) {
            // As a stopped device would, play nothing until Start, which signals it.
            std::unique_lock lock{state_mutex};
            wake.wait_until(lock, stop_token, (std::min)(until, now + std::chrono::seconds{1}),
                            [this, &is_done] { return !paused.load() || is_done(); });
            next_block = std::chrono::steady_clock::now();
            continue;
        }
        if (now < next_block) {
            if (!Common::StoppableTimedWait(stop_token, (std::min)(next_block, until) - now)) {
                return false;
            }
            continue;
        }
        {
            // Under the lock Stop holds as it pauses: no block plays after Stop returns, when the
            // owner may clear the queue.
            std::scoped_lock control{control_mutex};
            if (paused) {
                continue;
            }
            ProcessAudioOutAndRender(stand_in_block, TargetSampleCount);
        }
        // The clock runs on while a backend starts, so that time is made up for afterwards; but
        // after a host stall skip all but 20 blocks of it.
        next_block = (std::max)(next_block + BlockPeriod, now - 20 * BlockPeriod);
    }
}

} // namespace AudioCore::Sink
