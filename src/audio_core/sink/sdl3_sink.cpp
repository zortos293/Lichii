// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <mutex>
#include <span>
#include <vector>

#include <SDL3/SDL.h>

#include "audio_core/common/common.h"
#include "audio_core/sink/sdl3_sink.h"
#include "audio_core/sink/sink_stream.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "core/core.h"

namespace AudioCore::Sink {

namespace {

[[nodiscard]] bool InitializeAudio() {
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        // See https://github.com/PCSX2/pcsx2/pull/12312
        // "SDL and cubeb backends previously resulted in different names for the output which
        // caused them be identified as different applications by the OS."
        //
        // Keep in sync with cubeb_sink.cpp name.
        SDL_SetHint("SDL_AUDIO_DEVICE_APP_NAME", "Lichii Latency Getter");
#ifdef __ANDROID__
        SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "openslES", SDL_HINT_OVERRIDE);
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            LOG_WARNING(Audio_Sink, "OpenSL ES audio initialization failed: {}; retrying default drivers", SDL_GetError());
            SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);
        }
#endif
        if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            LOG_CRITICAL(Audio_Sink, "SDL_InitSubSystem audio failed: {}", SDL_GetError());
            return false;
        }
        LOG_INFO(Audio_Sink, "SDL audio driver: {}", SDL_GetCurrentAudioDriver());
    }
    return true;
}

SDL_AudioDeviceID FindAudioDeviceByName(const std::string& device_name, bool capture) {
    int device_count = 0;
    SDL_AudioDeviceID* devices = capture ? SDL_GetAudioRecordingDevices(&device_count)
                                         : SDL_GetAudioPlaybackDevices(&device_count);
    if (devices == nullptr) {
        return capture ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    }

    SDL_AudioDeviceID selected = capture ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING
                                         : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    for (int i = 0; i < device_count; ++i) {
        const char* current_name = SDL_GetAudioDeviceName(devices[i]);
        if (current_name != nullptr && device_name == current_name) {
            selected = devices[i];
            break;
        }
    }
    SDL_free(devices);
    return selected;
}
} // Anonymous namespace

/**
 * SDL sink stream, responsible for sinking samples to hardware.
 */
class SDLSinkStream final : public SinkStream {
public:
    /**
     * Create a new sink stream.
     *
     * @param device_channels_ - Number of channels supported by the hardware.
     * @param system_channels_ - Number of channels the audio systems expect.
     * @param output_device    - Name of the output device to use for this stream.
     * @param input_device     - Name of the input device to use for this stream.
     * @param type_            - Type of this stream.
     * @param system_          - Core system.
     * @param event            - Event used only for audio renderer, signalled on buffer consume.
     */
    SDLSinkStream(u32 device_channels_, u32 system_channels_, const std::string& output_device,
                  const std::string& input_device, StreamType type_, Core::System& system_)
        : SinkStream{system_, type_} {
        system_channels = system_channels_;
        device_channels = device_channels_;

        spec.freq = TargetSampleRate;
        spec.channels = static_cast<u8>(device_channels);
        spec.format = SDL_AUDIO_S16;

        configured_device_name = (type == StreamType::In) ? input_device : output_device;

        bound_device = configured_device_name.empty()
                           ? (type == StreamType::In ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING
                                                     : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK)
                           : FindAudioDeviceByName(configured_device_name,
                                                   type == StreamType::In);

        stream = SDL_OpenAudioDeviceStream(bound_device, &spec, &SDLSinkStream::DataCallback,
                                           this);

        // A device the settings name can be absent at boot; note when we settled for less.
        const SDL_AudioDeviceID default_device = (type == StreamType::In)
                                                     ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING
                                                     : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
        on_fallback = !configured_device_name.empty() && bound_device == default_device;

        if (stream == nullptr) {
            // Not final: the ADDED watch below retries when a device shows up (#1448, #1089).
            open_failed = true;
            LOG_CRITICAL(Audio_Sink, "Error opening SDL audio device: {}", SDL_GetError());
        }

        SDL_AddEventWatch(&SDLSinkStream::DeviceEventWatch, this);

        if (stream == nullptr) {
            return;
        }

        SDL_AudioSpec stream_in{};
        SDL_AudioSpec stream_out{};
        static_cast<void>(SDL_GetAudioStreamFormat(stream, &stream_in, &stream_out));

        LOG_INFO(Service_Audio,
                 "Opening SDL stream {} with: rate {} channels {} (system channels {}) "
                 " format {}",
                 static_cast<const void*>(stream), stream_out.freq, stream_out.channels,
                 system_channels, static_cast<int>(stream_out.format));
    }

