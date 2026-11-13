// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <random>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/precache_journal.h"

namespace {
struct TempDir {
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("precache-journal-" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::filesystem::path path;
};
} // Anonymous namespace

TEST_CASE("A build left in flight is skipped by the next precache", "[video_core][precache]") {
    TempDir temp;
    const auto base = temp.path / "precache-jobs";
    {
        Vulkan::PrecacheJournal journal{base, "driver 1", 4, "pipeline"};
        REQUIRE_FALSE(journal.Poisoned(42));
        journal.Begin(2, 42); // the process ends here (a driver crash): no End
        journal.Begin(1, 7);
        journal.End(1);
    }
    {
        Vulkan::PrecacheJournal next{base, "driver 1", 4, "pipeline"};
        REQUIRE(next.Poisoned(42));
        REQUIRE_FALSE(next.Poisoned(7));
        REQUIRE(next.PoisonedCount() == 1);
    }
    // Still skipped at the start after (the poison list is kept, the in-flight slots reset).
    Vulkan::PrecacheJournal third{base, "driver 1", 4, "pipeline"};
    REQUIRE(third.Poisoned(42));
}

TEST_CASE("A new driver tries the skipped builds again", "[video_core][precache]") {
    TempDir temp;
    const auto base = temp.path / "warm-graphics.bin";
    {
        Vulkan::PrecacheJournal journal{base, "driver 1", 2, "graphics program"};
        journal.Begin(0, 9);
    }
    REQUIRE(Vulkan::PrecacheJournal{base, "driver 1", 2, "graphics program"}.Poisoned(9));
    REQUIRE_FALSE(Vulkan::PrecacheJournal{base, "driver 2", 2, "graphics program"}.Poisoned(9));
}

TEST_CASE("Builds in flight with one driver are not skipped by the next",
          "[video_core][precache]") {
    TempDir temp;
    const auto base = temp.path / "precache-jobs";
    {
        Vulkan::PrecacheJournal journal{base, "driver 1", 2, "pipeline"};
        journal.Begin(0, 5); // ended by a kill or crash, then the driver is updated
    }
    // Recovered first by the new driver: its in-flight ids belong to driver 1.
    Vulkan::PrecacheJournal updated{base, "driver 2", 2, "pipeline"};
    REQUIRE_FALSE(updated.Poisoned(5));
    REQUIRE(updated.PoisonedCount() == 0);
    // Nor are they saved under driver 2 for later starts.
    REQUIRE_FALSE(Vulkan::PrecacheJournal{base, "driver 2", 2, "pipeline"}.Poisoned(5));
}

TEST_CASE("In-flight ids survive a poison list that cannot be saved", "[video_core][precache]") {
    TempDir temp;
    const auto base = temp.path / "precache-jobs";
    {
        Vulkan::PrecacheJournal journal{base, "driver 1", 2, "pipeline"};
        journal.Begin(1, 77);
    }
    auto poison = base;
    poison += ".poison";
    std::filesystem::remove(poison);
    std::filesystem::create_directory(poison); // the new list cannot replace it
    {
        Vulkan::PrecacheJournal blocked{base, "driver 1", 2, "pipeline"};
        REQUIRE(blocked.Poisoned(77)); // still skipped by this precache
        blocked.Begin(0, 78);          // the journal is off: not recorded
    }
    std::filesystem::remove(poison);
    // The in-flight file was not reset, so the next start still names 77.
    Vulkan::PrecacheJournal next{base, "driver 1", 2, "pipeline"};
    REQUIRE(next.Poisoned(77));
    REQUIRE_FALSE(next.Poisoned(78));
    auto tmp = poison;
    tmp += ".tmp";
    REQUIRE_FALSE(std::filesystem::exists(tmp));
}

TEST_CASE("The stamp changes with the driver, device and cache versions",
          "[video_core][precache]") {
    using Vulkan::DriverBuildStamp;
    const auto base = DriverBuildStamp(1, 100, 0x8086, 0xe20b, 7, 2);
    REQUIRE(base == DriverBuildStamp(1, 100, 0x8086, 0xe20b, 7, 2));
    REQUIRE(base != DriverBuildStamp(2, 100, 0x8086, 0xe20b, 7, 2));
    REQUIRE(base != DriverBuildStamp(1, 101, 0x8086, 0xe20b, 7, 2));
    REQUIRE(base != DriverBuildStamp(1, 100, 0x10de, 0xe20b, 7, 2));
    REQUIRE(base != DriverBuildStamp(1, 100, 0x8086, 0xe20c, 7, 2));
    REQUIRE(base != DriverBuildStamp(1, 100, 0x8086, 0xe20b, 8, 2));
    REQUIRE(base != DriverBuildStamp(1, 100, 0x8086, 0xe20b, 7, 3));
}

TEST_CASE("A kill leaves at most one id per slot of the driver build journal",
          "[video_core][precache]") {
    TempDir temp;
    const auto base = temp.path / "driver-builds";
    {
        Vulkan::DriverBuildJournal journal{base, "driver 1", 2, "pipeline"};
        {
            const Vulkan::DriverBuildJournal::Scope done{&journal, 1};
        }
        // Three builds begin, but only two slots exist; the third is not recorded.
        const Vulkan::DriverBuildJournal::Scope first{&journal, 10};
        const Vulkan::DriverBuildJournal::Scope second{&journal, 11};
        const Vulkan::DriverBuildJournal::Scope third{&journal, 12};
        // The process is copied as it stands: a kill here.
        std::filesystem::copy_file(base.string() + ".inflight", base.string() + ".killed");
    }
    std::filesystem::copy_file(base.string() + ".killed", base.string() + ".inflight",
                               std::filesystem::copy_options::overwrite_existing);
    Vulkan::DriverBuildJournal next{base, "driver 1", 2, "pipeline"};
    REQUIRE(next.PoisonedCount() == 2);
    REQUIRE(next.Poisoned(10));
    REQUIRE(next.Poisoned(11));
    REQUIRE_FALSE(next.Poisoned(12));
    REQUIRE_FALSE(next.Poisoned(1));
}

TEST_CASE("Slots of the driver build journal are reused once a build ends",
          "[video_core][precache]") {
    TempDir temp;
    const auto base = temp.path / "driver-builds";
    {
        Vulkan::DriverBuildJournal journal{base, "driver 1", 1, "pipeline"};
        for (u64 id = 1; id <= 5; ++id) {
            const Vulkan::DriverBuildJournal::Scope scope{&journal, id};
        }
        const Vulkan::DriverBuildJournal::Scope last{&journal, 6};
        std::filesystem::copy_file(base.string() + ".inflight", base.string() + ".killed");
    }
    std::filesystem::copy_file(base.string() + ".killed", base.string() + ".inflight",
                               std::filesystem::copy_options::overwrite_existing);
    Vulkan::DriverBuildJournal next{base, "driver 1", 1, "pipeline"};
    REQUIRE(next.PoisonedCount() == 1);
    REQUIRE(next.Poisoned(6));
}

TEST_CASE("Skip counts the poisoned builds and leaves the others", "[video_core][precache]") {
    TempDir temp;
    const auto base = temp.path / "driver-builds";
    {
        Vulkan::DriverBuildJournal journal{base, "driver 1", 2, "pipeline"};
        const Vulkan::DriverBuildJournal::Scope crashed{&journal, 77};
    }
    // The scope ended: nothing was left in flight.
    {
        Vulkan::DriverBuildJournal clean{base, "driver 1", 2, "pipeline"};
        REQUIRE_FALSE(clean.Skip(77));
        REQUIRE(clean.Skipped() == 0);
        const Vulkan::DriverBuildJournal::Scope crashed{&clean, 78};
        std::filesystem::copy_file(base.string() + ".inflight", base.string() + ".killed");
    }
    std::filesystem::copy_file(base.string() + ".killed", base.string() + ".inflight",
                               std::filesystem::copy_options::overwrite_existing);
    Vulkan::DriverBuildJournal next{base, "driver 1", 2, "pipeline"};
    REQUIRE(next.Skip(78));
    REQUIRE(next.Skip(78));
    REQUIRE_FALSE(next.Skip(79));
    REQUIRE(next.Skipped() == 2);
}

TEST_CASE("A hash equal to the idle marker still gets recorded", "[video_core][precache]") {
    using Vulkan::DriverBuildJournal;
    using Vulkan::PrecacheJournal;
    REQUIRE(DriverBuildJournal::Id(PrecacheJournal::Idle) != PrecacheJournal::Idle);
    REQUIRE(DriverBuildJournal::Id(5) == 5);
    TempDir temp;
    const auto base = temp.path / "driver-builds";
    {
        DriverBuildJournal journal{base, "driver 1", 1, "pipeline"};
        const DriverBuildJournal::Scope scope{&journal, PrecacheJournal::Idle};
        std::filesystem::copy_file(base.string() + ".inflight", base.string() + ".killed");
    }
    std::filesystem::copy_file(base.string() + ".killed", base.string() + ".inflight",
                               std::filesystem::copy_options::overwrite_existing);
    DriverBuildJournal next{base, "driver 1", 1, "pipeline"};
    REQUIRE(next.Poisoned(PrecacheJournal::Idle));
}

TEST_CASE("No journal records nothing", "[video_core][precache]") {
    const Vulkan::DriverBuildJournal::Scope scope{nullptr, 1};
}

TEST_CASE("A second journal cannot consume or overwrite a live owner's builds",
          "[video_core][precache]") {
    TempDir temp;
    const auto base = temp.path / "driver-builds";
    {
        Vulkan::PrecacheJournal owner{base, "driver 1", 2, "pipeline"};
        owner.Begin(0, 41);
        {
            Vulkan::PrecacheJournal contender{base, "driver 1", 2, "pipeline"};
            CHECK_FALSE(contender.Poisoned(41));
            contender.Begin(0, 99);
            contender.End(0);
        }
        owner.Begin(1, 42);
        owner.End(1);
    }
    Vulkan::PrecacheJournal recovered{base, "driver 1", 2, "pipeline"};
    CHECK(recovered.Poisoned(41));
    CHECK_FALSE(recovered.Poisoned(42));
    CHECK_FALSE(recovered.Poisoned(99));
    CHECK(recovered.PoisonedCount() == 1);
}
