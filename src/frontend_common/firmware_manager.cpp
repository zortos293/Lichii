// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "firmware_manager.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <common/fs/fs_paths.h>
#include <common/scope_exit.h>

#include "common/fs/fs.h"
#include "common/fs/path_util.h"

#include "common/logging.h"

#include "core/crypto/key_manager.h"
#include "frontend_common/content_manager.h"

#ifdef __ANDROID__
#include <jni.h>
#include <common/android/id_cache.h>
#include <common/android/android_common.h>
#endif

namespace {

bool AppendKeyFile(const std::filesystem::path& input, std::ofstream& output) {
    std::ifstream file{input};
    if (!file) {
        return false;
    }
    std::string line;
    while (std::getline(file, line)) {
        output << line << '\n';
    }
    return !file.bad() && output.good();
}

bool SetOwnerOnlyPermissions(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, error);
    return !error;
}

bool CreateOwnerOnlyFile(const std::filesystem::path& path) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (!file) {
        return false;
    }
    file.close();
    return SetOwnerOnlyPermissions(path);
}

bool PathsReferToSameFile(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
    std::error_code error;
    const bool same = std::filesystem::equivalent(lhs, rhs, error);
    if (!error) {
        return same;
    }
    return lhs == rhs;
}

bool WriteMergedKeyFile(const std::filesystem::path& existing,
                        const std::filesystem::path& replacement,
                        const std::filesystem::path& output) {
    std::error_code exists_error;
    const bool has_existing = std::filesystem::exists(existing, exists_error);
    if (exists_error) {
        return false;
    }
    if (!CreateOwnerOnlyFile(output)) {
        return false;
    }
    if (has_existing && PathsReferToSameFile(existing, replacement)) {
        std::error_code copy_error;
        if (!std::filesystem::copy_file(replacement, output,
                                        std::filesystem::copy_options::overwrite_existing,
                                        copy_error)) {
            return false;
        }
        return SetOwnerOnlyPermissions(output);
    }
    std::ofstream file{output, std::ios::binary | std::ios::app};
    if (!file) {
        return false;
    }
    if (has_existing && !AppendKeyFile(existing, file)) {
        return false;
    }
    if (!AppendKeyFile(replacement, file)) {
        return false;
    }
    file.flush();
    return file.good() && SetOwnerOnlyPermissions(output);
}

std::filesystem::path StageKeyFiles(
    const std::vector<std::filesystem::path>& source_key_files) {
    const auto nonce = static_cast<unsigned long long>(
                           std::chrono::steady_clock::now().time_since_epoch().count()) ^
                       std::random_device{}();
    std::error_code temp_error;
    const auto temp_dir = std::filesystem::temp_directory_path(temp_error);
    if (temp_error) {
        return {};
    }
    const auto staged = temp_dir / ("lichii-key-install-" + std::to_string(nonce));
    std::error_code error;

    if (!std::filesystem::create_directories(staged, error) || error) {
        return {};
    }
    std::error_code cleanup_error;
    auto cleanup = SCOPE_GUARD { std::filesystem::remove_all(staged, cleanup_error); };
    std::filesystem::permissions(staged, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
    if (error) {
        return {};
    }

    for (const auto& source : source_key_files) {
        std::error_code copy_error;
        if (!std::filesystem::copy_file(source, staged / source.filename(),
                                        std::filesystem::copy_options::overwrite_existing,
                                        copy_error)) {
            return {};
        }
    }
    cleanup.Cancel();
    return staged;
}

bool MergeInstalledProdKeys(const std::filesystem::path& keys_dir,
                            const std::filesystem::path& staged,
                            const std::filesystem::path& prod_key_filename,
                            bool merge_installed_keys) {
    if (!merge_installed_keys) {
        return true;
    }
    const auto installed = keys_dir / prod_key_filename;
    const auto staged_file = staged / prod_key_filename;
    const auto merged = staged / (prod_key_filename.string() + ".merged");
    if (!WriteMergedKeyFile(installed, staged_file, merged)) {
        return false;
    }
    std::error_code error;
    if (!std::filesystem::copy_file(merged, staged_file,
                                    std::filesystem::copy_options::overwrite_existing, error)) {
        return false;
    }
    std::filesystem::remove(merged, error);
    return SetOwnerOnlyPermissions(staged_file);
}

}

FirmwareManager::KeyInstallResult
FirmwareManager::InstallKeys(std::string location, std::string extension) {
    LOG_INFO(Frontend, "Installing key files from {}", location);

    const auto keys_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir);