    /**
     * Destroy the sink stream.
     */
    ~SDLSinkStream() override {
        LOG_DEBUG(Service_Audio, "Destructing SDL stream {}", name);
        SDL_RemoveEventWatch(&SDLSinkStream::DeviceEventWatch, this);
        Finalize();
    }

    /**
     * Finalize the sink stream.
     */
    void Finalize() override {
        std::scoped_lock lock{stream_mutex};
        if (stream == nullptr) {
            return;
        }

        StopLocked();
        SDL_ClearAudioStream(stream);
        SDL_DestroyAudioStream(stream);
        stream = nullptr;
    }

    /**
     * Start the sink stream.
     *
     * @param resume - Set to true if this is resuming the stream a previously-active stream.
     *                 Default false.
     */
    void Start(bool resume = false) override {
        std::scoped_lock lock{stream_mutex};
        if (stream == nullptr || failed || !paused) {
            return;
        }

        if (!SDL_ResumeAudioStreamDevice(stream)) {
            failed = true;
            LOG_CRITICAL(Audio_Sink, "Error starting SDL audio stream: {}", SDL_GetError());
            return;
        }
        paused = false;
    }

    bool IsReady() const override {
        std::scoped_lock lock{stream_mutex};
        return stream != nullptr && !failed;
    }

    /**
     * Queue an audio buffer for output or input, reopening the stream first when the device
     * watch asked for it. This is the steady per-buffer call from the audio systems, so the
     * reopen happens on an emulator thread, not inside SDL's event dispatch.
     *
     * @param buffer  - Audio buffer information to be queued.
     * @param samples - Samples in PCM16 format to be output or input.
     */
    void AppendBuffer(SinkBuffer& buffer, std::span<s16> samples) override {
        if (reopen_requested.exchange(false, std::memory_order_relaxed)) {
            ReopenStream();
        }
        SinkStream::AppendBuffer(buffer, samples);
    }

