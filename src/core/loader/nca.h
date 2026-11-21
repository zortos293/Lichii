// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <functional>
#include <span>

#include "common/common_types.h"
#include "core/loader/loader.h"

namespace Core {
class System;
}

namespace FileSys {
class NACP;
class NCA;
}

namespace Loader {

class AppLoader_DeconstructedRomDirectory;

/// How a verification read waits on a file that returns no data before its end.
struct VerifyReadPolicy {
    /// The check fails after this long without a byte.
    std::chrono::milliseconds stall_limit;
    /// The wait between tries meanwhile; the progress function is called after each.
    std::chrono::milliseconds poll_interval;
};

/// The policy NCA verification uses (nca.cpp says why).
extern const VerifyReadPolicy DefaultVerifyReadPolicy;

/**
 * Reads `file` from start to end in pieces of up to `buffer`'s size, handing each to `consume`,
 * and calls `progress` (which may be empty) with the bytes read so far and the file's size. A read
 * that returns nothing is tried again, as `policy` says, until data comes or the stall limit
 * passes. Fails if `consume` or `progress` returns false, or the stall limit passes.
 */
ResultStatus ReadForVerification(const FileSys::VirtualFile& file, std::span<u8> buffer,
                                 const std::function<bool(std::span<const u8>)>& consume,
                                 const std::function<bool(size_t, size_t)>& progress,
                                 const VerifyReadPolicy& policy);

/// Loads an NCA file
class AppLoader_NCA final : public AppLoader {
public:
    explicit AppLoader_NCA(FileSys::VirtualFile file_, u64 update_only_program_id = 0);
    ~AppLoader_NCA() override;

    /**
     * Identifies whether or not the given file is an NCA file.
     *
     * @param nca_file The file to identify.
     *
     * @return FileType::NCA, or FileType::Error if the file is not an NCA file.
     */
    static FileType IdentifyType(const FileSys::VirtualFile& nca_file);

    FileType GetFileType() const override {
        return IdentifyType(file);
    }

    LoadResult Load(Kernel::KProcess& process, Core::System& system) override;

    ResultStatus VerifyIntegrity(std::function<bool(size_t, size_t)> progress_callback) override;

    ResultStatus ReadRomFS(FileSys::VirtualFile& dir) override;
    ResultStatus ReadProgramId(u64& out_program_id) override;
    ResultStatus ReadControlData(FileSys::NACP& out_control) override;
    ResultStatus ReadTitle(std::string& out_title) override;

    ResultStatus ReadBanner(std::vector<u8>& buffer) override;
    ResultStatus ReadLogo(std::vector<u8>& buffer) override;

    ResultStatus ReadNSOModules(Modules& modules) override;
    ResultStatus ReadMainBuildId(Core::System& system, std::array<u8, 0x20>& build_id) override;

    u64 GetProgramId() const;

private:
    FileSys::VirtualDir ResolveExeFS(Core::System& system) const;

    std::unique_ptr<FileSys::NCA> nca;
    u64 update_only_program_id{};
    std::unique_ptr<FileSys::NACP> nacp;
    std::unique_ptr<AppLoader_DeconstructedRomDirectory> directory_loader;
};

} // namespace Loader
