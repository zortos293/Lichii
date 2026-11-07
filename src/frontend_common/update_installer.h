// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace UpdateInstaller {

// Where a download is staged before it replaces `target`; always beside it, so the final step is
// a rename on one filesystem.
std::filesystem::path StagedPath(const std::filesystem::path& target);

// Where the replaced file is kept for one rollback.
std::filesystem::path PreviousPath(const std::filesystem::path& target);

// Whether a file can be replaced in place: it exists and its folder accepts new files.
bool CanReplace(const std::filesystem::path& target);

// Replace `target` with `staged`, keeping the old file at PreviousPath(target). The new file takes
// the old file's permissions and is executable. On failure `target` is left as it was.
bool Swap(const std::filesystem::path& staged, const std::filesystem::path& target,
          std::error_code& ec);

// Exchange `target` and PreviousPath(target), so rolling back can itself be undone.
bool Rollback(const std::filesystem::path& target, std::error_code& ec);

// Remove the entries of a ':' separated list that are `dir` or lie below it.
std::string StripPathListEntries(std::string_view list, std::string_view dir);

} // namespace UpdateInstaller