    /**
     * Stop the sink stream.
     */
    void Stop() override {
        std::scoped_lock lock{stream_mutex};
        StopLocked();
    }

private:
    /**
     * Main callback from SDL. Either expects samples from us (audio render/audio out), or will
     * provide samples to be copied (audio in).
     *
     * @param userdata - Custom data pointer passed along, points to a SDLSinkStream.
     * @param stream   - Buffer of samples to be filled or read.
     * @param len      - Length of the stream in bytes.
     */
    static void DataCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                             int total_amount) {
        auto* impl = static_cast<SDLSinkStream*>(userdata);

        if (!impl) {
            return;
        }

        const std::size_t num_channels = impl->GetDeviceChannels();
        const std::size_t frame_size = num_channels;

        if (impl->type == StreamType::In) {
            const int bytes_available = SDL_GetAudioStreamAvailable(stream);
            if (bytes_available <= 0) {
                return;
            }

            std::vector<s16> input(bytes_available / static_cast<int>(sizeof(s16)));
            const int bytes_read = SDL_GetAudioStreamData(stream, input.data(), bytes_available);
            if (bytes_read <= 0) {
                return;
            }

            const std::size_t num_frames =
                static_cast<std::size_t>(bytes_read) / sizeof(s16) / frame_size;
            std::span<const s16> input_buffer{input.data(),
                                              static_cast<std::size_t>(bytes_read) / sizeof(s16)};
            impl->ProcessAudioIn(input_buffer, num_frames);
        } else {
            if (additional_amount <= 0 && total_amount <= 0) {
                return;
            }

            const int bytes_requested = additional_amount > 0 ? additional_amount : total_amount;
            std::vector<s16> output(bytes_requested / static_cast<int>(sizeof(s16)));
            const std::size_t num_frames =
                static_cast<std::size_t>(bytes_requested) / sizeof(s16) / frame_size;
            std::span<s16> output_buffer{output.data(), output.size()};
            impl->ProcessAudioOutAndRender(output_buffer, num_frames);
            static_cast<void>(SDL_PutAudioStreamData(stream, output.data(), bytes_requested));
        }
    }

    /**
     * SDL event hook. Runs on whatever thread SDL dispatches events from, so it only flags the
     * reopen; AppendBuffer does the work. Removal kills a stream bound to a device the settings
     * name: SDL migrates default-bound streams on its own, but a named one dies with its device
     * (#1448, #1089). Addition retries a stream that never opened, and takes a fallen-back
     * stream back to its named device once that returns.
     *
     * @param userdata - Custom data pointer passed along, points to a SDLSinkStream.
     * @param event    - The SDL event.
     */
    static bool SDLCALL DeviceEventWatch(void* userdata, SDL_Event* event) {
        auto* impl = static_cast<SDLSinkStream*>(userdata);
        if (impl == nullptr) {
            return true;
        }
        const bool capture = impl->type == StreamType::In;
        const SDL_AudioDeviceID default_device =
            capture ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;

        if (event->type == SDL_EVENT_AUDIO_DEVICE_REMOVED) {
            if (event->adevice.recording != capture) {
                return true;
            }
            const SDL_AudioDeviceID bound = impl->bound_device.load(std::memory_order_relaxed);
            if (bound != default_device && event->adevice.which == bound) {
                impl->reopen_requested.store(true, std::memory_order_relaxed);
            }
            return true;
        }

        if (event->type == SDL_EVENT_AUDIO_DEVICE_ADDED) {
            if (event->adevice.recording != capture) {
                return true;
            }
            if (impl->open_failed.load(std::memory_order_relaxed)) {
                impl->reopen_requested.store(true, std::memory_order_relaxed);
                return true;
            }
            if (impl->on_fallback.load(std::memory_order_relaxed)) {
                const char* added = SDL_GetAudioDeviceName(event->adevice.which);
                if (added != nullptr && impl->configured_device_name == added) {
                    impl->reopen_requested.store(true, std::memory_order_relaxed);
                }
            }
        }
        return true;
    }

    /**
     * Reopen the stream on the configured device, or the default when it is absent. Carries the
     * paused state over; the samples queued for the old device are dropped.
     */
    void ReopenStream() {
        std::scoped_lock lock{stream_mutex};
        const bool was_paused = paused.load(std::memory_order_relaxed);

        if (stream != nullptr) {
            SDL_ClearAudioStream(stream);
            SDL_DestroyAudioStream(stream);
            stream = nullptr;
        }

        const bool capture = type == StreamType::In;
        const SDL_AudioDeviceID default_device =
            capture ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
        const SDL_AudioDeviceID device = configured_device_name.empty()
                                             ? default_device
                                             : FindAudioDeviceByName(configured_device_name,
                                                                     capture);

        SDL_AudioStream* new_stream =
            SDL_OpenAudioDeviceStream(device, &spec, &SDLSinkStream::DataCallback, this);
        if (new_stream == nullptr) {
            open_failed = true;
            LOG_CRITICAL(Audio_Sink, "Error reopening SDL audio device: {}", SDL_GetError());
            return;
        }

        bound_device = device;
        on_fallback = !configured_device_name.empty() && device == default_device;
        open_failed = false;
        stream = new_stream;

        LOG_INFO(Service_Audio, "Reopened SDL stream on device {}",
                 static_cast<u32>(device));

        if (!was_paused && !SDL_ResumeAudioStreamDevice(stream)) {
            failed = true;
            LOG_CRITICAL(Audio_Sink, "Error resuming reopened SDL audio stream: {}",
                         SDL_GetError());
        }
    }

    /// Stop with stream_mutex already held.
    void StopLocked() {
        if (stream == nullptr || paused) {
            return;
        }
        SignalPause();
        if (!SDL_PauseAudioStreamDevice(stream)) {
            failed = true;
            LOG_CRITICAL(Audio_Sink, "Error stopping SDL audio stream: {}", SDL_GetError());
        }
    }

    /// Serializes stream access between the audio-tick thread (AppendBuffer and ReopenStream)
    /// and the service threads (Start, Stop, Finalize)
    mutable std::mutex stream_mutex;
    /// SDL stream attached to an opened input/output device
    SDL_AudioStream* stream{};
    /// Spec the stream was opened with, kept for a reopen
    SDL_AudioSpec spec{};
    /// Device name from settings; empty means the default device
    std::string configured_device_name;
    /// Device the stream is bound to; the default-device constant when unnamed
    std::atomic<SDL_AudioDeviceID> bound_device{};
    /// The last open attempt failed; retry when a device is added
    std::atomic<bool> open_failed{};
    /// Bound to the default device although settings name one (it was absent)
    std::atomic<bool> on_fallback{};
    /// Set by the event watch, consumed by AppendBuffer
    std::atomic<bool> reopen_requested{};
};

