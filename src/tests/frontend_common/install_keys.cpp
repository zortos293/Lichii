// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <fstream>
#include <random>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include "common/fs/path_util.h"
#include "core/crypto/key_manager.h"
#include "frontend_common/firmware_manager.h"
#include "tests/common/restore_keys.h"

namespace {

/// Points the keys folder at an empty temporary directory for one test.
class KeysDirectory {
public:
    KeysDirectory()
        : original{Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir)},
          root{std::filesystem::temp_directory_path() /
               fmt::format("lichii-keys-{}", std::random_device{}())},
          keys{root / "keys"} {
        REQUIRE(std::filesystem::create_directories(keys));
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, keys);
    }

    ~KeysDirectory() {
        // The tests read keys from this folder into the KeyManager; forget them as well.
        Tests::RestoreKeys(original);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    KeysDirectory(const KeysDirectory&) = delete;
    KeysDirectory& operator=(const KeysDirectory&) = delete;

    const std::filesystem::path original;
    const std::filesystem::path root;
    const std::filesystem::path keys;
};

void WriteFile(const std::filesystem::path& path, const char* text) {
    std::ofstream file{path};
    file << text;
    REQUIRE(file.good());
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file{path};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::string ValidProdKeys() {
    std::string keys = fmt::format("header_key = {}\n", std::string(64, '1'));
    for (int generation = 0; generation < 5; ++generation) {
        const auto index = fmt::format("{:02X}", generation);
        keys += fmt::format("master_key_{} = {}\n", index, std::string(32, '2'));
        keys += fmt::format("key_area_key_application_{} = {}\n", index, std::string(32, '3'));
        keys += fmt::format("key_area_key_ocean_{} = {}\n", index, std::string(32, '4'));
        keys += fmt::format("key_area_key_system_{} = {}\n", index, std::string(32, '5'));
        keys += fmt::format("titlekek_{} = {}\n", index, std::string(32, '6'));
    }
    return keys;
}

} // Anonymous namespace

TEST_CASE("Installing the keys already in the keys folder does not copy them onto themselves",
          "[frontend_common][install_keys]") {
    const KeysDirectory directory;
    const auto prod_keys = directory.keys / "prod.keys";
    WriteFile(prod_keys, "header_key = 00\n");
    WriteFile(directory.keys / "title.keys", "0100000000000000 = 00\n");

    // The dummy keys are not valid ones, so the result may be ErrorFailedInit; what matters is
    // that the copy neither throws (it aborted the emulator) nor reports a failed copy.
    FirmwareManager::KeyInstallResult result{};
    REQUIRE_NOTHROW(result = FirmwareManager::InstallKeys(prod_keys.string(), "keys"));
    CHECK(result != FirmwareManager::ErrorFailedCopy);
    CHECK(ReadFile(prod_keys) == "header_key = 00\n");
}

TEST_CASE("Reinstalling installed prod.keys does not duplicate its contents",
          "[frontend_common][install_keys]") {
    const KeysDirectory directory;
    const auto valid_keys = ValidProdKeys();
    const auto prod_keys = directory.keys / "prod.keys";
    WriteFile(prod_keys, valid_keys.c_str());

    FirmwareManager::KeyInstallResult result{};
    REQUIRE_NOTHROW(result = FirmwareManager::InstallKeys(prod_keys.string(), "keys"));
    CHECK(result == FirmwareManager::Success);
    CHECK(ReadFile(prod_keys) == valid_keys);
}

TEST_CASE("Installing keys from another folder copies them into the keys folder",
          "[frontend_common][install_keys]") {
    const KeysDirectory directory;
    const auto source = directory.root / "dump";
    REQUIRE(std::filesystem::create_directory(source));
    const auto valid_keys = ValidProdKeys();
    WriteFile(source / "prod.keys", valid_keys.c_str());
    WriteFile(source / "title.keys", "0100000000000000 = 11\n");
    WriteFile(directory.keys / "prod.keys", "header_key = 00\n");
    REQUIRE(std::filesystem::create_directory(directory.keys / "backups"));
    WriteFile(directory.keys / "backups" / "old.keys", "header_key = 00\n");

    FirmwareManager::KeyInstallResult result{};
    REQUIRE_NOTHROW(result = FirmwareManager::InstallKeys((source / "prod.keys").string(), "keys"));
    CHECK(result == FirmwareManager::Success);
    CHECK(ReadFile(directory.keys / "prod.keys") == std::string("header_key = 00\n") + valid_keys);
    CHECK(ReadFile(directory.keys / "title.keys") == "0100000000000000 = 11\n");
}

