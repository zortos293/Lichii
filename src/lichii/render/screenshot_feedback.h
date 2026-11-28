// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>
#include <QString>
#include <QWidget>

class QTimer;
class QVariantAnimation;
struct SDL_AudioStream;

/// A click-through window over the game that flashes and plays a shutter sound when a screenshot
/// is taken and then shows whether it was saved, like the console does.
class ScreenshotFeedback : public QWidget {
    Q_OBJECT

public:
    explicit ScreenshotFeedback(QWidget* parent = nullptr);
    ~ScreenshotFeedback() override;

    /// Flashes `area` (global coordinates, the game's picture).
    void Flash(const QRect& area);

    /// Plays a camera shutter sound at `gain` (1 is full scale; 0 plays nothing).
    void PlayShutter(float gain);

    /// Shows `text` in the corner of `area` for a moment.
    void ShowMessage(const QRect& area, const QString& text);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void Cover(const QRect& area);
    void HideWhenDone();

    void CloseAudio();

    QVariantAnimation* flash_animation;
    QVariantAnimation* message_animation;
    qreal flash_opacity = 0.0;
    qreal message_opacity = 0.0;
    QString message;

    /// Open while a shutter sound plays, and briefly after
    SDL_AudioStream* audio_stream = nullptr;
    /// The output device setting the stream was opened for
    std::string audio_device_name;
    QTimer* audio_close_timer;
    std::vector<float> shutter_sound;
};
