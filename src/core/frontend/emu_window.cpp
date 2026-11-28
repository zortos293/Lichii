// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include "core/frontend/emu_window.h"

namespace Core::Frontend {
namespace {

std::pair<u32, u32> ClipTouchCoordinates(const Common::Rectangle<u32>& screen, u32 x, u32 y) {
    x = (std::max)(x, screen.left);
    x = (std::min)(x, screen.right - 1);
    y = (std::max)(y, screen.top);
    y = (std::min)(y, screen.bottom - 1);
    return {x, y};
}

} // namespace

EmuWindow::EmuWindow() {
    // TODO: Find a better place to set this.
    config.min_client_area_size =
        std::make_pair(Layout::MinimumSize::Width, Layout::MinimumSize::Height);
    active_config = config;
}

EmuWindow::~EmuWindow() {}

std::pair<f32, f32> EmuWindow::MapToTouchScreen(u32 framebuffer_x, u32 framebuffer_y) const {
    const auto screen = GetFramebufferLayout().screen;
    const auto [clipped_x, clipped_y] = ClipTouchCoordinates(screen, framebuffer_x, framebuffer_y);
    framebuffer_x = clipped_x;
    framebuffer_y = clipped_y;
    const float x =
        static_cast<float>(framebuffer_x - screen.left) /
        static_cast<float>(screen.right - screen.left);
    const float y =
        static_cast<float>(framebuffer_y - screen.top) /
        static_cast<float>(screen.bottom - screen.top);

    return std::make_pair(x, y);
}

std::pair<u32, u32> EmuWindow::ClipToTouchScreen(u32 new_x, u32 new_y) const {
    return ClipTouchCoordinates(GetFramebufferLayout().screen, new_x, new_y);
}

void EmuWindow::UpdateCurrentFramebufferLayout(u32 width, u32 height) {
    NotifyFramebufferLayoutChanged(Layout::DefaultFrameLayout(width, height));
}

void EmuWindow::UpdateCurrentFramebufferLayout() {
    std::scoped_lock lock{framebuffer_layout_mutex};
    framebuffer_layout =
        Layout::DefaultFrameLayout(framebuffer_layout.width, framebuffer_layout.height);
}

} // namespace Core::Frontend