TEST_CASE("Installing invalid replacement keys leaves the installed keys intact",
          "[frontend_common][install_keys]") {
    const KeysDirectory directory;
    const auto valid_keys = ValidProdKeys();
    WriteFile(directory.keys / "prod.keys", valid_keys.c_str());
    WriteFile(directory.keys / "title.keys", "0100000000000000 = 11\n");
    Core::Crypto::KeyManager::Instance().ReloadKeys();
    REQUIRE_FALSE(Core::Crypto::KeyManager::Instance().BaseDeriveNecessary());

    const auto source = directory.root / "dump";
    REQUIRE(std::filesystem::create_directory(source));
    WriteFile(source / "prod.keys", "");

    FirmwareManager::KeyInstallResult result{};
    REQUIRE_NOTHROW(result = FirmwareManager::InstallKeys((source / "prod.keys").string(), "keys"));
    CHECK(result == FirmwareManager::ErrorFailedInit);
    CHECK(ReadFile(directory.keys / "prod.keys") == valid_keys);
    CHECK(ReadFile(directory.keys / "title.keys") == "0100000000000000 = 11\n");
}

TEST_CASE("An empty replacement is not validated by installed autogenerated keys",
          "[frontend_common][install_keys]") {
    const KeysDirectory directory;
    const auto valid_keys = ValidProdKeys();
    WriteFile(directory.keys / "prod.keys_autogenerated", valid_keys.c_str());
    WriteFile(directory.keys / "prod.keys", valid_keys.c_str());
    Core::Crypto::KeyManager::Instance().ReloadKeys();
    REQUIRE_FALSE(Core::Crypto::KeyManager::Instance().BaseDeriveNecessary());

    const auto source = directory.root / "dump";
    REQUIRE(std::filesystem::create_directory(source));
    WriteFile(source / "prod.keys", "");

    FirmwareManager::KeyInstallResult result{};
    REQUIRE_NOTHROW(result = FirmwareManager::InstallKeys((source / "prod.keys").string(), "keys"));
    CHECK(result == FirmwareManager::ErrorFailedInit);
    CHECK(ReadFile(directory.keys / "prod.keys") == valid_keys);
}

TEST_CASE("Installing replacement keys preserves keys found only in the installed prod.keys",
          "[frontend_common][install_keys]") {
    const KeysDirectory directory;
    const auto valid_keys = ValidProdKeys();
    const std::string installed_only =
        fmt::format("keyblob_00 = {}\n", std::string(0x90 * 2, '7'));
    const auto installed_keys = valid_keys + installed_only;
    WriteFile(directory.keys / "prod.keys", installed_keys.c_str());

    const auto source = directory.root / "dump";
    REQUIRE(std::filesystem::create_directory(source));
    WriteFile(source / "prod.keys", valid_keys.c_str());

    FirmwareManager::KeyInstallResult result{};
    REQUIRE_NOTHROW(result = FirmwareManager::InstallKeys((source / "prod.keys").string(), "keys"));
    CHECK(result == FirmwareManager::Success);
    CHECK(ReadFile(directory.keys / "prod.keys").find(installed_only) != std::string::npos);
}

TEST_CASE("A keys file line with a short value does not replace the key read before it",
          "[frontend_common][install_keys]") {
    using namespace Core::Crypto;
    const KeysDirectory directory;
    // Made-up keys of the right length, each followed by a line for the same key that is too short.
    // Not header_key: the KeyManager keeps what this loads for the rest of the run, and a header key
    // makes every later test that needs none skip itself.
    const std::string source(64, '1');
    const std::string rights_id = "0100000000000000" + std::string(16, '0');
    const std::string title(32, '2');
    WriteFile(directory.keys / "prod.keys",
              fmt::format("sd_card_save_key_source = {}\nsd_card_save_key_source = 00\n", source)
                  .c_str());
    WriteFile(
        directory.keys / "title.keys",
        fmt::format("{} = {}\n{} = 00\n0100000000000000 = {}\n", rights_id, title, rights_id, title)
            .c_str());

    KeyManager& keys = KeyManager::Instance();
    keys.ReloadKeys();
    Key256 expected_source;
    expected_source.fill(0x11);
    CHECK(keys.GetKey(S256KeyType::SDKeySource, static_cast<u64>(SDKeyType::Save)) ==
          expected_source);
    Key128 expected_title;
    expected_title.fill(0x22);
    // Stored as the rights ID's two halves; the line with half a rights ID is skipped too.
    CHECK(keys.GetKey(S128KeyType::Titlekey, 0, 0x0000000000000001ULL) == expected_title);
    CHECK_FALSE(keys.HasKey(S128KeyType::Titlekey, 0, 0));
}

TEST_CASE("A keys test leaves the key manager as it found it", "[frontend_common][install_keys]") {
    using namespace Core::Crypto;
    KeyManager& keys = KeyManager::Instance();
    const bool had_header = keys.HasKey(S256KeyType::Header);
    const Key256 header_before = keys.GetKey(S256KeyType::Header);
    {
        const KeysDirectory directory;
        WriteFile(directory.keys / "prod.keys", fmt::format("header_key = {}\n",
                                                            std::string(64, '3')).c_str());
        keys.ReloadKeys();
        Key256 loaded;
        loaded.fill(0x33);
        REQUIRE(keys.GetKey(S256KeyType::Header) == loaded);
    }
    CHECK(keys.HasKey(S256KeyType::Header) == had_header);
    CHECK(keys.GetKey(S256KeyType::Header) == header_before);
}
