// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/alignment.h"
#include "common/common_funcs.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/random.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_memory_manager.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/nro.h"
#include "core/memory.h"

#ifdef HAS_NCE
#include "core/arm/nce/patcher.h"
#include "core/device_memory.h"
#endif

namespace Loader {

struct NroSegmentHeader {
    u32_le offset;
    u32_le size;
};
static_assert(sizeof(NroSegmentHeader) == 0x8, "NroSegmentHeader has incorrect size.");

struct NroHeader {
    INSERT_PADDING_BYTES(0x4);
    u32_le module_header_offset;
    u32 magic_ext1;
    u32 magic_ext2;
    u32_le magic;
    INSERT_PADDING_BYTES(0x4);
    u32_le file_size;
    INSERT_PADDING_BYTES(0x4);
    std::array<NroSegmentHeader, 3> segments; // Text, RoData, Data (in that order)
    u32_le bss_size;
    INSERT_PADDING_BYTES(0x44);
};
static_assert(sizeof(NroHeader) == 0x80, "NroHeader has incorrect size.");

struct ModHeader {
    u32_le magic;
    u32_le dynamic_offset;
    u32_le bss_start_offset;
    u32_le bss_end_offset;
    u32_le unwind_start_offset;
    u32_le unwind_end_offset;
    u32_le module_offset; // Offset to runtime-generated module object. typically equal to .bss base
};
static_assert(sizeof(ModHeader) == 0x1c, "ModHeader has incorrect size.");

struct AssetSection {
    u64_le offset;
    u64_le size;
};
static_assert(sizeof(AssetSection) == 0x10, "AssetSection has incorrect size.");

struct AssetHeader {
    u32_le magic;
    u32_le format_version;
    AssetSection icon;
    AssetSection nacp;
    AssetSection romfs;
};
static_assert(sizeof(AssetHeader) == 0x38, "AssetHeader has incorrect size.");

AppLoader_NRO::AppLoader_NRO(FileSys::VirtualFile file_) : AppLoader(std::move(file_)) {
    NroHeader nro_header{};
    if (file->ReadObject(&nro_header) != sizeof(NroHeader)) {
        return;
    }

    if (file->GetSize() >= nro_header.file_size + sizeof(AssetHeader)) {
        const u64 offset = nro_header.file_size;
        AssetHeader asset_header{};
        if (file->ReadObject(&asset_header, offset) != sizeof(AssetHeader)) {
            return;
        }

        if (asset_header.format_version != 0) {
            LOG_WARNING(Loader,
                        "NRO Asset Header has format {}, currently supported format is 0. If "
                        "strange glitches occur with metadata, check NRO assets.",
                        asset_header.format_version);
        }

        if (asset_header.magic != Common::MakeMagic('A', 'S', 'E', 'T')) {
            return;
        }

        if (asset_header.nacp.size > 0) {
            nacp = std::make_unique<FileSys::NACP>(std::make_shared<FileSys::OffsetVfsFile>(
                file, asset_header.nacp.size, offset + asset_header.nacp.offset, "Control.nacp"));
        }

        if (asset_header.romfs.size > 0) {
            romfs = std::make_shared<FileSys::OffsetVfsFile>(
                file, asset_header.romfs.size, offset + asset_header.romfs.offset, "game.romfs");
        }

        if (asset_header.icon.size > 0) {
            icon_data = file->ReadBytes(asset_header.icon.size, offset + asset_header.icon.offset);
        }
    }
}

AppLoader_NRO::~AppLoader_NRO() = default;

FileType AppLoader_NRO::IdentifyType(const FileSys::VirtualFile& nro_file) {
    // Read NSO header
    NroHeader nro_header{};
    if (sizeof(NroHeader) != nro_file->ReadObject(&nro_header)) {
        return FileType::Error;
    }
    if (nro_header.magic == Common::MakeMagic('N', 'R', 'O', '0')) {
        return FileType::NRO;
    }
    return FileType::Error;
}

bool AppLoader_NRO::IsHomebrew() {
    // Read NSO header
    NroHeader nro_header{};
    if (sizeof(NroHeader) != file->ReadObject(&nro_header)) {
        return false;
    }
    return nro_header.magic_ext1 == Common::MakeMagic('H', 'O', 'M', 'E') &&
           nro_header.magic_ext2 == Common::MakeMagic('B', 'R', 'E', 'W');
}

std::optional<NroLayout> ValidateNroLayout(std::span<const u8> data, std::string_view* reason) {
    constexpr u64 PageSize = Core::Memory::LICHII_PAGESIZE;
    const auto reject = [reason](std::string_view why) -> std::optional<NroLayout> {
        if (reason != nullptr) {
            *reason = why;
        }
        return std::nullopt;
    };
    NroHeader header{};
    if (data.size() < sizeof(NroHeader)) {
        return reject("the file is shorter than an NRO header");
    }
    std::memcpy(&header, data.data(), sizeof(NroHeader));
    if (header.magic != Common::MakeMagic('N', 'R', 'O', '0')) {
        return reject("the file is not an NRO");
    }

    // The loader used to size the image from the header's 32-bit fields and copy and read at the
    // offsets they give without checking them, so a damaged or crafted .nro could make it read
    // outside its buffers or allocate gigabytes when launched. Every sum here is 64-bit, so none
    // wraps.
    //
    // As hbloader, which launches homebrew on the console, does: the file must hold the size its
    // header gives (the header included), and each segment must lie inside that size.
    const u64 file_size = header.file_size;
    if (file_size < sizeof(NroHeader)) {
        return reject("its size is smaller than its header");
    }
    if (file_size > data.size()) {
        return reject("its size is larger than the file");
    }
    // As Atmosphere's ro (ValidateNro) requires of an NRO: .text at 0, then .rodata and .data in
    // order. ro also wants them back to back with page aligned sizes; here each segment is
    // rounded up to a page, as this loader always did, and must end before the next begins, so
    // the protections it gets do not overlap. An empty segment is left out of those rules, as the
    // loader always took one: it gets an empty extent.
    NroLayout layout{};
    u64 previous_end = 0;
    for (std::size_t i = 0; i < header.segments.size(); ++i) {
        const u64 offset = header.segments[i].offset;
        const u64 size = header.segments[i].size;
        if (size == 0) {
            layout.segments[i] = {offset, 0};
            continue;
        }
        if (i == 0 && offset != 0) {
            return reject(".text does not start at 0");
        }
        if (offset % PageSize != 0) {
            return reject("a segment does not start on a page");
        }
        if (offset < previous_end) {
            return reject("a segment starts before the one ahead of it ends");
        }
        if (offset + size > file_size) {
            return reject("a segment ends past the NRO's size");
        }
        layout.segments[i] = {offset, Common::AlignUp(size, PageSize)};
        previous_end = offset + layout.segments[i].size;
    }

    const u64 file_image_size = Common::AlignUp(file_size, PageSize);
    u64 bss_size = Common::AlignUp(u64{header.bss_size}, PageSize);
    // The module header gives the size of .bss when there is one. hbloader never reads it and
    // libnx finds its own, so one that cannot be used is passed over, not refused: .bss then
    // comes from the NRO's header, as when there is none.
    const u64 mod_offset = header.module_header_offset;
    if (mod_offset + sizeof(ModHeader) > file_size) {
        layout.module_header_unused = "its offset is outside the file";
    } else {
        ModHeader mod_header{};
        std::memcpy(&mod_header, data.data() + mod_offset, sizeof(ModHeader));
        if (mod_header.magic == Common::MakeMagic('M', 'O', 'D', '0')) {
            if (mod_header.bss_end_offset < mod_header.bss_start_offset) {
                layout.module_header_unused = "its .bss ends before it starts";
            } else {
                bss_size = Common::AlignUp(
                    u64{mod_header.bss_end_offset} - mod_header.bss_start_offset, PageSize);
            }
        }
    }

    // A code set segment's size is a u32: real NROs are megabytes.
    const u64 image_size = file_image_size + bss_size;
    if (image_size > std::numeric_limits<u32>::max()) {
        return reject("its image is larger than 4 GiB");
    }
    // .data runs over .bss to the end of the image; an empty .data leaves .bss alone.
    if (layout.segments[2].size != 0) {
        layout.segments[2].size = image_size - layout.segments[2].offset;
    } else {
        layout.segments[2] = {file_image_size, bss_size};
    }
    layout.file_size = file_size;
    layout.image_size = image_size;
    return layout;
}

static bool LoadNroImpl(Core::System& system, Kernel::KProcess& process,
                        const std::vector<u8>& data) {
    std::string_view reason;
    const auto layout = ValidateNroLayout(data, &reason);
    if (!layout) {
        LOG_ERROR(Loader, "NRO not loaded: {}", reason);
        return false;
    }
    if (!layout->module_header_unused.empty()) {
        LOG_WARNING(Loader, "NRO module header not used, {}: .bss size from the NRO header",
                    layout->module_header_unused);
    }

    struct ConfigEntry {
        u32_le key;
        u32_le flags;
        u64_le value[2];
    };
    static_assert(sizeof(ConfigEntry) == 0x18);
    // AArch64 encoding for svc #0x7 (ExitProcess).
    constexpr u32 kSvcExitProcessInstruction = 0xD40000E1;
    constexpr size_t kNumEntries = 4; // MainThreadHandle, AppletType, Argv, EndOfList
    constexpr size_t kConfigTableSize = kNumEntries * sizeof(ConfigEntry);
    // The arguments follow the image, sized to fit them: one copy of the setting, which the UI
    // thread can assign while this runs.
    std::string argv_string;
    const std::string program_args = Settings::values.program_args.GetValue();
    if (!program_args.empty()) {
        argv_string = "homebrew ";
        argv_string += program_args;
        argv_string.push_back('\0');
    }
    const u64 args_offset_in_image = layout->image_size;
    const u64 entries_and_argv =
        argv_string.empty()
            ? 0
            : Common::AlignUp(u64{kConfigTableSize} + argv_string.size(),
                              Core::Memory::LICHII_PAGESIZE);
    const u64 total_size = args_offset_in_image + entries_and_argv;

    // Before anything is allocated: .data, which the arguments extend, fits a u32, and the whole
    // is not more than the memory of the pool the process is created in.
    const auto default_metadata = FileSys::ProgramMetadata::GetDefault();
    const auto pool =
        static_cast<Kernel::KMemoryManager::Pool>(default_metadata.GetPoolPartition());
    if (total_size > std::numeric_limits<u32>::max() ||
        total_size > system.Kernel().MemoryManager().GetSize(pool)) {
        LOG_ERROR(Loader, "NRO not loaded: it needs a {:#x} byte image, which the process cannot hold",
                  total_size);
        return false;
    }

    // The file's part of the image, then .bss and the arguments, zero.
    std::vector<u8> program_image(static_cast<std::size_t>(total_size));
    std::memcpy(program_image.data(), data.data(), static_cast<std::size_t>(layout->file_size));

    Kernel::CodeSet codeset;
    for (std::size_t i = 0; i < layout->segments.size(); ++i) {
        codeset.segments[i].addr = layout->segments[i].offset;
        codeset.segments[i].offset = layout->segments[i].offset;
        codeset.segments[i].size = static_cast<u32>(layout->segments[i].size);
    }
    codeset.DataSegment().size += static_cast<u32>(entries_and_argv);

    std::optional<size_t> exit_process_offset_in_image;
    if (!argv_string.empty()) {
        const auto& code = codeset.CodeSegment();
        const size_t code_end = (std::min)(static_cast<size_t>(layout->image_size),
                                           size_t{code.offset} + code.size);
        for (size_t offset = code.offset; offset + sizeof(u32) <= code_end; offset += sizeof(u32)) {
            u32 instruction{};
            std::memcpy(&instruction, program_image.data() + offset, sizeof(instruction));
            if (instruction == kSvcExitProcessInstruction) {
                exit_process_offset_in_image = offset;
                break;
            }
        }
        if (!exit_process_offset_in_image) {
            LOG_WARNING(Loader,
                        "Unable to find svcExitProcess in NRO; returning from main may fault");
        }
    }
    size_t image_size = program_image.size();

#ifdef HAS_NCE
    const auto& code = codeset.CodeSegment();

    // NROs always have a 39-bit address space.
    const u64 fastmem_base =
        system.IsPoweredOn() ? 0 : system.DeviceMemory().PrepareNativeExecution(true);

    // Create NCE patcher
    Core::NCE::Patcher patch{};

    if (fastmem_base != 0) {
        // Patch SVCs and MRS calls in the guest code
        patch.PatchText(program_image, code);

        // We only support PostData patching for NROs.
        ASSERT(patch.GetPatchMode() == Core::NCE::PatchMode::PostData);

        // Update patch section.
        auto& patch_segment = codeset.PatchSegment();
        patch_segment.addr = image_size;
        patch_segment.size = static_cast<u32>(patch.GetSectionSize());

        // Add patch section size to the module size.
        image_size += patch_segment.size;
    }
#else
    constexpr u64 fastmem_base = 0;
#endif

    // TODO: this is bad form of ASLR, it sucks
    std::uintptr_t aslr_offset = ((::Settings::values.rng_seed_enabled.GetValue()
        ? ::Settings::values.rng_seed.GetValue() : Common::Random::Random64(0)) << 12) & 0xfff000;

    // Setup the process code layout
    if (process
            .LoadFromMetadata(system.Kernel(), default_metadata, image_size, fastmem_base, aslr_offset)
            .IsError()) {
        return false;
    }

    // Relocate code patch and copy to the program_image if running under NCE.
    // This needs to be after LoadFromMetadata so we can use the process entry point.
#ifdef HAS_NCE
    if (fastmem_base != 0) {
        patch.RelocateAndCopy(process.GetEntryPoint(), code, program_image,
                              &process.GetPostHandlers());
    }
#endif

    // Load codeset for current process
    codeset.memory = std::move(program_image);
    process.LoadModule(system.Kernel(), std::move(codeset), process.GetEntryPoint());
    if (!argv_string.empty()) {
        constexpr u32 kEntryEndOfList = 0;
        constexpr u32 kEntryMainThreadHandle = 1;
        constexpr u32 kEntryArgv = 5;
        constexpr u32 kEntryAppletType = 7;
        constexpr u32 kAppletTypeApplication = 0;

        const u64 base = GetInteger(process.GetEntryPoint());
        const u64 config_addr = base + args_offset_in_image;
        const u64 argv_addr = config_addr + kConfigTableSize;

        const ConfigEntry entries[kNumEntries] = {
            {kEntryMainThreadHandle, 0, {0, 0}}, // Value[0] patched in Run()
            {kEntryAppletType,       0, {kAppletTypeApplication, 0}},
            {kEntryArgv,             0, {0, argv_addr}},
            {kEntryEndOfList,        0, {0, 0}},
        };
        process.GetMemory().WriteBlock(Common::ProcessAddress{config_addr}, entries, sizeof(entries));
        process.GetMemory().WriteBlock(Common::ProcessAddress{argv_addr}, argv_string.data(), argv_string.size());
        constexpr size_t kMainThreadHandleValueOffset = offsetof(ConfigEntry, value);
        process.SetArgPointer(Kernel::KProcessAddress{config_addr});
        if (exit_process_offset_in_image) {
            process.SetArgReturnAddress(Kernel::KProcessAddress{base + *exit_process_offset_in_image});
        }
        process.SetMainThreadHandleAddr(Kernel::KProcessAddress{config_addr + kMainThreadHandleValueOffset});
    }

    return true;
}

bool AppLoader_NRO::LoadNro(Core::System& system, Kernel::KProcess& process,
                            const FileSys::VfsFile& nro_file) {
    return LoadNroImpl(system, process, nro_file.ReadAllBytes());
}

AppLoader_NRO::LoadResult AppLoader_NRO::Load(Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }

