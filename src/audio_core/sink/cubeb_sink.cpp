// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

#include "audio_core/common/common.h"
#include "audio_core/sink/cubeb_sink.h"
#include "audio_core/sink/sink_stream.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "core/core.h"

#ifdef _WIN32
#include <objbase.h>
#undef CreateEvent
#endif

namespace AudioCore::Sink {
/**
 * Cubeb sink stream, responsible for sinking samples to hardware.
 */
class CubebSinkStream final : public SinkStream {
public:
    /**
     * Create a new sink stream.
     *
     * @param ctx_             - Cubeb context to create this stream with.
     * @param device_channels_ - Number of channels supported by the hardware.
     * @param system_channels_ - Number of channels the audio systems expect.
     * @param output_device    - Cubeb output device id.
     * @param input_device     - Cubeb input device id.
     * @param name_            - Name of this stream.
     * @param type_            - Type of this stream.
     * @param system_          - Core system.
     * @param event            - Event used only for audio renderer, signalled on buffer consume.
     */
    CubebSinkStream(cubeb* ctx_, u32 device_channels_, u32 system_channels_,
                    cubeb_devid output_device_, cubeb_devid input_device_,
                    const std::string& name_, StreamType type_, Core::System& system_)
        : SinkStream(system_, type_), ctx{ctx_} {
#ifdef _WIN32
        com_init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
        name = name_;
        device_channels = device_channels_;
        system_channels = system_channels_;

        if (!ctx) {
            return;
        }

        output_device = output_device_;
        input_device = input_device_;
        OpenBackend();
    }

    /**
     * Destroy the sink stream.
     */
    ~CubebSinkStream() override {
        LOG_DEBUG(Service_Audio, "Destructing cubeb stream {}", name);

        if (ctx) {
            Finalize();
        }

#ifdef _WIN32
        if (SUCCEEDED(com_init_result)) {
            CoUninitialize();
        }
#endif
    }

    /**
     * Finalize the sink stream.
     */
    void Finalize() override {
        std::scoped_lock lk{backend_mutex};
        finalized = true;
        device_lost = false; // a finalized stream is never reopened, so it is not ready
        if (!stream_backend) {
            return;
        }
        StopLocked();
        backend_open = false;
        cubeb_stream_destroy(stream_backend);
        stream_backend = nullptr;
    }

    /// Callers outside backend_mutex read this while the watchdog replaces the backend: it reads
    /// backend_open and device_lost, published under the mutex, not the pointer.
    /// A stream whose device was lost stays ready while the watchdog reopens it: callers that
    /// treat not-ready as a permanent fault (sysmix Pump and SetEngaged) must not end a stream
    /// the watchdog would bring back. Only a failed first open and a real fault are not ready.
    bool IsReady() const override {
        return (backend_open || device_lost) && !failed;
    }

    /**
     * Start the sink stream.
     *
     * @param resume - Set to true if this is resuming the stream a previously-active stream.
     *                 Default false.
     */
    void Start(bool resume = false) override {
        std::scoped_lock lk{backend_mutex};
        if (failed || !paused) {
            return;
        }
        if (!stream_backend) {
            // Being reopened after a lost device: the watchdog starts it once it is open again.
            if (device_lost) {
                paused = false;
            }
            return;
        }

        last_callback = Now();
        stalled_ticks = 0;
        if (cubeb_stream_start(stream_backend) != CUBEB_OK) {
            // The device may be gone for a moment (the computer woke up, a headset was unplugged):
            // the sink's watchdog opens the stream again.
            device_lost = true;
            LOG_ERROR(Audio_Sink, "Error starting cubeb stream {}, it will be reopened", name);
        }
        paused = false;
    }

    /**
     * Stop the sink stream.
     */
    void Stop() override {
        std::scoped_lock lk{backend_mutex};
        StopLocked();
    }

