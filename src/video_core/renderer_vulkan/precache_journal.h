// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/fs/file_lock.h"
#include "common/fs/path_util.h"
#include "common/logging.h"

namespace Vulkan {

/// The shader precache, the background prebuild and the boot load build pipelines the game hasn't
/// drawn yet, and a driver compiler crash on one of them ends the whole process; the next start
/// reaches the same pipeline again and crashes again (Mesa ANV 26.2 crashes in
/// brw_instruction_scheduler compiling 3 of Ocarina of Time's ~60,000 warm-up fragment shaders).
/// Each driver stage writes the id of the build it is in to a slot of "<base>.inflight", flushed
/// at once. At the next start the ids left there join "<base>.poison" and are skipped from then on
/// (a kill during these builds leaves the builds in flight there too: one per slot, built later
/// when the game uses them).
/// Both files carry `stamp` (the driver, device, cache version and anything else the ids depend
/// on): a new driver tries the skipped builds again, and in-flight ids are only taken over by the
/// stamp they were written for.
class PrecacheJournal {
public:
    PrecacheJournal(const std::filesystem::path& base, std::string stamp_, size_t slots,
                    const char* what)
        : owner{WithSuffix(base, ".lock")}, inflight_path{WithSuffix(base, ".inflight")},
          poison_path{WithSuffix(base, ".poison")}, stamp{std::move(stamp_)} {
        if (!owner.Held()) {
            return;
        }
        {
            std::ifstream in(poison_path);
            std::string line;
            if (in && std::getline(in, line) && line == stamp) {
                u64 id{};
                while (in >> id) {
                    poisoned.insert(id);
                }
            }
        }
        size_t fresh = 0;
        {
            // The stamp line, then one binary id per slot.
            std::ifstream in(inflight_path, std::ios::binary);
            std::string line;
            if (in && std::getline(in, line) && line == stamp) {
                u64 id{};
                while (in.read(reinterpret_cast<char*>(&id), sizeof(id))) {
                    if (id != Idle && poisoned.insert(id).second) {
                        ++fresh;
                    }
                }
            }
        }
        if (fresh != 0) {
            LOG_WARNING(Render_Vulkan,
                        "Shader precache: the last {} precache stopped during {} builds (a driver "
                        "crash?); they are skipped from now on with this driver",
                        what, fresh);
        }
        // The new ids must be durable before the in-flight file that names them is reset: write
        // the whole list beside the old one and rename it over it.
        if (!WritePoison()) {
            LOG_ERROR(Render_Vulkan,
                      "Shader precache: could not save {}; the {} builds journal is off for this "
                      "precache",
                      Common::FS::PathToUTF8String(poison_path), what);
            return;
        }
        if (!poisoned.empty()) {
            LOG_INFO(Render_Vulkan, "Shader precache: {} {} builds skipped (listed in {})",
                     poisoned.size(), what, Common::FS::PathToUTF8String(poison_path));
        }
        inflight.open(inflight_path, std::ios::binary | std::ios::out | std::ios::trunc);
        inflight << stamp << '\n';
        slots_offset = static_cast<std::streamoff>(stamp.size() + 1);
        const std::vector<u64> idle(slots, Idle);
        inflight.write(reinterpret_cast<const char*>(idle.data()),
                       static_cast<std::streamsize>(idle.size() * sizeof(u64)));
        inflight.flush();
    }

    /// True for a build the driver crashed on (or may have) with this stamp.
    [[nodiscard]] bool Poisoned(u64 id) const {
        return poisoned.contains(id);
    }
    [[nodiscard]] size_t PoisonedCount() const {
        return poisoned.size();
    }
    void Begin(size_t slot, u64 id) {
        Write(slot, id);
    }
    void End(size_t slot) {
        Write(slot, Idle);
    }

    /// Ids never take this value.
    static constexpr u64 Idle = ~u64{0};

private:
    static std::filesystem::path WithSuffix(std::filesystem::path path, const char* suffix) {
        path += suffix;
        return path;
    }

