// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/settings.h"
#include "core/hle/service/acc/profile_manager.h"
#include "frontend_common/cloud_sync/save_archive.h"
#include "tests/common/restore_eden_path.h"

namespace Tests {

// A NAND and save folder of their own, with the ProfileManager it loads (one user, "Eden", when
// new), so that a test can play two machines syncing through one Drive.
class CloudSyncMachine {
public:
    CloudSyncMachine() {
        std::filesystem::create_directories(nand);
        previous_nand = Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir);
        previous_save = Common::FS::GetEdenPath(Common::FS::EdenPath::SaveDir);
        Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir, nand);
        Common::FS::SetEdenPath(Common::FS::EdenPath::SaveDir, nand);
        // Opening the profiles picks the first user when current_user isn't one of them.
        previous_user = Settings::values.current_user.GetValue();
        profiles.emplace();
    }

    ~CloudSyncMachine() {
        profiles.reset();
        Settings::values.current_user = previous_user;
        Tests::RestoreEdenPath(Common::FS::EdenPath::NANDDir, previous_nand);
        Tests::RestoreEdenPath(Common::FS::EdenPath::SaveDir, previous_save);
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    Common::UUID FirstUser() const {
        return *profiles->GetUser(0);
    }

    static void WriteSave(const FrontendCommon::CloudSync::SaveId& save, std::string_view text) {
        const auto folder = FrontendCommon::CloudSync::LocalSavePath(save);
        std::filesystem::create_directories(folder);
        std::ofstream{folder / "save.dat", std::ios::binary} << text;
    }

    // The save's save.dat, empty when there is none.
    static std::string ReadSave(const FrontendCommon::CloudSync::SaveId& save) {
        std::ifstream file{FrontendCommon::CloudSync::LocalSavePath(save) / "save.dat",
                           std::ios::binary};
        return {std::istreambuf_iterator<char>{file}, {}};
    }

    // A save whose save.dat holds text, packed as an upload from elsewhere would be.
    FrontendCommon::CloudSync::PackedSave Packed(std::string_view text) const {
        const auto folder = root / "packed";
        std::filesystem::create_directories(folder);
        std::ofstream file{folder / "save.dat", std::ios::binary};
        file << text;
        file.close();
        const bool written = !file.fail();
        auto packed = written ? FrontendCommon::CloudSync::PackSave(folder) : std::nullopt;
        std::filesystem::remove_all(folder);
        REQUIRE(written);
        REQUIRE(packed.has_value());
        return *packed;
    }

    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        fmt::format("lichii-cloud-sync-machine-{}", std::random_device{}());
    std::filesystem::path nand = root / "nand";
    std::optional<Service::Account::ProfileManager> profiles;

private:
    std::filesystem::path previous_nand;
    std::filesystem::path previous_save;
    s32 previous_user{};
};

} // namespace Tests
