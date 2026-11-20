// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <mutex>
#include <optional>

#include "common/dynamic_library.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/frame_gen/flyhigh_library.h"

namespace VideoCore::FrameGen {

namespace {

#ifdef _WIN32
constexpr const char* LIBRARY_NAME = "flyhigh.dll";
#elif defined(__APPLE__)
constexpr const char* LIBRARY_NAME = "libflyhigh.dylib";
#else
constexpr const char* LIBRARY_NAME = "libflyhigh.so";
#endif

struct Loaded {
    Common::DynamicLibrary library;
    FlyHighLibrary functions;
};

std::optional<Loaded> Load() {
    const std::filesystem::path path = Common::FS::GetExeDirectory() / LIBRARY_NAME;
    Loaded loaded;
    // Beside the executable first, then the system's search path (a distribution's package).
    if (!loaded.library.Open(Common::FS::PathToUTF8String(path).c_str()) &&
        !loaded.library.Open(LIBRARY_NAME)) {
        LOG_WARNING(Render_Vulkan, "FlyHigh: {} not found beside Lichii", LIBRARY_NAME);
        return std::nullopt;
    }
    bool complete = true;
#define FLYHIGH_SYMBOL(name)                                                                       \
    if (!loaded.library.GetSymbol(#name, &loaded.functions.name)) {                                \
        LOG_WARNING(Render_Vulkan, "FlyHigh: {} has no {}; it is older than this Lichii needs",    \
                    LIBRARY_NAME, #name);                                                          \
        complete = false;                                                                          \
    }
    FLYHIGH_FUNCTIONS(FLYHIGH_SYMBOL)
#undef FLYHIGH_SYMBOL
    if (!complete) {
        return std::nullopt;
    }
    return loaded;
}

} // Anonymous namespace

const FlyHighLibrary* LoadFlyHighLibrary() {
    static std::once_flag once;
    static std::optional<Loaded> loaded;
    std::call_once(once, [] { loaded = Load(); });
    return loaded ? &loaded->functions : nullptr;
}

bool FlyHighLibraryPresent() {
    return LoadFlyHighLibrary() != nullptr;
}

std::filesystem::path FlyHighModelDir() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::FlyHighDir) / "model";
}

std::filesystem::path FlyHighTuneDir() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::FlyHighDir) / "tune";
}

bool FlyHighModelInstalled() {
    const std::filesystem::path dir = FlyHighModelDir();
    for (const FlyHighModel::File& file : FlyHighModel::FILES) {
        const std::filesystem::path path = dir / file.name;
        if (!Common::FS::IsFile(path) || Common::FS::GetSize(path) != file.size) {
            return false;
        }
    }
    return true;
}

} // namespace VideoCore::FrameGen
