// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "audio_core/common/common.h"
#include "audio_core/sink/cubeb_sink.h"
#include "audio_core/sink/sink_stream.h"
#include "audio_core/sink/stream_standby.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "core/core.h"

#ifdef _WIN32
#include <objbase.h>
#undef CreateEvent
#endif

namespace AudioCore::Sink {
/**
 * Cubeb sink stream, responsible for sinking samples to hardware. Stands in for its device when
 * cubeb gives up on it (see StandbySinkStream).
 */
class CubebSinkStream final : public StandbySinkStream {
public:
    /**
     * Create a new sink stream.
     *
     * @param ctx_             - Cubeb context to create this stream with.
     * @param device_channels_ - Number of channels supported by the hardware.
     * @param system_channels_ - Number of channels the audio systems expect.
     * @param device_name_     - Friendly name of the configured device, empty for the default.
     * @param name_            - Name of this stream.
     * @param type_            - Type of this stream.
     * @param system_          - Core system.
     */
    CubebSinkStream(cubeb* ctx_, u32 device_channels_, u32 system_channels_,
                    std::string device_name_, const std::string& name_, StreamType type_,
                    Core::System& system_)
        : StandbySinkStream(system_, type_), ctx{ctx_}, device_name{std::move(device_name_)} {
#ifdef _WIN32
        com_init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
        name = name_;
        device_channels = device_channels_;
        system_channels = system_channels_;

        if (!ctx) {
            return;
        }

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
            LOG_CRITICAL(Audio_Sink, "Error getting minimum latency, error: {}", latency_error);
            minimum_latency = TargetSampleCount * 2;
        }

        latency = (std::max)(minimum_latency, TargetSampleCount * 2);

        LOG_INFO(Service_Audio,
                 "Opening cubeb stream {} type {} with: rate {} channels {} (system channels {}) "
                 "latency {}",
                 name, type, params.rate, params.channels, system_channels, latency);

        OpenInitialBackend();
    }

    /**
     * Destroy the sink stream.
     */
    ~CubebSinkStream() override {
        LOG_DEBUG(Service_Audio, "Destructing cubeb stream {}", name);

        // Joins the standby worker, which calls the hooks below.
        Finalize();

#ifdef _WIN32
        if (SUCCEEDED(com_init_result)) {
            CoUninitialize();
        }
#endif
    }

private:
    /// A cubeb stream and what the stream knows of it. The callbacks' user data, so that they know
    /// which backend calls.
    struct Backend {
        CubebSinkStream* owner{};
        cubeb_stream* stream{};
        bool on_configured{};
        /// Set by BackendPublished, under the stream's control lock.
        bool published{};
        /// An error before its init returned is the init's, not a loss of the stream.
        BackendOpenState open_state;
    };

    /// Open on the configured device or the default one, as ChooseDevice says. The device is
    /// looked up by name at each open, and its id used only while the enumeration that gave it is
    /// alive.
    void* OpenBackend(u32 attempt) override {
        const bool configured = !device_name.empty();
        cubeb_device_collection collection{};
        const bool enumerated =
            configured && cubeb_enumerate_devices(ctx,
                                                  type == StreamType::In ? CUBEB_DEVICE_TYPE_INPUT
                                                                         : CUBEB_DEVICE_TYPE_OUTPUT,
                                                  &collection) == CUBEB_OK;
        SCOPE_EXIT {
            if (enumerated) {
                cubeb_device_collection_destroy(ctx, &collection);
            }
        };
        if (configured && !enumerated && attempt == 0) {
            LOG_WARNING(Audio_Sink, "Audio device enumeration not supported");
        }
        cubeb_devid device{};
        for (size_t i = 0; enumerated && i < collection.count; ++i) {
            const cubeb_device_info& info = collection.device[i];
            if (info.friendly_name && device_name == info.friendly_name &&
                info.state == CUBEB_DEVICE_STATE_ENABLED) {
                device = info.devid;
                break;
            }
        }
        // Not listed also when the enumeration failed while devices come and go.
        const DeviceChoice choice =
            configured ? ChooseDevice(attempt, device != nullptr, published_on_configured,
                                      configured_start_failed)
                       : DeviceChoice::Default;
        if (choice == DeviceChoice::Wait) {
            LOG_DEBUG(Audio_Sink, "Cubeb stream {}: {} is not back yet", name, device_name);
            // Gone again: once back it may well start.
            configured_start_failed = false;
            return nullptr;
        }
        if (!device && published_on_configured && attempt == ConfiguredDeviceAttempts + 1) {
            LOG_WARNING(Audio_Sink, "Cubeb stream {}: {} has not come back, trying the default too",
                        name, device_name);
        }
        if (choice == DeviceChoice::Default) {
            if (device) {
                LOG_DEBUG(Audio_Sink, "Cubeb stream {}: {} did not start, trying the default",
                          name, device_name);
            }
            // Taken, so the next attempt tries the configured device again.
            configured_start_failed = false;
            device = nullptr;
        }
        int error{};
        std::unique_ptr<Backend> opened = Init(device, error);
        if (!opened && device && choice == DeviceChoice::ConfiguredThenDefault) {
            // Listed, but it does not open, and nothing waits for it: the default one, then.
            device = nullptr;
            opened = Init(device, error);
        }
        if (!opened) {
            if (attempt == 0) {
                LOG_ERROR(Audio_Sink, "Error initializing cubeb stream {}, error: {}", name, error);
            } else {
                LOG_DEBUG(Audio_Sink, "Cubeb stream {} could not reopen yet, error {}", name,
                          error);
            }
            return nullptr;
        }
        return opened.release();
    }