/// Configure the selected device name for both playback and capture streams.
SDLSink::SDLSink(std::string_view target_device_name) {
    if (InitializeAudio()) {
        if (target_device_name != auto_device_name && !target_device_name.empty()) {
            output_device = target_device_name;
            input_device = target_device_name;
        } else {
            output_device.clear();
        }
        device_channels = 2;
    }
}

SDLSink::~SDLSink() = default;

SinkStream* SDLSink::AcquireSinkStream(Core::System& system, u32 system_channels_,
                                       const std::string&, StreamType type) {
    system_channels = system_channels_;
    SinkStreamPtr& stream = sink_streams.emplace_back(std::make_unique<SDLSinkStream>(
        device_channels, system_channels, output_device, input_device, type, system));
    stream->SetDeviceVolume(device_volume);
    return stream.get();
}

void SDLSink::CloseStream(SinkStream* stream) {
    for (size_t i = 0; i < sink_streams.size(); i++) {
        if (sink_streams[i].get() == stream) {
            sink_streams[i].reset();
            sink_streams.erase(sink_streams.begin() + i);
            break;
        }
    }
}

void SDLSink::CloseStreams() {
    sink_streams.clear();
}

f32 SDLSink::GetDeviceVolume() const {
    return device_volume;
}

void SDLSink::SetDeviceVolume(f32 volume) {
    device_volume = volume;
    for (auto& stream : sink_streams) {
        stream->SetDeviceVolume(volume);
    }
}

void SDLSink::SetSystemVolume(f32 volume) {
    for (auto& stream : sink_streams) {
        stream->SetSystemVolume(volume);
    }
}

std::vector<std::string> ListSDLSinkDevices(bool capture) {
    if (!InitializeAudio())
        return {}; //no devices

    std::vector<std::string> device_list;
    int device_count = 0;
    SDL_AudioDeviceID* devices =
        capture ? SDL_GetAudioRecordingDevices(&device_count)
                : SDL_GetAudioPlaybackDevices(&device_count);
    if (devices == nullptr) {
        return device_list;
    }

    for (int i = 0; i < device_count; ++i) {
        if (const char* name = SDL_GetAudioDeviceName(devices[i])) {
            device_list.emplace_back(name);
        }
    }
    SDL_free(devices);

    return device_list;
}

/* REVERSION to 3833 - function GetSDLLatency() REINTRODUCED FROM 3833 - DIABLO 3 FIX */
u32 GetSDLLatency() {
    return TargetSampleCount * 2;
}

// REVERTED back to 3833 - Below function IsSDLSuitable() removed, reverting to GetSDLLatency() above. - DIABLO 3 FIX
/*
bool IsSDLSuitable() {
#if !defined(HAVE_SDL3)
    return false;
#else
    // Check SDL can init
    if (!InitializeAudio()!
        return false;

    // We can set any latency frequency we want with SDL, so no need to check that.

    // Check we can open a device with standard parameters
    SDL_AudioSpec spec;
    spec.freq = TargetSampleRate;
    spec.channels = 2u;
    spec.format = AUDIO_S16SYS;
    spec.samples = TargetSampleCount * 2;
    spec.callback = nullptr;
    spec.userdata = nullptr;

    SDL_AudioSpec obtained;
    auto device = SDL_OpenAudioDevice(nullptr, false, &spec, &obtained, false);

    if (device == 0) {
        LOG_ERROR(Audio_Sink, "SDL failed to open a device, it is not suitable. Error: {}",
                  SDL_GetError());
        return false;
    }

    SDL_CloseAudioDevice(device);
    return true;
#endif
}
*/

} // namespace AudioCore::Sink