    /**
     * Called by the sink's watchdog about once a second. When the host device was lost (cubeb
     * reported an error, or a running stream stopped asking for data, as happens after the
     * computer sleeps), the backend stream is closed and opened again on the current device.
     * The guest sees nothing: the stream object, its queue and its state stay.
     */
    void Watch() {
        std::scoped_lock lk{backend_mutex};
        if (!ctx || finalized || failed) {
            return;
        }
        const char* why = nullptr;
        if (device_lost) {
            why = "the device reported an error";
        } else if (stream_backend && !paused) {
            // Two ticks in a row: the first one after a sleep always sees a long gap.
            if (Now() - last_callback > StallMs) {
                if (++stalled_ticks >= 2) {
                    why = "no audio callback for 5 s";
                }
            } else {
                stalled_ticks = 0;
            }
        }
        if (why == nullptr) {
            return;
        }
        const bool running = !paused;
        if (!reopen_logged) {
            LOG_WARNING(Audio_Sink, "cubeb stream {}: {}, reopening it", name, why);
        }
        // device_lost before backend_open goes: IsReady stays true across the swap.
        device_lost = true;
        if (stream_backend) {
            backend_open = false;
            cubeb_stream_stop(stream_backend);
            cubeb_stream_destroy(stream_backend);
            stream_backend = nullptr;
        }
        stalled_ticks = 0;
        if (!OpenBackend()) {
            if (!reopen_logged) {
                LOG_ERROR(Audio_Sink, "cubeb stream {} could not be reopened yet, trying on", name);
            }
            reopen_logged = true;
            return;
        }
        last_callback = Now();
        if (running && cubeb_stream_start(stream_backend) != CUBEB_OK) {
            reopen_logged = true;
            return; // device_lost stays set: tried again at the next tick
        }
        device_lost = false;
        reopen_logged = false;
        LOG_WARNING(Audio_Sink, "cubeb stream {} reopened ({})", name, running ? "running" : "paused");
    }

private:
    static constexpr s64 StallMs = 5000;

    static s64 Now() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void StopLocked() {
        if (paused) {
            return;
        }

        // Recorded even with no backend (a reopen that failed): the watchdog must not start the
        // stream again when it reopens it.
        SignalPause();
        if (stream_backend && cubeb_stream_stop(stream_backend) != CUBEB_OK) {
            device_lost = true;
            LOG_ERROR(Audio_Sink, "Error stopping cubeb stream {}, it will be reopened", name);
        }
    }

    /// Opens the backend stream on the current device. Returns false when it could not.
    bool OpenBackend() {
        cubeb_stream_params params{};
        params.rate = TargetSampleRate;
        params.channels = device_channels;
        params.format = CUBEB_SAMPLE_S16LE;
        params.prefs = CUBEB_STREAM_PREF_NONE;
        switch (params.channels) {
        case 1:
            params.layout = CUBEB_LAYOUT_MONO;
            break;
        case 2:
            params.layout = CUBEB_LAYOUT_STEREO;
            break;
        case 6:
            params.layout = CUBEB_LAYOUT_3F2_LFE;
            break;
        }

        u32 minimum_latency{0};
        const auto latency_error = cubeb_get_min_latency(ctx, &params, &minimum_latency);
        if (latency_error != CUBEB_OK) {
            if (!reopen_logged) {
                LOG_CRITICAL(Audio_Sink, "Error getting minimum latency, error: {}", latency_error);
            }
            minimum_latency = TargetSampleCount * 2;
        }

        minimum_latency = (std::max)(minimum_latency, TargetSampleCount * 2);

        if (!reopen_logged) {
            LOG_INFO(Service_Audio,
                     "Opening cubeb stream {} type {} with: rate {} channels {} (system channels "
                     "{}) latency {}",
                     name, type, params.rate, params.channels, system_channels, minimum_latency);
        }

        auto init_error{0};
        cubeb_stream* opened{};
        if (type == StreamType::In) {
            init_error = cubeb_stream_init(ctx, &opened, name.c_str(), input_device,
                                           &params, output_device, nullptr, minimum_latency,
                                           &CubebSinkStream::DataCallback,
                                           &CubebSinkStream::StateCallback, this);
        } else {
            init_error = cubeb_stream_init(ctx, &opened, name.c_str(), input_device,
                                           nullptr, output_device, &params, minimum_latency,
                                           &CubebSinkStream::DataCallback,
                                           &CubebSinkStream::StateCallback, this);
        }

        if (init_error != CUBEB_OK) {
            if (!reopen_logged) {
                LOG_CRITICAL(Audio_Sink, "Error initializing cubeb stream, error: {}", init_error);
            }
            return false;
        }
        stream_backend = opened;
        backend_open = true;
        return true;
    }