    /// A backend of its own for each init, so that nothing one that failed reported is charged to
    /// the next.
    std::unique_ptr<Backend> Init(cubeb_devid device, int& error) {
        auto backend = std::make_unique<Backend>();
        backend->owner = this;
        backend->on_configured = device != nullptr;
        if (type == StreamType::In) {
            error = cubeb_stream_init(ctx, &backend->stream, name.c_str(), device, &params, nullptr,
                                      nullptr, latency, &CubebSinkStream::DataCallback,
                                      &CubebSinkStream::StateCallback, backend.get());
        } else {
            error = cubeb_stream_init(ctx, &backend->stream, name.c_str(), nullptr, nullptr, device,
                                      &params, latency, &CubebSinkStream::DataCallback,
                                      &CubebSinkStream::StateCallback, backend.get());
        }
        if (error != CUBEB_OK) {
            return nullptr;
        }
        if (!backend->open_state.FinishOpen()) {
            // It reported an error while it opened, as PulseAudio can: a failed open, and on the
            // configured device a failed start.
            cubeb_stream_stop(backend->stream);
            cubeb_stream_destroy(backend->stream);
            if (backend->on_configured) {
                configured_start_failed = true;
            }
            error = CUBEB_ERROR;
            return nullptr;
        }
        return backend;
    }

    bool StartBackend(void* backend) override {
        const auto* const opened = static_cast<Backend*>(backend);
        if (cubeb_stream_start(opened->stream) == CUBEB_OK) {
            return true;
        }
        // Listed and opening, but not playing: the next open that may take the default does.
        if (opened->on_configured) {
            configured_start_failed = true;
        }
        return false;
    }

    bool StopBackend(void* backend) override {
        return cubeb_stream_stop(static_cast<Backend*>(backend)->stream) == CUBEB_OK;
    }

    /// cubeb requires a stop before the destroy; harmless on a stream not started or lost.
    void DestroyBackend(void* backend) override {
        const auto* const opened = static_cast<Backend*>(backend);
        cubeb_stream_stop(opened->stream);
        cubeb_stream_destroy(opened->stream);
        // Its callbacks are over. One on the configured device that reported an error and was
        // never published counts as a start that failed: the next open that may take the default
        // does.
        if (opened->on_configured && opened->open_state.Lost() && !opened->published) {
            configured_start_failed = true;
        }
        delete opened;
    }

    /// Only a published backend tells which device the stream was on: one that opened but did
    /// not start, or was dropped, does not.
    void BackendPublished(void* backend) override {
        auto* const published = static_cast<Backend*>(backend);
        published->published = true;
        published_on_configured = published->on_configured;
        configured_start_failed = false;
    }

