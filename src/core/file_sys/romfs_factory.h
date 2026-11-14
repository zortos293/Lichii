// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "core/hle/result.h"

namespace Loader {
class AppLoader;
} // namespace Loader

namespace Service::FileSystem {
class FileSystemController;
}

namespace FileSys {

class ContentProvider;
class NCA;

enum class ContentRecordType : u8;

enum class StorageId : u8 {
    None = 0,
    Host = 1,
    GameCard = 2,
    NandSystem = 3,
    NandUser = 4,
    SdCard = 5,
};

// Content of the booted title that has no NCA in the content provider (NUP containers). Program
// entries are the other programs of a multi-program application, keyed by the title ID of their
// program index (Switch 2 Editions run index 1 and read index 0 through
// OpenDataStorageWithProgramIndex). Data entries are add-on content, listed by aoc:u. Every boot
// replaces the whole set; RomFS images are built on first open.
struct BootContent {
    u64 title_id{};
    ContentRecordType type{};
    VirtualDir romfs;
};
void SetBootContent(std::vector<BootContent> entries);
[[nodiscard]] std::vector<u64> ListBootContent(ContentRecordType type);
/// The registered RomFS for a title ID and type, built on first use, or nullptr.
[[nodiscard]] VirtualFile OpenBootContent(u64 title_id, ContentRecordType type);
/// Whether the add-on content of a program is disabled ("DLC"). The per-game dialog saves the
/// setting under the program ID, aoc:u looked it up under the base title ID: either counts.
[[nodiscard]] bool IsAddOnContentDisabled(u64 program_id);

/// File system interface to the RomFS archive
class RomFSFactory {
public:
    explicit RomFSFactory(Loader::AppLoader& app_loader, ContentProvider& provider,
                          Service::FileSystem::FileSystemController& controller);
    ~RomFSFactory();

    void SetPackedUpdate(VirtualFile packed_update_raw);
    [[nodiscard]] VirtualFile OpenCurrentProcess(u64 current_process_title_id) const;
    [[nodiscard]] VirtualFile OpenPatchedRomFS(u64 title_id, ContentRecordType type) const;
    [[nodiscard]] VirtualFile OpenPatchedRomFSWithProgramIndex(u64 title_id, u8 program_index,
                                                               ContentRecordType type) const;
    [[nodiscard]] VirtualFile Open(u64 title_id, StorageId storage, ContentRecordType type) const;
    [[nodiscard]] std::shared_ptr<NCA> GetEntry(u64 title_id, StorageId storage,
                                                ContentRecordType type) const;

private:
    VirtualFile file;
    VirtualFile packed_update_raw;

    VirtualFile base;

    bool updatable;

    ContentProvider& content_provider;
    Service::FileSystem::FileSystemController& filesystem_controller;
};

} // namespace FileSys