    /**
     * Main callback from Cubeb. Either expects samples from us (audio render/audio out), or will
     * provide samples to be copied (audio in).
     *
     * @param stream      - Cubeb-specific data about the stream.
     * @param user_data   - Custom data pointer passed along, points to a CubebSinkStream.
     * @param in_buff     - Input buffer to be used if the stream is an input type.
     * @param out_buff    - Output buffer to be used if the stream is an output type.
     * @param num_frames_ - Number of frames of audio in the buffers. Note: Not number of samples.
     */
    static long DataCallback([[maybe_unused]] cubeb_stream* stream, void* user_data,
                             [[maybe_unused]] const void* in_buff, void* out_buff,
                             long num_frames_) {
        auto* impl = static_cast<CubebSinkStream*>(user_data);
        if (!impl) {
            return -1;
        }

        impl->last_callback.store(Now(), std::memory_order_relaxed);
        const std::size_t num_channels = impl->GetDeviceChannels();
        const std::size_t frame_size = num_channels;
        const std::size_t num_frames{static_cast<size_t>(num_frames_)};

        if (impl->type == StreamType::In) {
            std::span<const s16> input_buffer{reinterpret_cast<const s16*>(in_buff),
                                              num_frames * frame_size};
            impl->ProcessAudioIn(input_buffer, num_frames);
        } else {
            std::span<s16> output_buffer{reinterpret_cast<s16*>(out_buff), num_frames * frame_size};
            impl->ProcessAudioOutAndRender(output_buffer, num_frames);
        }

        return num_frames_;
    }

    /**
     * Cubeb callback for if a device state changes. An error means the device was lost: the
     * stream cannot be closed from its own callback, so the sink's watchdog reopens it.
     *
     * @param stream      - Cubeb-specific data about the stream.
     * @param user_data   - Custom data pointer passed along, points to a CubebSinkStream.
     * @param state       - New state of the device.
     */
    static void StateCallback(cubeb_stream*, void* user_data, cubeb_state state) {
        if (state == CUBEB_STATE_ERROR) {
            static_cast<CubebSinkStream*>(user_data)->device_lost = true;
        }
    }