    if (!LoadNro(system, process, *file)) {
        return {ResultStatus::ErrorLoadingNRO, {}};
    }

    u64 program_id{};
    ReadProgramId(program_id);
    system.GetFileSystemController().RegisterProcess(
        process.GetProcessId(), program_id,
        std::make_unique<FileSys::RomFSFactory>(*this, system.GetContentProvider(),
                                                system.GetFileSystemController()));

    is_loaded = true;
    return {ResultStatus::Success, LoadParameters{Kernel::KThread::DefaultThreadPriority,
                                                  Core::Memory::DEFAULT_STACK_SIZE}};
}

ResultStatus AppLoader_NRO::ReadIcon(std::vector<u8>& buffer) {
    if (icon_data.empty()) {
        return ResultStatus::ErrorNoIcon;
    }

    buffer = icon_data;
    return ResultStatus::Success;
}

ResultStatus AppLoader_NRO::ReadProgramId(u64& out_program_id) {
    if (nacp == nullptr) {
        return ResultStatus::ErrorNoControl;
    }

    out_program_id = nacp->GetTitleId();
    return ResultStatus::Success;
}

ResultStatus AppLoader_NRO::ReadRomFS(FileSys::VirtualFile& dir) {
    if (romfs == nullptr) {
        return ResultStatus::ErrorNoRomFS;
    }

    dir = romfs;
    return ResultStatus::Success;
}

ResultStatus AppLoader_NRO::ReadTitle(std::string& title) {
    if (nacp == nullptr) {
        return ResultStatus::ErrorNoControl;
    }

    title = nacp->GetApplicationName();
    return ResultStatus::Success;
}

ResultStatus AppLoader_NRO::ReadControlData(FileSys::NACP& control) {
    if (nacp == nullptr) {
        return ResultStatus::ErrorNoControl;
    }

    control = *nacp;
    return ResultStatus::Success;
}

bool AppLoader_NRO::IsRomFSUpdatable() const {
    return false;
}

} // namespace Loader
