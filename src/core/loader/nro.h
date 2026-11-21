// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "common/common_types.h"
#include "core/loader/loader.h"

namespace Core {
class System;
}

namespace FileSys {
class NACP;
}

namespace Kernel {
class KProcess;
}

namespace Loader {

/// Where an NRO's segments lie in the image it is loaded as, checked by ValidateNroLayout.
struct NroLayout {
    struct Segment {
        u64 offset;
        u64 size;
    };
    /// .text, .rodata and .data, page aligned and in order; .data runs to the end of the image,
    /// over .bss. An empty segment has size 0; an empty .data's segment is .bss alone.
    std::array<Segment, 3> segments;
    /// How many bytes of the file the image holds: the NRO's size from its header.
    u64 file_size;
    /// The image: the file's part page aligned, then .bss. At most 4 GiB.
    u64 image_size;
    /// Why the module header was not used for the size of .bss, if it was not; empty otherwise.
    std::string_view module_header_unused;
};

/// Checks the header of the NRO in `data` (the whole file as read) against what the file holds
/// and what an image can describe, before anything is allocated. Returns no layout for a damaged
/// NRO, and sets `reason`, if given, to the rule it breaks. Logs nothing.
std::optional<NroLayout> ValidateNroLayout(std::span<const u8> data,
                                           std::string_view* reason = nullptr);

/// Loads an NRO file
class AppLoader_NRO final : public AppLoader {
public:
    explicit AppLoader_NRO(FileSys::VirtualFile file_);
    ~AppLoader_NRO() override;

    /**
     * Identifies whether or not the given file is an NRO file.
     *
     * @param nro_file The file to identify.
     *
     * @return FileType::NRO, or FileType::Error if the file is not an NRO file.
     */
    static FileType IdentifyType(const FileSys::VirtualFile& nro_file);

    bool IsHomebrew();

    FileType GetFileType() const override {
        return IdentifyType(file);
    }

    LoadResult Load(Kernel::KProcess& process, Core::System& system) override;

    ResultStatus ReadIcon(std::vector<u8>& buffer) override;
    ResultStatus ReadProgramId(u64& out_program_id) override;
    ResultStatus ReadRomFS(FileSys::VirtualFile& dir) override;
    ResultStatus ReadTitle(std::string& title) override;
    ResultStatus ReadControlData(FileSys::NACP& control) override;
    bool IsRomFSUpdatable() const override;

private:
    bool LoadNro(Core::System& system, Kernel::KProcess& process, const FileSys::VfsFile& nro_file);

    std::vector<u8> icon_data;
    std::unique_ptr<FileSys::NACP> nacp;
    FileSys::VirtualFile romfs;
};

} // namespace Loader
