// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/fs/file_lock.h"

namespace VideoCommon {

inline constexpr u32 OpenGLPipelineCacheVersion = 48;

// The Vulkan disk cache holds guest environments (the guest code and the values its translation
// read), not SPIR-V: whatever is built from it is translated and emitted by the running build, so
// an emitter change needs no new version. Bump the version when the record layout changes. When
// translation starts asking the environment something older records don't hold, those records
// fail one by one and their pipelines are built when the game uses them; a new version would
// throw every record away instead. CompatibleVulkanPipelineCacheVersions lists the older versions
// with this version's record layout.
inline constexpr u32 VulkanPipelineCacheVersion = 54;

/// Older Vulkan cache versions with this version's record layout, newest first. A build that
/// finds no cache of its own version starts from the newest of these on disk, so an update keeps
/// the pipelines the game has already shown.
inline constexpr std::array<u32, 2> CompatibleVulkanPipelineCacheVersions{53, 52};

/// Header magic of the guest environment caches and of the Vulkan driver pipeline cache.
inline constexpr std::array<char, 8> PipelineCacheMagic{'y', 'u', 'z', 'u', 'c', 'a', 'c', 'h'};
inline constexpr std::array<char, 8> VulkanDriverPipelineCacheMagic{'y', 'u', 'z', 'u',
                                                                    'v', 'k', 'c', 'h'};

/// Whether `versions` are all below `version` and strictly decreasing. An empty list is valid.
[[nodiscard]] constexpr bool IsOlderVersionList(std::span<const u32> versions, u32 version) {
    u32 newer = version;
    for (const u32 older : versions) {
        if (older >= newer) {
            return false;
        }
        newer = older;
    }
    return true;
}
static_assert(IsOlderVersionList(CompatibleVulkanPipelineCacheVersions,
                                 VulkanPipelineCacheVersion));
// A version with another record layout must not start from these: their files would be read as
// its own. Check the list whenever the version changes, then update this.
static_assert(VulkanPipelineCacheVersion == 54,
              "Check CompatibleVulkanPipelineCacheVersions against the new version");

// Official Eglantine (cache version 18 at c0cb3325) and this fork (a different version) both
// used <shader dir>/<title>/vulkan.bin and vulkan_pipelines.bin. Each build deletes a file
// with another version at boot, so alternating builds made every run start with no pipelines
// (MKW log: "Deleting old pipeline cache", "Total Pipeline Count: 0", then hundreds of
// pipelines built on the GPU thread). Version-suffixed names let the builds coexist.
[[nodiscard]] inline std::filesystem::path VersionedPipelineCachePath(
    const std::filesystem::path& directory, std::string_view stem, u32 version) {
    return directory / fmt::format("{}.fork-v{}.bin", stem, version);
}

/// The file whose lock (Common::FS::FileLock) is held by whatever writes a guest environment cache
/// (PipelineCacheMagic), which is appended to in place, so that emulators running at once never
/// write it together or read what another is writing. The driver pipeline cache is replaced whole
/// by a rename and has no lock.
[[nodiscard]] inline std::filesystem::path PipelineCacheLockPath(
    const std::filesystem::path& file) {
    auto lock = file;
    lock += ".lock";
    return lock;
}

/// How long one waits for that lock while another emulator holds it: longer than a load, append
/// or rewrite takes, but a crashed emulator holds it while it writes a crash dump. Then a load goes
/// on as where the file system has no locks, and an append is left out.
inline constexpr std::chrono::seconds PipelineCacheLockWait{5};

/// True when the file starts with this magic number followed by this cache version.
[[nodiscard]] inline bool PipelineCacheHeaderMatches(const std::filesystem::path& file,
                                                     const std::array<char, 8>& magic,
                                                     u32 version) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream.is_open()) {
        return false;
    }
    std::array<char, 8> file_magic{};
    u32 file_version{};
    stream.read(file_magic.data(), file_magic.size());
    stream.read(reinterpret_cast<char*>(&file_version), sizeof(file_version));
    return stream.good() && file_magic == magic && file_version == version;
}