    /**
     * Main callback from Cubeb. Either expects samples from us (audio render/audio out), or will
     * provide samples to be copied (audio in).
     *
     * @param stream      - Cubeb-specific data about the stream.
     * @param user_data   - Custom data pointer passed along, points to the Backend.
     * @param in_buff     - Input buffer to be used if the stream is an input type.
     * @param out_buff    - Output buffer to be used if the stream is an output type.
     * @param num_frames_ - Number of frames of audio in the buffers. Note: Not number of samples.
     */
    static long DataCallback([[maybe_unused]] cubeb_stream* stream, void* user_data,
                             [[maybe_unused]] const void* in_buff, void* out_buff,
                             long num_frames_) {
        const auto* const backend = static_cast<const Backend*>(user_data);
        if (!backend) {
            return -1;
        }
        auto* const impl = backend->owner;

        const std::size_t num_channels = impl->GetDeviceChannels();
        const std::size_t frame_size = num_channels;
        const std::size_t num_frames{static_cast<size_t>(num_frames_)};

        // Only once published, whatever the cubeb backend calls before: the stand-in may be
        // playing the stream, which has one consumer at a time.
        if (impl->type == StreamType::In) {
            if (impl->IsPublished(backend)) {
                std::span<const s16> input_buffer{reinterpret_cast<const s16*>(in_buff),
                                                  num_frames * frame_size};
                impl->ProcessAudioIn(input_buffer, num_frames);
            }
        } else {
            std::span<s16> output_buffer{reinterpret_cast<s16*>(out_buff), num_frames * frame_size};
            impl->RenderFromBackend(backend, output_buffer, num_frames);
        }

        return num_frames_;
    }

    /**
     * Cubeb callback for if a device state changes.
     *
     * An error is cubeb giving up on the device, such as when it was unplugged while the system
     * had no default device to move the stream to. It does not retry, so the stream reopens it,
     * standing in meanwhile if it is a renderer or audio out stream (see StandbySinkStream).
     *
     * @param stream      - Cubeb-specific data about the stream.
     * @param user_data   - Custom data pointer passed along, points to the Backend.
     * @param state       - New state of the device.
     */
    static void StateCallback(cubeb_stream*, void* user_data, cubeb_state state) {
        auto* const backend = static_cast<Backend*>(user_data);
        if (!backend) {
            return;
        }
        auto* const impl = backend->owner;
        if (state == CUBEB_STATE_ERROR) {
            // During its init, the init handles it.
            if (backend->open_state.ReportError()) {
                impl->BackendLost();
            }
            return;
        }
        LOG_DEBUG(Audio_Sink, "Cubeb stream {} state {}", impl->name, static_cast<int>(state));
    }

    /// Main Cubeb context
    cubeb* ctx{};
    /// Friendly name of the configured device; empty follows the system default
    std::string device_name;
    cubeb_stream_params params{};
    u32 latency{};
    /// Whether the last published backend is on the configured device.
    std::atomic<bool> published_on_configured{};
    /// A backend on the configured device failed to start since then. Set wherever the stream
    /// starts, read by the opener.
    std::atomic<bool> configured_start_failed{};
#ifdef _WIN32
    /// Result of this stream's COM initialization, balanced only when it succeeded
    HRESULT com_init_result{};
#endif
};

/// Streams open on the device named in the settings, or on the defaults when unspecified.
CubebSink::CubebSink(std::string_view target_device_name) {
    // Cubeb requires COM to be initialized on the thread calling cubeb_init on Windows
#ifdef _WIN32
    com_init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    if (cubeb_init(&ctx, "Eden", nullptr) != CUBEB_OK) {
        LOG_CRITICAL(Audio_Sink, "cubeb_init failed");
        return;
    }

    // Looked up by name at each stream open: a cubeb device id is only valid with the
    // enumeration that returned it.
    if (target_device_name != auto_device_name && !target_device_name.empty()) {
        device_name = target_device_name;
    }

    cubeb_get_max_channel_count(ctx, &device_channels);
    device_channels = device_channels >= 6U ? 6U : 2U;
}

CubebSink::~CubebSink() {
    if (!ctx) {
        return;
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

/// Open a unidirectional stream on the configured device and with its channel layout.
SinkStream* CubebSink::AcquireSinkStream(Core::System& system, u32 system_channels_,
                                         const std::string& name, StreamType type) {
    system_channels = system_channels_;
    SinkStreamPtr& stream = sink_streams.emplace_back(
        std::make_unique<CubebSinkStream>(ctx, type == StreamType::In ? 2U : device_channels,
                                          system_channels, device_name, name, type, system));
    stream->SetDeviceVolume(device_volume);

    return stream.get();
}

void CubebSink::CloseStream(SinkStream* stream) {
    for (size_t i = 0; i < sink_streams.size(); i++) {
        if (sink_streams[i].get() == stream) {
            sink_streams[i].reset();
            sink_streams.erase(sink_streams.begin() + i);
            break;
        }
    }
}

void CubebSink::CloseStreams() {
    sink_streams.clear();
}

f32 CubebSink::GetDeviceVolume() const {
    return device_volume;
}

void CubebSink::SetDeviceVolume(f32 volume) {
    device_volume = volume;
    for (auto& stream : sink_streams) {
        stream->SetDeviceVolume(volume);
    }
}

void CubebSink::SetSystemVolume(f32 volume) {
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