#ifdef __ANDROID__
    JNIEnv *env = Common::Android::GetEnvForThread();

    jstring jsrc = Common::Android::ToJString(env, location);

    jclass native = Common::Android::GetNativeLibraryClass();
    jmethodID getExtension = Common::Android::GetFileExtension();

    jstring jext = static_cast<jstring>(env->CallStaticObjectMethod(
        native,
        getExtension,
        jsrc
    ));

    std::string ext = Common::Android::GetJString(env, jext);

    if (ext != extension) {
        return ErrorWrongFilename;
    }

    jmethodID copyToStorage = Common::Android::GetCopyToStorage();
    jstring jdest = Common::Android::ToJString(env, keys_dir.string() + "/");

    jboolean copyResult = env->CallStaticBooleanMethod(
        native,
        copyToStorage,
        jsrc,
        jdest
    );

    if (!copyResult) {
        return ErrorFailedCopy;
    }
#else
    if (!location.ends_with(extension)) {
        return ErrorWrongFilename;
    }

    bool prod_keys_found = false;

    const std::filesystem::path prod_key_path = location;
    const std::filesystem::path prod_key_filename = prod_key_path.filename();
    const std::filesystem::path key_source_path = prod_key_path.parent_path();

    if (!Common::FS::IsDir(key_source_path)) {
        return InvalidDir;
    }

    std::vector<std::filesystem::path> source_key_files;

    if (Common::FS::Exists(prod_key_path)) {
        prod_keys_found = true;
        source_key_files.emplace_back(prod_key_path);
    }

    if (Common::FS::Exists(key_source_path / "title.keys")) {
        source_key_files.emplace_back(key_source_path / "title.keys");
    }

    if (Common::FS::Exists(key_source_path / "key_retail.bin")) {
        source_key_files.emplace_back(key_source_path / "key_retail.bin");
    }

    if (source_key_files.empty() || !prod_keys_found) {
        return ErrorWrongFilename;
    }

    bool merge_installed_keys = false;
    std::error_code installed_error;
    const auto installed_prod_key = keys_dir / prod_key_filename;
    if (std::filesystem::exists(installed_prod_key, installed_error)) {
        if (installed_error) {
            return ErrorFailedCopy;
        }
        merge_installed_keys = !PathsReferToSameFile(prod_key_path, installed_prod_key);
    } else if (installed_error) {
        return ErrorFailedCopy;
    }

    const auto staged = StageKeyFiles(source_key_files);
    if (staged.empty()) {
        return ErrorFailedCopy;
    }
    std::error_code cleanup_error;
    SCOPE_EXIT { std::filesystem::remove_all(staged, cleanup_error); };

    auto manager = Core::Crypto::KeyManager::CreateForDirectory(staged);
    if (!manager || manager->BaseDeriveNecessary()) {
        return ErrorFailedInit;
    }
    if (!MergeInstalledProdKeys(keys_dir, staged, prod_key_filename, merge_installed_keys)) {
        return ErrorFailedCopy;
    }
    manager = Core::Crypto::KeyManager::CreateForDirectory(staged);
    if (!manager || manager->BaseDeriveNecessary()) {
        return ErrorFailedInit;
    }

    std::error_code create_error;
    std::filesystem::create_directories(keys_dir, create_error);
    if (create_error) {
        return ErrorFailedCopy;
    }

    for (const auto& key_file : source_key_files) {
        const auto staged_key_file = staged / key_file.filename();
        const auto destination_key_file = keys_dir / key_file.filename();
        std::error_code error;
        if (!std::filesystem::copy_file(staged_key_file,
                                        destination_key_file,
                                        std::filesystem::copy_options::overwrite_existing,
                                        error)) {
            LOG_ERROR(Frontend,
                      "Failed to copy file {} to {}: {}",
                      staged_key_file.string(),
                      destination_key_file.string(),
                      error.message());
            return ErrorFailedCopy;
        }
        if (key_file.filename() == prod_key_filename &&
            !SetOwnerOnlyPermissions(destination_key_file)) {
            return ErrorFailedCopy;
        }
    }
#endif

    // Reinitialize the key manager
    Core::Crypto::KeyManager::Instance().ReloadKeys();

    if (ContentManager::AreKeysPresent()) {
        return Success;
    }

    // Let the frontend handle everything else
    return ErrorFailedInit;
}

FirmwareManager::FirmwareCheckResult FirmwareManager::VerifyFirmware(Core::System &system) {
    if (!CheckFirmwarePresence(system)) {
        return ErrorFirmwareMissing;
    } else {
        const auto pair = GetFirmwareVersion(system);
        const auto result = pair.second;

        if (result.IsError()) {
            LOG_INFO(Frontend, "Unable to read firmware");
            return ErrorFirmwareCorrupted;
        }
    }

    return FirmwareGood;
}