    /// Main Cubeb context
    cubeb* ctx{};
    /// Cubeb stream backend
    cubeb_stream* stream_backend{};
    std::atomic<bool> backend_open{}; ///< stream_backend is set; IsReady reads this
    /// The devices the backend stream is opened on (null: the default one)
    cubeb_devid output_device{};
    cubeb_devid input_device{};
    /// Guards stream_backend against the watchdog reopening it
    std::mutex backend_mutex;
    /// The host device was lost; the watchdog reopens the stream
    std::atomic<bool> device_lost{};
    /// When cubeb last asked for (or delivered) data, ms on the steady clock
    std::atomic<s64> last_callback{};
    int stalled_ticks{};
    bool reopen_logged{};
    bool finalized{};
#ifdef _WIN32
    /// Result of this stream's COM initialization, balanced only when it succeeded
    HRESULT com_init_result{};
#endif
};

/// Resolve enabled input and output devices independently, using defaults when unspecified.
CubebSink::CubebSink(std::string_view target_device_name) {
    // Cubeb requires COM to be initialized on the thread calling cubeb_init on Windows
#ifdef _WIN32
    com_init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    if (cubeb_init(&ctx, "Eden", nullptr) != CUBEB_OK) {
        LOG_CRITICAL(Audio_Sink, "cubeb_init failed");
        return;
    }

    if (target_device_name != auto_device_name && !target_device_name.empty()) {
        cubeb_device_collection collection;
        if (cubeb_enumerate_devices(ctx, static_cast<cubeb_device_type>(
                                            CUBEB_DEVICE_TYPE_INPUT | CUBEB_DEVICE_TYPE_OUTPUT),
                                    &collection) != CUBEB_OK) {
            LOG_WARNING(Audio_Sink, "Audio device enumeration not supported");
        } else {
            for (size_t i = 0; i < collection.count; ++i) {
                const auto& device = collection.device[i];
                if (!device.friendly_name || target_device_name != device.friendly_name ||
                    device.state != CUBEB_DEVICE_STATE_ENABLED) {
                    continue;
                }
                if (device.type & CUBEB_DEVICE_TYPE_INPUT) {
                    input_device = device.devid;
                }
                if (device.type & CUBEB_DEVICE_TYPE_OUTPUT) {
                    output_device = device.devid;
                }
            }
            cubeb_device_collection_destroy(ctx, &collection);
        }
    }

    cubeb_get_max_channel_count(ctx, &device_channels);
    device_channels = device_channels >= 6U ? 6U : 2U;

    // Reopens a stream whose host device was lost (the computer slept, the device was unplugged).
    watchdog = std::jthread([this](std::stop_token stop) {
#ifdef _WIN32
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
        while (!stop.stop_requested()) {
            for (int i = 0; i < 10 && !stop.stop_requested(); i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (stop.stop_requested()) {
                break;
            }
            std::scoped_lock lk{streams_mutex};
            for (auto& stream : sink_streams) {
                static_cast<CubebSinkStream*>(stream.get())->Watch();
            }
        }
#ifdef _WIN32
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
#endif
    });
}

CubebSink::~CubebSink() {
    if (!ctx) {
        return;
    }

    watchdog.request_stop();
    if (watchdog.joinable()) {
        watchdog.join();
    }

    for (auto& sink_stream : sink_streams) {
        sink_stream.reset();
    }

    cubeb_destroy(ctx);

#ifdef _WIN32
    if (SUCCEEDED(com_init_result)) {
        CoUninitialize();
    }
#endif
}

/// Open a unidirectional stream with only its matching device ID and channel layout.
SinkStream* CubebSink::AcquireSinkStream(Core::System& system, u32 system_channels_,
                                         const std::string& name, StreamType type) {
    system_channels = system_channels_;
    std::scoped_lock lk{streams_mutex};
    SinkStreamPtr& stream = sink_streams.emplace_back(std::make_unique<CubebSinkStream>(
        ctx, type == StreamType::In ? 2U : device_channels, system_channels,
        type == StreamType::In ? nullptr : output_device,
        type == StreamType::In ? input_device : nullptr, name, type, system));
    stream->SetDeviceVolume(device_volume);

    return stream.get();
}

void CubebSink::CloseStream(SinkStream* stream) {
    std::scoped_lock lk{streams_mutex};
    for (size_t i = 0; i < sink_streams.size(); i++) {
        if (sink_streams[i].get() == stream) {
            sink_streams[i].reset();
            sink_streams.erase(sink_streams.begin() + i);
            break;
        }
    }
}

void CubebSink::CloseStreams() {
    std::scoped_lock lk{streams_mutex};
    sink_streams.clear();
}

f32 CubebSink::GetDeviceVolume() const {
    return device_volume;
}

void CubebSink::SetDeviceVolume(f32 volume) {
    device_volume = volume;
    std::scoped_lock lk{streams_mutex};
    for (auto& stream : sink_streams) {
        stream->SetDeviceVolume(volume);
    }
}

void CubebSink::SetSystemVolume(f32 volume) {
    std::scoped_lock lk{streams_mutex};
    for (auto& stream : sink_streams) {
        stream->SetSystemVolume(volume);
    }
}

std::vector<std::string> ListCubebSinkDevices(bool capture) {
    std::vector<std::string> device_list;
    cubeb* ctx;

#ifdef _WIN32
    auto com_init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    if (cubeb_init(&ctx, "Eden Device Enumerator", nullptr) != CUBEB_OK) {
        LOG_CRITICAL(Audio_Sink, "cubeb_init failed");
        return {};
    }

#ifdef _WIN32
    if (SUCCEEDED(com_init_result)) {
        CoUninitialize();
    }
#endif

    auto type{capture ? CUBEB_DEVICE_TYPE_INPUT : CUBEB_DEVICE_TYPE_OUTPUT};
    cubeb_device_collection collection;
    if (cubeb_enumerate_devices(ctx, type, &collection) != CUBEB_OK) {
        LOG_WARNING(Audio_Sink, "Audio output device enumeration not supported");
    } else {
        for (std::size_t i = 0; i < collection.count; i++) {
            const cubeb_device_info& device = collection.device[i];
            if (device.friendly_name && device.friendly_name[0] != '\0' &&
                device.state == CUBEB_DEVICE_STATE_ENABLED) {
                device_list.emplace_back(device.friendly_name);
            }
        }
        cubeb_device_collection_destroy(ctx, &collection);
    }

    cubeb_destroy(ctx);
    return device_list;
}

/* REVERSION TO 3833 - function GetCubebLatency REINTRODUCED FROM 3833 - DIABLO 3 FIX */
u32 GetCubebLatency() {
    cubeb* ctx;

#ifdef _WIN32
    auto com_init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    // Init cubeb
    if (cubeb_init(&ctx, "Lichii Latency Getter", nullptr) != CUBEB_OK) {
        LOG_CRITICAL(Audio_Sink, "cubeb_init failed");
        // Return a large latency so we choose SDL instead.
        return 10000u;
    }

#ifdef _WIN32
    if (SUCCEEDED(com_init_result)) {
        CoUninitialize();
    }
#endif

    // Get min latency
    cubeb_stream_params params{};
    params.rate = TargetSampleRate;
    params.channels = 2;
    params.format = CUBEB_SAMPLE_S16LE;
    params.prefs = CUBEB_STREAM_PREF_NONE;
    params.layout = CUBEB_LAYOUT_STEREO;

    u32 latency{0};
    const auto latency_error = cubeb_get_min_latency(ctx, &params, &latency);
    if (latency_error != CUBEB_OK) {
        LOG_CRITICAL(Audio_Sink, "Error getting minimum latency, error: {}", latency_error);
        latency = TargetSampleCount * 2;
    }
    latency = (std::max)(latency, TargetSampleCount * 2);
    cubeb_destroy(ctx);
    return latency;
}

// REVERTED back to 3833 - Below namespace section and function IsCubebSuitable() removed, reverting to GetCubebLatency() above. - DIABLO 3 FIX
/*
namespace {
static long TmpDataCallback(cubeb_stream*, void*, const void*, void*, long) {
    return TargetSampleCount;
}
static void TmpStateCallback(cubeb_stream*, void*, cubeb_state) {}
} // namespace

bool IsCubebSuitable() {
#if !defined(HAVE_CUBEB)
    return false;
#else
    cubeb* ctx{nullptr};

#ifdef _WIN32
    auto com_init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    // Init cubeb
    if (cubeb_init(&ctx, "Eden Latency Getter", nullptr) != CUBEB_OK) {
        LOG_ERROR(Audio_Sink, "Cubeb failed to init, it is not suitable.");
        return false;
    }

    SCOPE_EXIT {
        cubeb_destroy(ctx);
    };

#ifdef _WIN32
    if (SUCCEEDED(com_init_result)) {
        CoUninitialize();
    }
#endif

    // Get min latency
    cubeb_stream_params params{};
    params.rate = TargetSampleRate;
    params.channels = 2;
    params.format = CUBEB_SAMPLE_S16LE;
    params.prefs = CUBEB_STREAM_PREF_NONE;
    params.layout = CUBEB_LAYOUT_STEREO;

    u32 latency{0};
    const auto latency_error = cubeb_get_min_latency(ctx, &params, &latency);
    if (latency_error != CUBEB_OK) {
        LOG_ERROR(Audio_Sink, "Cubeb could not get min latency, it is not suitable.");
        return false;
    }
    latency = (std::max)(latency, TargetSampleCount * 2);

    // Test opening a device with standard parameters
    cubeb_devid output_device{0};
    cubeb_devid input_device{0};
    std::string name{"Eden test"};
    cubeb_stream* stream{nullptr};

    if (cubeb_stream_init(ctx, &stream, name.c_str(), input_device, nullptr, output_device, &params,
                          latency, &TmpDataCallback, &TmpStateCallback, nullptr) != CUBEB_OK) {
        LOG_CRITICAL(Audio_Sink, "Cubeb could not open a device, it is not suitable.");
        return false;
    }

    cubeb_stream_stop(stream);
    cubeb_stream_destroy(stream);
    return true;
#endif
}
*/

} // namespace AudioCore::Sink
