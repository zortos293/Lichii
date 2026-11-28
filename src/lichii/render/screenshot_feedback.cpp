// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVariantAnimation>
#include <SDL3/SDL.h>

#include "audio_core/sink/sdl3_sink.h"
#include "audio_core/sink/sink.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "common/settings.h"
#include "render/screenshot_feedback.h"

namespace {

constexpr int FlashMs = 250;
constexpr qreal FlashOpacity = 0.7;
// The message holds for most of its time and fades out at the end.
constexpr int MessageMs = 2200;
constexpr qreal MessageFadeStart = 0.75;

constexpr int SampleRate = 48000;
// The audio device is closed this long after the last shutter sound starts.
constexpr int AudioCloseMs = 1000;

/// A camera shutter: two short clicks, the mirror going up and coming back down, each a burst of
/// noise over a low thump. Generated, so no sound file is shipped.
std::vector<float> MakeShutterSound() {
    std::vector<float> samples(SampleRate * 12 / 100);
    u32 seed = 0x2468ACE1;
    const auto click = [&](float start, float strength) {
        const auto first = static_cast<size_t>(start * SampleRate);
        for (size_t i = first; i < samples.size(); ++i) {
            const float t = static_cast<float>(i - first) / SampleRate;
            seed = seed * 1664525 + 1013904223;
            const float noise = static_cast<float>(seed >> 8) / static_cast<float>(1 << 23) - 1.0f;
            const float thump = std::sin(2.0f * std::numbers::pi_v<float> * 180.0f * t);
            samples[i] += strength * (noise * std::exp(-t / 0.006f) +
                                      0.5f * thump * std::exp(-t / 0.015f));
        }
    };
    click(0.0f, 1.0f);
    click(0.055f, 0.6f);
    for (float& sample : samples) {
        sample *= 0.35f;
    }
    return samples;
}

} // Anonymous namespace

ScreenshotFeedback::ScreenshotFeedback(QWidget* parent)
    : QWidget(parent), flash_animation{new QVariantAnimation(this)},
      message_animation{new QVariantAnimation(this)}, audio_close_timer{new QTimer(this)} {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint |
                   Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_ShowWithoutActivating);

    flash_animation->setDuration(FlashMs);
    flash_animation->setStartValue(FlashOpacity);
    flash_animation->setEndValue(0.0);
    flash_animation->setEasingCurve(QEasingCurve::OutQuad);
    connect(flash_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        flash_opacity = value.toReal();
        update();
    });
    connect(flash_animation, &QVariantAnimation::finished, this,
            &ScreenshotFeedback::HideWhenDone);

    message_animation->setDuration(MessageMs);
    message_animation->setStartValue(0.0);
    message_animation->setEndValue(1.0);
    connect(message_animation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                const qreal progress = value.toReal();
                message_opacity = progress < MessageFadeStart
                                      ? 1.0
                                      : (1.0 - progress) / (1.0 - MessageFadeStart);
                update();
            });
    connect(message_animation, &QVariantAnimation::finished, this,
            &ScreenshotFeedback::HideWhenDone);

    audio_close_timer->setSingleShot(true);
    audio_close_timer->setInterval(AudioCloseMs);
    connect(audio_close_timer, &QTimer::timeout, this, &ScreenshotFeedback::CloseAudio);
}

ScreenshotFeedback::~ScreenshotFeedback() {
    CloseAudio();
}

