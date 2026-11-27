// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

namespace VideoCore {

// One owner per decoder session. The session serializes input, flush and end of stream.
class Nx2MoviePlayback {
public:
    Nx2MoviePlayback() = default;
    Nx2MoviePlayback(const Nx2MoviePlayback&) = delete;
    Nx2MoviePlayback& operator=(const Nx2MoviePlayback&) = delete;

    ~Nx2MoviePlayback() {
        SetActive(false);
    }

    void SetActive(bool playing) {
        if (playing == active) {
            return;
        }
        active = playing;
        if (playing) {
            sessions.fetch_add(1, std::memory_order_relaxed);
        } else {
            sessions.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    static bool AnyActive() {
        return sessions.load(std::memory_order_relaxed) != 0;
    }

private:
    bool active{};
    inline static std::atomic_uint sessions{};
};

} // namespace VideoCore
