// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "common/common_types.h"

namespace Dynarmic::A64 {
class CodeImage;
}

namespace Core {

/// The translated guest code of one application process, kept across runs
/// (Dynarmic::A64::CodeImage): one image shared by the process's cores, read from
/// <cache dir>/cpu/<program id>/<main module build ID>.dci when the process starts and written
/// back while it runs and when it ends, so a second run installs the code earlier runs compiled
/// instead of compiling it on every core again. New records are appended to the file as they
/// come (Dynarmic::A64::CodeImage::SerializeAdded), so a write costs what was compiled since
/// the last one, not the whole image.
class DynarmicCodeImageFile {
public:
    DynarmicCodeImageFile();
    ~DynarmicCodeImageFile();
    DynarmicCodeImageFile(const DynarmicCodeImageFile&) = delete;
    DynarmicCodeImageFile& operator=(const DynarmicCodeImageFile&) = delete;

    /// The image the process's Jits use (UserConfig::code_image).
    const std::shared_ptr<Dynarmic::A64::CodeImage>& Image() const {
        return m_image;
    }

    /// Before the process runs, once its modules are loaded: reads the file for this program and
    /// module set and starts writing new blocks back periodically.
    void Open(u64 program_id, std::span<const u8> main_build_id,
              std::span<const std::string> module_build_ids);

    /// Writes the image when it has new blocks.
    void Flush();

    /// Image key of a program and its modules.
    static std::vector<u8> MakeKey(u64 program_id, std::span<const std::string> module_build_ids);
    /// Image file of a program, named after its main module.
    static std::filesystem::path MakeRelativePath(u64 program_id,
                                                  std::span<const u8> main_build_id);

private:
    void FlushLoop(std::stop_token stop);

    std::shared_ptr<Dynarmic::A64::CodeImage> m_image;
    std::filesystem::path m_path;
    /// The file holds this image's header and whole chunks: new records are appended.
    bool m_file_valid = false;
    std::mutex m_flush_mutex;
    std::condition_variable_any m_flush_cv;
    std::jthread m_flush_thread;
};

} // namespace Core
