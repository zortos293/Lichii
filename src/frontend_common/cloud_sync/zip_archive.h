// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace FrontendCommon::CloudSync {

struct ZipEntry {
    // Separated by '/'. A trailing '/' marks a directory, which has no data.
    std::string name;
    std::vector<u8> data;

    bool operator==(const ZipEntry&) const = default;
};

// Whether an entry of size bytes needs ZIP64 sizes: when deflating it could take 4 GiB or more
// (zlib's compressBound). minizip fails an entry whose sizes don't fit.
bool NeedsZip64(u64 size);

// A zip of the entries in the given order, deflated and with a fixed timestamp, so the same
// entries always make the same bytes. nullopt when minizip fails.
std::optional<std::vector<u8>> WriteZip(std::span<const ZipEntry> entries);

// The zip's entries in its order. nullopt when it is not a zip minizip can read, or an entry
// fails its CRC.
std::optional<std::vector<ZipEntry>> ReadZip(std::span<const u8> zip);

} // namespace FrontendCommon::CloudSync