void ScreenshotFeedback::PlayShutter(float gain) {
    if (gain <= 0.0f) {
        return;
    }
    // The output device the game's sound goes to. A stream still open on another device (the
    // setting changed within a second of the last shot) is reopened.
    const std::string device = Settings::values.audio_output_device_id.GetValue();
    if (audio_stream != nullptr && device != audio_device_name) {
        CloseAudio();
    }
    if (audio_stream == nullptr) {
        // SDL audio stays initialized once it is: the SDL audio backend may share it.
        if (!SDL_WasInit(SDL_INIT_AUDIO)) {
            // The name the SDL backend gives Lichii's audio (sdl3_sink.cpp).
            SDL_SetHint("SDL_AUDIO_DEVICE_APP_NAME", "Lichii Latency Getter");
            if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
                LOG_WARNING(Frontend, "Screenshot sound: SDL audio failed: {}", SDL_GetError());
                return;
            }
        }
        // Found as the SDL backend finds it.
        const SDL_AudioDeviceID playback =
            device.empty() || device == AudioCore::Sink::auto_device_name
                ? SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK
                : AudioCore::Sink::FindAudioDeviceByName(device, false);
        const SDL_AudioSpec spec{SDL_AUDIO_F32, 1, SampleRate};
        audio_stream = SDL_OpenAudioDeviceStream(playback, &spec, nullptr, nullptr);
        if (audio_stream == nullptr) {
            LOG_WARNING(Frontend, "Screenshot sound: no playback device: {}", SDL_GetError());
            return;
        }
        audio_device_name = device;
        SDL_ResumeAudioStreamDevice(audio_stream);
        LOG_DEBUG(Frontend, "Screenshot sound: playing on {}",
                  playback == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ? "the default device" : device);
        if (shutter_sound.empty()) {
            shutter_sound = MakeShutterSound();
        }
    }

    // A shot taken while the last one's sound plays starts it over.
    SDL_ClearAudioStream(audio_stream);
    std::vector<float> samples = shutter_sound;
    for (float& sample : samples) {
        sample = std::clamp(sample * gain, -1.0f, 1.0f);
    }
    if (!SDL_PutAudioStreamData(audio_stream, samples.data(),
                                static_cast<int>(samples.size() * sizeof(float)))) {
        LOG_WARNING(Frontend, "Screenshot sound: {}", SDL_GetError());
    } else {
        LOG_DEBUG(Frontend, "Screenshot sound: {} samples queued",
                  SDL_GetAudioStreamQueued(audio_stream) / static_cast<int>(sizeof(float)));
    }
    audio_close_timer->start();
}

void ScreenshotFeedback::CloseAudio() {
    if (audio_stream != nullptr) {
        SDL_DestroyAudioStream(audio_stream);
        audio_stream = nullptr;
    }
}

void ScreenshotFeedback::Flash(const QRect& area) {
    Cover(area);
    flash_opacity = FlashOpacity;
    flash_animation->stop();
    flash_animation->start();
}

void ScreenshotFeedback::ShowMessage(const QRect& area, const QString& text) {
    Cover(area);
    message = text;
    message_opacity = 1.0;
    message_animation->stop();
    message_animation->start();
}

void ScreenshotFeedback::Cover(const QRect& area) {
    setGeometry(area);
    show();
    raise();
}

void ScreenshotFeedback::HideWhenDone() {
    if (flash_animation->state() != QAbstractAnimation::Running &&
        message_animation->state() != QAbstractAnimation::Running) {
        flash_opacity = 0.0;
        message_opacity = 0.0;
        hide();
    }
}

void ScreenshotFeedback::paintEvent(QPaintEvent*) {
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing);
    if (flash_opacity > 0.0) {
        painter.fillRect(rect(), QColor(255, 255, 255, static_cast<int>(flash_opacity * 255)));
    }
    if (message_opacity <= 0.0 || message.isEmpty()) {
        return;
    }

    QFont font = painter.font();
    font.setPointSizeF(font.pointSizeF() * 1.15);
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    const QFontMetrics metrics{font};
    const int padding_x = metrics.height();
    const int padding_y = metrics.height() / 2;
    const QSize box_size{metrics.horizontalAdvance(message) + padding_x * 2,
                         metrics.height() + padding_y * 2};
    const int margin = metrics.height();
    const QRect box{QPoint{margin, height() - margin - box_size.height()}, box_size};

    painter.setOpacity(message_opacity);
    QPainterPath path;
    path.addRoundedRect(box, box_size.height() / 2.0, box_size.height() / 2.0);
    painter.fillPath(path, QColor(20, 20, 24, 220));
    painter.setPen(Qt::white);
    painter.drawText(box, Qt::AlignCenter, message);
}
