// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/arm/dynarmic/dynarmic_code_image.h"

#include <chrono>
#include <fstream>
#include <iterator>
#include <system_error>

#include <fmt/format.h>

#include <dynarmic/interface/A64/code_image.h>
#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/logging.h"

namespace Core {

namespace {

/// How often new blocks are written while the process runs: the file is complete even when the
/// emulator does not shut down cleanly.
constexpr auto flush_interval = std::chrono::seconds(15);

} // namespace

DynarmicCodeImageFile::DynarmicCodeImageFile()
    : m_image{std::make_shared<Dynarmic::A64::CodeImage>(std::vector<u8>{})} {}

DynarmicCodeImageFile::~DynarmicCodeImageFile() {
    if (m_flush_thread.joinable()) {
        m_flush_thread.request_stop();
        m_flush_thread.join();
    }
    Flush();
    const auto stats = m_image->GetStatistics();
    LOG_INFO(
        Core_ARM,
        "Guest code image: {} blocks, {} installed, {} compiled and recorded, {} not recordable, "
        "{} with changed guest code, {} not installable",
        stats.blocks, stats.loaded, stats.recorded, stats.not_recordable, stats.guest_changed,
        stats.not_installable);
}

std::vector<u8> DynarmicCodeImageFile::MakeKey(u64 program_id,
                                               std::span<const std::string> module_build_ids) {
    std::string key = fmt::format("lichii-guest-code;{:016X}", program_id);
    for (const auto& build_id : module_build_ids) {
        key += ';';
        key += build_id;
    }
    return {key.begin(), key.end()};
}

std::filesystem::path DynarmicCodeImageFile::MakeRelativePath(u64 program_id,
                                                              std::span<const u8> main_build_id) {
    return std::filesystem::path{"cpu"} / fmt::format("{:016X}", program_id) /
           (Common::HexToString(main_build_id.first(std::min<size_t>(main_build_id.size(), 20))) +
            ".dci");
}

void DynarmicCodeImageFile::Open(u64 program_id, std::span<const u8> main_build_id,
                                 std::span<const std::string> module_build_ids) {
    m_image->SetFrontendKey(MakeKey(program_id, module_build_ids));
    m_path = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) /
             MakeRelativePath(program_id, main_build_id);

    std::ifstream file{m_path, std::ios::binary};
    if (file) {
        const std::vector<u8> data{std::istreambuf_iterator<char>{file},
                                   std::istreambuf_iterator<char>{}};
        file.close();
        std::size_t whole = 0;
        if (m_image->Deserialize(data, &whole)) {
            LOG_INFO(Core_ARM, "Guest code image {}: {} blocks", m_path.string(),
                     m_image->GetStatistics().blocks);
            // New records are appended after the whole part; a chunk whose write did not finish
            // (the emulator stopped meanwhile) is cut off first.
            std::error_code ec;
            if (whole != data.size()) {
                std::filesystem::resize_file(m_path, whole, ec);
            }
            m_file_valid = !ec;
        } else {
            LOG_INFO(Core_ARM,
                     "Guest code image {} is for another build or damaged: starting a new one",
                     m_path.string());
        }
    }
    m_flush_thread = std::jthread([this](std::stop_token stop) { FlushLoop(stop); });
}

void DynarmicCodeImageFile::FlushLoop(std::stop_token stop) {
    std::unique_lock lock{m_flush_mutex};
    while (!stop.stop_requested()) {
        m_flush_cv.wait_for(lock, stop, flush_interval, [] { return false; });
        if (!stop.stop_requested()) {
            lock.unlock();
            Flush();
            lock.lock();
        }
    }
}

void DynarmicCodeImageFile::Flush() {
    if (m_path.empty() || !m_image->Dirty()) {
        return;
    }
    if (m_file_valid) {
        // Only the records added since the last write, as a chunk appended to the file.
        const std::vector<u8> chunk = m_image->SerializeAdded();
        std::ofstream file{m_path, std::ios::binary | std::ios::app};
        file.write(reinterpret_cast<const char*>(chunk.data()), std::streamsize(chunk.size()));
        if (!file) {
            LOG_WARNING(Core_ARM, "Could not append to guest code image {}", m_path.string());
            m_file_valid = false; // rewritten whole next time
        }
        return;
    }
    // A new file (or one for another build): the whole image, written next to the file and
    // renamed over it.
    const std::vector<u8> data = m_image->Serialize();
    std::error_code ec;
    std::filesystem::create_directories(m_path.parent_path(), ec);
    auto temporary = m_path;
    temporary += ".tmp";
    {
        std::ofstream file{temporary, std::ios::binary | std::ios::trunc};
        file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!file) {
            LOG_WARNING(Core_ARM, "Could not write guest code image {}", temporary.string());
            return;
        }
    }
    std::filesystem::rename(temporary, m_path, ec);
    if (ec) {
        LOG_WARNING(Core_ARM, "Could not replace guest code image {}: {}", m_path.string(),
                    ec.message());
        return;
    }
    m_file_valid = true;
}

} // namespace Core
