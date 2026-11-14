// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace VideoCommon {

// Only the buffer phase of a safe command read may publish private command snapshots.
class CommandReadScope {
public:
    CommandReadScope() : previous{active} {
        active = true;
    }
    ~CommandReadScope() {
        active = previous;
    }

    CommandReadScope(const CommandReadScope&) = delete;
    CommandReadScope& operator=(const CommandReadScope&) = delete;

    static bool IsActive() {
        return active;
    }

private:
    inline static thread_local bool active{};
    bool previous;
};

} // namespace VideoCommon
