// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <span>
#include <vector>

#include "audio_core/sink/sink_stream.h"
#include "common/common_types.h"
#include "common/polyfill_thread.h"

namespace Core {
class System;
} // namespace Core

namespace AudioCore::Sink {

/**
 * Who handles an error a backend reports while its open may still be running, as PulseAudio's can:
 * exactly one of the opener, which then drops the backend as a failed open, and the error
 * callback, which then reports a loss of the stream. Each side makes one atomic transition, so an
 * error can never be handled by both or by neither.
 */
class BackendOpenState {
public:
    /// The opener, once the open returned. False if an error came first: drop the backend.
    bool FinishOpen() {
        Phase expected = Phase::Opening;
        return phase.compare_exchange_strong(expected, Phase::Open, std::memory_order_acq_rel);
    }

    /// The error callback. True if the open had finished: report a loss of the stream. Every
    /// error of an open that failed, however many, is the opener's.
    bool ReportError() {
        Phase current = phase.load(std::memory_order_acquire);
        while (true) {
            const Phase next = current == Phase::Opening ? Phase::FailedOpening
                               : current == Phase::FailedOpening ? Phase::FailedOpening
                                                                  : Phase::Lost;
            if (phase.compare_exchange_weak(current, next, std::memory_order_acq_rel)) {
                return next == Phase::Lost;
            }
        }
    }

    /// Reported an error after its open finished.
    bool Lost() const {
        return phase.load(std::memory_order_acquire) == Phase::Lost;
    }

private:
    enum class Phase : u8 { Opening, Open, FailedOpening, Lost };
    std::atomic<Phase> phase{Phase::Opening};
};

/**
 * A sink stream whose backend can lose its device, such as a headset behind a switch that went to
 * another computer while Windows had no default device to move the stream to. Backends that give
 * up on a lost device, as cubeb does, implement the hooks below.
 *
 * A renderer or audio out stream stands in for the device until a backend opens again: it is
 * played into nothing in real time, as a device's callback would, so the audio renderer stays
 * paced and never waits forever for queue space, which also blocked emulation shutdown. Their
 * owners never check that a stream is ready, so a dead one left the game silent until restarted.
 * The backend is reopened with backoff, on the default device once the configured one has failed a
 * few times, and started again unless the owner paused the stream meanwhile. Each reopen opens on
 * a thread of its own, since opening can block while devices come and go: the stand-in plays on
 * meanwhile, so neither the renderer nor the owner's Start and Stop wait for it. Only closing the
 * stream does, as an open cannot be cancelled.
 *
 * Input and sysmix streams do not stand in: their owners check IsReady and fall back to silence or
 * fail the guest's open, so a loss leaves them not ready, as it always did. A fault the owner
 * reports with MarkFaulted, as sysmix does for a guest fault, is final for every stream.
 *
 * Only one backend plays at a time: the lost one is destroyed, which stops its callbacks, before
 * the stand-in plays a block or a new one opens, and a new one consumes the stream only once it is
 * published (IsPublished), whatever its callbacks do before. So a loss reported while a reopen is
 * under way comes from the new backend, which is then dropped instead of published.
 *
 * The derived constructor calls OpenInitialBackend last, and the derived destructor must call
 * Finalize first: the worker calls the hooks, so it is joined there, while the derived stream is
 * still whole. The base destructor asserts that it was.
 */
class StandbySinkStream : public SinkStream {
public:
    struct Timing {
        std::chrono::milliseconds first_retry{500};
        std::chrono::milliseconds max_retry{4000};
    };

    /// Reopens on a configured device before the backend should try the default one.
    static constexpr u32 ConfiguredDeviceAttempts = 3;

    enum class DeviceChoice {
        Wait,                  ///< Open nothing: the configured device may come back.
        Configured,            ///< Only the configured device.
        ConfiguredThenDefault, ///< The configured device, or the default one if it does not open.
        Default,               ///< Only the default device.
    };
    /**
     * Which device a backend with a configured device opens. A stream published on the configured
     * device waits ConfiguredDeviceAttempts reopens for it; any other takes the default one when
     * the configured one is not listed or does not open. One that takes the default one also does
     * when the configured one opened but failed to start the last time.
     *
     * @param attempt                 - As given to OpenBackend.
     * @param listed                  - The configured device is listed and enabled.
     * @param published_on_configured - The stream's last published backend was on it.
     * @param start_failed            - A backend on it failed to start since that one.
     */
    static DeviceChoice ChooseDevice(u32 attempt, bool listed, bool published_on_configured,
                                     bool start_failed);

