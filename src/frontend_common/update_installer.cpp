// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend_common/update_installer.h"

#include <fstream>

namespace fs = std::filesystem;

namespace UpdateInstaller {

fs::path StagedPath(const fs::path& target) {
    return fs::path{target}.concat(".new");
}

fs::path PreviousPath(const fs::path& target) {
    return fs::path{target}.concat(".prev");
}

bool CanReplace(const fs::path& target) {
    std::error_code ec;
    if (!fs::is_regular_file(target, ec)) {
        return false;
    }
    const auto probe = target.parent_path() / ".lichii-update-write-test";
    const bool writable = std::ofstream{probe}.is_open();
    fs::remove(probe, ec);
    return writable;
}

bool Swap(const fs::path& staged, const fs::path& target, std::error_code& ec) {
    const auto previous = PreviousPath(target);
    const auto permissions = fs::status(target, ec).permissions();
    if (ec) {
        return false;
    }
    fs::permissions(staged,
                    permissions | fs::perms::owner_exec | fs::perms::group_exec |
                        fs::perms::others_exec,
                    ec);
    if (ec) {
        return false;
    }
    fs::remove(previous, ec);
    if (ec) {
        return false;
    }
    fs::rename(target, previous, ec);
    if (ec) {
        return false;
    }
    fs::rename(staged, target, ec);
    if (ec) {
        std::error_code restore_ec;
        fs::rename(previous, target, restore_ec);
        return false;
    }
    return true;
}

bool Rollback(const fs::path& target, std::error_code& ec) {
    const auto previous = PreviousPath(target);
    const auto scratch = StagedPath(target);
    if (!fs::is_regular_file(previous, ec)) {
        if (!ec) {
            ec = std::make_error_code(std::errc::no_such_file_or_directory);
        }
        return false;
    }
    fs::remove(scratch, ec);
    if (ec) {
        return false;
    }
    fs::rename(previous, scratch, ec);
    if (ec) {
        return false;
    }
    fs::rename(target, previous, ec);
    if (ec) {
        std::error_code restore_ec;
        fs::rename(scratch, previous, restore_ec);
        return false;
    }
    fs::rename(scratch, target, ec);
    if (ec) {
        std::error_code restore_ec;
        fs::rename(previous, target, restore_ec);
        return false;
    }
    return true;
}

std::string StripPathListEntries(std::string_view list, std::string_view dir) {
    if (dir.empty()) {
        return std::string{list};
    }
    std::string result;
    while (!list.empty()) {
        const auto end = list.find(':');
        const auto entry = list.substr(0, end);
        list = end == std::string_view::npos ? std::string_view{} : list.substr(end + 1);
        const bool inside = entry == dir || (entry.starts_with(dir) && entry[dir.size()] == '/');
        if (entry.empty() || inside) {
            continue;
        }
        if (!result.empty()) {
            result += ':';
        }
        result += entry;
    }
    return result;
}

} // namespace UpdateInstaller
