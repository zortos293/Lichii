// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"

namespace FileSys {

RomFSFactory::RomFSFactory(Loader::AppLoader& app_loader, ContentProvider& provider,
                           Service::FileSystem::FileSystemController& controller)
    : content_provider{provider}, filesystem_controller{controller} {
    // Load the RomFS from the app
    if (app_loader.ReadRomFS(file) != Loader::ResultStatus::Success) {
        LOG_WARNING(Service_FS, "Unable to read base RomFS");
    }

    updatable = app_loader.IsRomFSUpdatable();
}

RomFSFactory::~RomFSFactory() = default;

void RomFSFactory::SetPackedUpdate(VirtualFile update_raw_file) {
    packed_update_raw = std::move(update_raw_file);
}

VirtualFile RomFSFactory::OpenCurrentProcess(u64 current_process_title_id) const {
    const auto type = ContentRecordType::Program;
    if (!updatable) {
        // sergio: a NUP already carries its update merged, but LayeredFS mods
        // (user/load/<title>/<mod>/romfs) still apply; no NCA, so no update step.
        const PatchManager mods{current_process_title_id, filesystem_controller,
                                content_provider};
        return mods.PatchRomFS(nullptr, file, type, nullptr);
    }

    const auto nca = content_provider.GetEntry(current_process_title_id, type);
    const PatchManager patch_manager{current_process_title_id, filesystem_controller,
                                     content_provider};
    return patch_manager.PatchRomFS(nca.get(), file, ContentRecordType::Program, packed_update_raw);
}

VirtualFile RomFSFactory::OpenPatchedRomFS(u64 title_id, ContentRecordType type) const {
    auto nca = content_provider.GetEntry(title_id, type);

    if (nca == nullptr) {
        return nullptr;
    }

    const PatchManager patch_manager{title_id, filesystem_controller, content_provider};

    return patch_manager.PatchRomFS(nca.get(), nca->GetRomFS(), type);
}

namespace {
// One global set, replaced on every boot and emptied at shutdown; only one title runs at a time.
// Directories are kept and built into a RomFS on first use: building walks every file, and most
// titles never open their sibling programs. Never destroyed: its files belong to the host
// filesystem, which may already be gone when static destructors run at exit.
struct BootContentState {
    std::mutex mutex;
    std::vector<BootContent> entries;
    std::map<std::pair<u64, ContentRecordType>, VirtualFile> romfs;
};
BootContentState& GetBootContentState() {
    static auto* const state = new BootContentState;
    return *state;
}
} // namespace

void SetBootContent(std::vector<BootContent> entries) {
    auto& state = GetBootContentState();
    std::scoped_lock lk{state.mutex};
    state.entries = std::move(entries);
    state.romfs.clear();
}

std::vector<u64> ListBootContent(ContentRecordType type) {
    auto& state = GetBootContentState();
    std::scoped_lock lk{state.mutex};
    std::vector<u64> ids;
    for (const auto& entry : state.entries) {
        if (entry.type == type) {
            ids.push_back(entry.title_id);
        }
    }
    return ids;
}

VirtualFile OpenBootContent(u64 title_id, ContentRecordType type) {
    auto& state = GetBootContentState();
    std::scoped_lock lk{state.mutex};
    if (const auto it = state.romfs.find({title_id, type}); it != state.romfs.end()) {
        return it->second;
    }
    for (const auto& entry : state.entries) {
        if (entry.title_id == title_id && entry.type == type) {
            LOG_INFO(Service_FS, "Serving {:016X} ({}) RomFS from {}", title_id,
                     type == ContentRecordType::Data ? "add-on" : "program",
                     entry.romfs->GetFullPath());
            return state.romfs[{title_id, type}] = CreateRomFS(entry.romfs);
        }
    }
    return nullptr;
}

bool IsAddOnContentDisabled(u64 program_id) {
    const auto disabled_in = [](u64 key) {
        const auto it = Settings::values.disabled_addons.find(key);
        return it != Settings::values.disabled_addons.end() &&
               std::find(it->second.begin(), it->second.end(), "DLC") != it->second.end();
    };
    return disabled_in(program_id) || disabled_in(GetBaseTitleID(program_id));
}

VirtualFile RomFSFactory::OpenPatchedRomFSWithProgramIndex(u64 title_id, u8 program_index,
                                                           ContentRecordType type) const {
    const auto res_title_id = GetBaseTitleIDWithProgramIndex(title_id, program_index);

    if (auto romfs = OpenPatchedRomFS(res_title_id, type); romfs != nullptr) {
        return romfs;
    }
    if (type == ContentRecordType::Program) {
        if (auto romfs = OpenBootContent(res_title_id, type)) {
            const PatchManager mods{res_title_id, filesystem_controller, content_provider};
            return mods.PatchRomFS(nullptr, std::move(romfs), type, nullptr);
        }
    }
    if (program_index == 0 && !updatable) {
        // NUP containers and loose dumps aren't in the content provider: use the loader's RomFS.
        return file;
    }
    return nullptr;
}

VirtualFile RomFSFactory::Open(u64 title_id, StorageId storage, ContentRecordType type) const {
    if (type == ContentRecordType::Data) {
        if (auto romfs = OpenBootContent(title_id, type)) {
            return romfs;
        }
    }

    const std::shared_ptr<NCA> res = GetEntry(title_id, storage, type);
    if (res == nullptr) {
        return nullptr;
    }

    return res->GetRomFS();
}

std::shared_ptr<NCA> RomFSFactory::GetEntry(u64 title_id, StorageId storage,
                                            ContentRecordType type) const {
    switch (storage) {
    case StorageId::None:
        return content_provider.GetEntry(title_id, type);
    case StorageId::NandSystem:
        return filesystem_controller.GetSystemNANDContents()->GetEntry(title_id, type);
    case StorageId::NandUser:
        return filesystem_controller.GetUserNANDContents()->GetEntry(title_id, type);
    case StorageId::SdCard:
        return filesystem_controller.GetSDMCContents()->GetEntry(title_id, type);
    case StorageId::Host:
    case StorageId::GameCard:
    default:
        UNIMPLEMENTED_MSG("Unimplemented storage_id={:02X}", static_cast<u8>(storage));
        return nullptr;
    }
}

} // namespace FileSys