    ~StandbySinkStream() override;

    void Start(bool resume = false) override;
    void Stop() override;
    void Finalize() override;
    /// A renderer or audio out stream stays ready while it stands in for a lost device.
    bool IsReady() const override;

protected:
    StandbySinkStream(Core::System& system_, StreamType type_, Timing timing_);
    StandbySinkStream(Core::System& system_, StreamType type_)
        : StandbySinkStream(system_, type_, Timing{}) {}

    /// Open a backend, or return null. Its callbacks may report a loss at any time, but consume
    /// the stream, which the stand-in may be playing, only once IsPublished. attempt is 0 for the
    /// stream's first open, on the constructing thread, then numbers the reopens since the device
    /// was lost from 1, each on a thread of its own. Called without any of the stream's locks,
    /// since it may block.
    virtual void* OpenBackend(u32 attempt) = 0;
    virtual bool StartBackend(void* backend) = 0;
    virtual bool StopBackend(void* backend) = 0;
    virtual void DestroyBackend(void* backend) = 0;
    /// The backend became the stream's, on its first open or a reopen; any other one opened since
    /// was destroyed. Called before the stream opens another one.
    virtual void BackendPublished(void* backend) {}

    /// Open the first backend. Renderer and audio out streams with no device yet stand in.
    void OpenInitialBackend();

    /// The backend lost its device. Takes no lock held around backend calls, so its callbacks can
    /// report it. Ignored once the stream is finalized.
    void BackendLost();

    /// Whether the backend is the stream's. Lock-free, for a backend's callbacks: one that is not,
    /// opening or starting during a reopen while the stand-in plays, must not consume the stream.
    bool IsPublished(const void* backend) const {
        return backend && backend == live_backend.load(std::memory_order_acquire);
    }

    /// For a backend's data callback: play the stream into `output` if the backend is published,
    /// else fill it with silence. Returns whether it played the stream.
    bool RenderFromBackend(const void* backend, std::span<s16> output, std::size_t num_frames);

    /// For tests: standing in for a lost device, and the reopen counts.
    bool IsStandingIn() const;
    u64 ReopenAttempts() const {
        return attempts.load();
    }
    u64 Reopens() const {
        return reopens.load();
    }

private:
    enum class State : u8 {
        Live,      ///< The published backend plays the stream.
        Lost,      ///< No usable backend; a renderer or audio out stream stands in.
        Reopening, ///< A backend is opening, or the worker is starting it. The stand-in plays on
                   ///< while it opens.
        Stopped,   ///< Never opened (no backend context), or finalized.
    };
    enum class Reopen { Opened, Failed, Stopped };

    bool StandsIn() const {
        return type == StreamType::Render || type == StreamType::Out;
    }
    void Run(std::stop_token stop_token);
    /// Play blocks into nothing until `until`, a fault, or `done` when given. Returns false when
    /// stopping. Worker thread only.
    bool StandIn(std::stop_token stop_token, std::chrono::steady_clock::time_point until,
                 const std::atomic<bool>* done = nullptr);
    void ReleaseBackend();
    Reopen TryReopen(std::stop_token stop_token, u32 attempt);
    /// Mark the stream finalized and pause it, so that nothing waits for queue space meanwhile,
    /// then stop and join the worker, which waits for an open under way. Returns the state before.
    State StopWorker();

    Timing timing;

    /// Serializes Start, Stop, Finalize, the worker's release and publication, and each block the
    /// stand-in plays, so that none plays after Stop returns: held around backend calls, except
    /// OpenBackend. Never taken by a backend's callbacks.
    std::mutex control_mutex;
    /// Guards the fields below. Never held around a backend call.
    mutable std::mutex state_mutex;
    std::condition_variable_any wake;
    std::atomic<State> state{State::Stopped};
    /// Atomic for IsPublished; written under state_mutex.
    std::atomic<void*> live_backend{};
    /// Losses reported, so that a reopen can tell its new backend reported one.
    u64 losses{};
    /// The worker starts on the first loss of a stream that stands in.
    std::jthread worker;

    /// Worker only: the block it plays, and when the next one is due.
    std::vector<s16> stand_in_block;
    std::chrono::steady_clock::time_point next_block{};

    std::atomic<u64> attempts{};
    std::atomic<u64> reopens{};
};

} // namespace AudioCore::Sink