    /// Replaces the poison list with `poisoned` under `stamp`; false (old list kept) on failure.
    bool WritePoison() const {
        const std::filesystem::path tmp = WithSuffix(poison_path, ".tmp");
        {
            std::ofstream out(tmp, std::ios::trunc);
            out << stamp << '\n';
            for (const u64 id : poisoned) {
                out << id << '\n';
            }
            out.flush();
            if (!out) {
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                return false;
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, poison_path, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            return false;
        }
        return true;
    }

    /// Flushed at once: the next start reads it after a crash of this process.
    void Write(size_t slot, u64 value) {
        std::scoped_lock lock{mutex};
        if (!inflight.is_open() || !inflight) {
            return;
        }
        inflight.seekp(slots_offset + static_cast<std::streamoff>(slot * sizeof(u64)));
        inflight.write(reinterpret_cast<const char*>(&value), sizeof(value));
        inflight.flush();
    }

    Common::FS::FileLock owner;
    std::filesystem::path inflight_path;
    std::filesystem::path poison_path;
    std::string stamp;
    std::unordered_set<u64> poisoned; ///< Read-only once the threads start
    std::mutex mutex;
    std::ofstream inflight;
    std::streamoff slots_offset{};
};

/// What the ids of the journal depend on: the driver and device (pipeline keys don't name them),
/// and the versions of the caches the keys come from.
inline std::string DriverBuildStamp(u32 driver_id, u32 driver_version, u32 vendor_id, u32 device_id,
                                    u32 cache_version, u32 format_version) {
    return fmt::format("driver {} {} device {:x}:{:x} cache {} v2 {}", driver_id, driver_version,
                       vendor_id, device_id, cache_version, format_version);
}

/// PrecacheJournal around the driver stages (vkCreate*Pipelines) of jobs nobody waits for: the
/// precache, the background prebuild and the boot load of cached pipelines. Stages run on the
/// driver workers, or on a thread that promoted its job, so a slot is taken for the length of a
/// build instead of being tied to a thread; a kill leaves at most one id per slot. Builds that
/// find no free slot are not recorded.
class DriverBuildJournal {
public:
    DriverBuildJournal(const std::filesystem::path& base, std::string stamp, size_t slots,
                       const char* what)
        : journal{base, std::move(stamp), slots, what}, free_slots(slots) {
        std::iota(free_slots.rbegin(), free_slots.rend(), size_t{0});
    }

    /// Ids never take PrecacheJournal::Idle.
    [[nodiscard]] static constexpr u64 Id(u64 hash) {
        return hash == PrecacheJournal::Idle ? hash - 1 : hash;
    }

    [[nodiscard]] bool Poisoned(u64 hash) const {
        return journal.Poisoned(Id(hash));
    }
    /// True for a build to leave out; counted.
    [[nodiscard]] bool Skip(u64 hash) {
        if (!Poisoned(hash)) {
            return false;
        }
        skipped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    [[nodiscard]] size_t Skipped() const {
        return skipped.load(std::memory_order_relaxed);
    }
    [[nodiscard]] size_t PoisonedCount() const {
        return journal.PoisonedCount();
    }

    /// Records a build for as long as it lives; does nothing with no journal.
    class Scope {
    public:
        Scope(DriverBuildJournal* journal_, u64 hash) : journal{journal_} {
            if (journal) {
                slot = journal->Begin(hash);
            }
        }
        ~Scope() {
            if (journal && slot) {
                journal->End(*slot);
            }
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        DriverBuildJournal* journal;
        std::optional<size_t> slot;
    };

private:
    std::optional<size_t> Begin(u64 hash) {
        size_t slot;
        {
            std::scoped_lock lock{mutex};
            if (free_slots.empty()) {
                return std::nullopt;
            }
            slot = free_slots.back();
            free_slots.pop_back();
        }
        journal.Begin(slot, Id(hash));
        return slot;
    }
    void End(size_t slot) {
        journal.End(slot);
        std::scoped_lock lock{mutex};
        free_slots.push_back(slot);
    }

    PrecacheJournal journal;
    std::atomic<size_t> skipped{};
    std::mutex mutex;
    std::vector<size_t> free_slots;
};

} // namespace Vulkan