/// Moves a legacy-named cache that this exact cache version wrote to its versioned name, so an
/// existing warm cache survives the rename. A file of any other version (another build's) is
/// never touched; neither is an existing versioned file.
inline bool AdoptLegacyPipelineCache(const std::filesystem::path& legacy,
                                     const std::filesystem::path& versioned,
                                     const std::array<char, 8>& magic, u32 version) {
    std::error_code error;
    if (std::filesystem::exists(versioned, error) || error) {
        return false;
    }
    if (!PipelineCacheHeaderMatches(legacy, magic, version)) {
        return false;
    }
    std::filesystem::rename(legacy, versioned, error);
    return !error;
}

inline bool CopyCompatiblePipelineCache(const std::filesystem::path& source,
                                        const std::filesystem::path& destination,
                                        const std::array<char, 8>& magic, u32 source_version,
                                        u32 destination_version) {
    std::error_code error;
    if (std::filesystem::exists(destination, error) || error ||
        !PipelineCacheHeaderMatches(source, magic, source_version)) {
        return false;
    }
    auto temporary = destination;
    temporary += ".migrating";
    if (!std::filesystem::copy_file(source, temporary,
                                    std::filesystem::copy_options::overwrite_existing, error)) {
        return false;
    }
    {
        std::fstream stream(temporary, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekp(static_cast<std::streamoff>(magic.size()));
        stream.write(reinterpret_cast<const char*>(&destination_version),
                     sizeof(destination_version));
        stream.close();
        if (stream.fail()) {
            std::filesystem::remove(temporary, error);
            return false;
        }
    }
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

struct PipelineCacheCarryOver {
    std::optional<u32> version{}; ///< The older version the cache was started from
    bool failed{}; ///< An older cache to start from is there, but it couldn't be read or copied
};

/// Starts the `version` cache named `stem` from the newest cache of `versions` in `directory`,
/// unless a `version` cache already exists. Older files are copied, never changed, so the builds
/// that wrote them keep them.
inline PipelineCacheCarryOver CarryOverPipelineCache(const std::filesystem::path& directory,
                                                     std::string_view stem,
                                                     const std::array<char, 8>& magic,
                                                     std::span<const u32> versions, u32 version) {
    const auto destination = VersionedPipelineCachePath(directory, stem, version);
    std::error_code error;
    if (std::filesystem::exists(destination, error) || error) {
        return {};
    }
    // Another emulator starting the same cache, or appending to it once started, goes first: the
    // copy is made only while there is no cache of this version (see CopyCompatiblePipelineCache).
    std::optional<Common::FS::FileLock> lock;
    if (magic == PipelineCacheMagic) {
        lock.emplace(PipelineCacheLockPath(destination), PipelineCacheLockWait);
    }
    for (const u32 source_version : versions) {
        const auto source = VersionedPipelineCachePath(directory, stem, source_version);
        if (!std::filesystem::is_regular_file(source, error)) {
            continue;
        }
        // Only the newest cache: an older one would bring back pipelines the newer one has since
        // replaced. One that can't be opened (another process holds it) stops here too.
        if (!std::ifstream(source, std::ios::binary).is_open()) {
            return {.failed = true};
        }
        if (!PipelineCacheHeaderMatches(source, magic, source_version)) {
            continue; // Another file under that name, or one cut off before its header ends
        }
        if (CopyCompatiblePipelineCache(source, destination, magic, source_version, version)) {
            return {.version = source_version};
        }
        return {.failed = true};
    }
    return {};
}

enum class PipelineCacheClear {
    Missing, ///< There was no cache of this version, nor an older one to start from
    Cleared,
    Failed,
};

/// Empties the `version` cache named `stem`. When an older cache of `versions` is on disk, a
/// header-only (empty) cache of this version takes the place of the removed one, so the next boot
/// doesn't start from the older cache again; the older files stay with the builds that wrote them.
inline PipelineCacheClear ClearPipelineCache(const std::filesystem::path& directory,
                                             std::string_view stem,
                                             const std::array<char, 8>& magic,
                                             std::span<const u32> versions, u32 version) {
    const auto file = VersionedPipelineCachePath(directory, stem, version);
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    const bool has_older = std::ranges::any_of(versions, [&](u32 older) {
        std::error_code older_error;
        return std::filesystem::exists(VersionedPipelineCachePath(directory, stem, older),
                                       older_error);
    });
    if (!exists && !has_older) {
        return PipelineCacheClear::Missing;
    }
    // A running game appending to it or rewriting it finishes first, so it can't bring back what
    // is removed here.
    std::optional<Common::FS::FileLock> lock;
    if (magic == PipelineCacheMagic) {
        lock.emplace(PipelineCacheLockPath(file), PipelineCacheLockWait);
    }
    if (exists && !std::filesystem::remove(file, error)) {
        return PipelineCacheClear::Failed;
    }
    if (has_older) {
        std::ofstream stream(file, std::ios::binary);
        stream.write(magic.data(), magic.size());
        stream.write(reinterpret_cast<const char*>(&version), sizeof(version));
        if (!stream.good()) {
            return PipelineCacheClear::Failed;
        }
    }
    return PipelineCacheClear::Cleared;
}

/// Consecutive sessions of a title that ended without a healthy cache checkpoint, kept next to
/// the v2 cache as a small decimal count. An empty file (an in-flight load, or one a build that
/// kept no count left) reads as one strike, as does content a crash cut short. The boot load
/// writes the count before any driver work; a session that reaches its used-pipeline checkpoint
/// or a clean close removes the file, so the next boot after a crash builds nothing cached.
[[nodiscard]] inline u32 ReadBootStrikes(const std::filesystem::path& marker) {
    std::error_code error;
    if (!std::filesystem::exists(marker, error) || error) {
        return 0;
    }
    std::ifstream stream(marker, std::ios::binary);
    const std::string content{std::istreambuf_iterator<char>(stream),
                              std::istreambuf_iterator<char>()};
    const auto begin = content.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return 1;
    }
    u32 strikes{};
    const auto* const first = content.data() + begin;
    const auto* const last = content.data() + content.size();
    if (std::from_chars(first, last, strikes).ec != std::errc{} || strikes == 0) {
        return 1;
    }
    return strikes;
}

/// One small truncating write; a crash can leave garbage, which ReadBootStrikes reads as one
/// strike.
inline void WriteBootStrikes(const std::filesystem::path& marker, u32 strikes) {
    std::ofstream stream(marker, std::ios::binary | std::ios::trunc);
    stream << strikes << '\n';
}

/// What a boot does with a title's shader cache files, from the boot marker's state (#2165).
/// Only a boot free of strikes, with no other emulator loading, builds cached pipelines at boot
/// and reads the driver pipeline cache blob. A recovery boot (strikes on the marker) or one
/// beside another emulator's load builds nothing at boot and leaves the blob unread: the blob a
/// crashed session left is why the next boot crashed, and feeding it back to the driver can
/// crash the recovery boot the same way; it stays on disk, untouched, for diagnosis.
struct BootCachePlan {
    bool build_at_boot;
    bool read_driver_cache;
};

[[nodiscard]] inline BootCachePlan PlanBootCache(bool lock_held, bool lock_busy,
                                                 u32 boot_strikes) noexcept {
    const bool build_at_boot = lock_held ? boot_strikes == 0 : !lock_busy;
    return {build_at_boot, build_at_boot};
}

/// Whether this session's fresh driver pipeline cache blob may replace the file on disk. A boot
/// that left that file unread (a recovery, or one beside another emulator's load) keeps it for
/// diagnosis until the fresh cache holds more than an empty one. The empty size is not zero:
/// an empty cache still serializes its header, so the baseline is measured right after the fresh
/// cache is created (#2165).
[[nodiscard]] constexpr bool MayReplaceUnreadDriverBlob(bool blob_left_unread,
                                                        size_t fresh_blob_size,
                                                        size_t empty_blob_size) noexcept {
    return !blob_left_unread || fresh_blob_size > empty_blob_size;
}

/// Moves a driver pipeline cache blob a boot left unread aside before the boot strikes clear.
/// Clearing the strikes tells the next boot the on-disk blob is safe to read; a blob this
/// session never read and never replaced has not earned that, so it moves to <file>.unread,
/// still on disk for diagnosis, and the next boot starts the driver cache fresh. An earlier
/// quarantine is replaced. False when the move failed: the strikes must stand (#2165).
[[nodiscard]] inline bool QuarantineUnreadDriverBlob(const std::filesystem::path& filename) {
    auto quarantine = filename;
    quarantine += ".unread";
    std::error_code error;
    std::filesystem::remove(quarantine, error);
    error.clear();
    std::filesystem::rename(filename, quarantine, error);
    return !error;
}

} // namespace VideoCommon
