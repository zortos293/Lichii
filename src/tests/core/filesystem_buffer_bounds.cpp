// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/service/filesystem/fsp/fs_i_file.h"
#include "core/hle/service/filesystem/fsp/fs_i_storage.h"
#include "tests/core/system_test_context.h"

namespace Service::FileSystem {
struct FileIoTestAccess {
    static constexpr int Attributes = BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure;

    static Result Read(IFile& file, std::span<u8> output, s64 size, s64& read) {
        return file.Read(FileSys::ReadOption::None, &read, 0, OutBuffer<Attributes>{output}, size);
    }
    static Result Write(IFile& file, std::span<const u8> input, s64 size) {
        return file.Write(InBuffer<Attributes>{input}, FileSys::WriteOption::None, 0, size);
    }
    static Result Read(IStorage& storage, std::span<u8> output, s64 size) {
        return storage.Read(OutBuffer<Attributes>{output}, 0, size);
    }
};
} // namespace Service::FileSystem

TEST_CASE("Filesystem requests respect the supplied IPC buffer extent", "[core][filesystem_bounds]") {
    // The full backing array is deliberately larger than the supplied span. A broken handler
    // changes the canary bytes without making this regression itself access invalid host memory.
    ScopedSystemTestContext context;
    Core::System system;
    auto data = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>{1, 2, 3, 4, 5, 6, 7, 8});
    Service::FileSystem::IFile file{system, data};
    Service::FileSystem::IStorage storage{system, data};
    using Access = Service::FileSystem::FileIoTestAccess;
    std::array<u8, 8> output;
    output.fill(0xa5);
    const auto short_output = std::span{output}.first(4);
    s64 read = -1;

    SECTION("file read rejects a length larger than its output") {
        CHECK(Access::Read(file, short_output, 8, read) == FileSys::ResultInvalidSize);
        CHECK(std::ranges::all_of(output, [](u8 value) { return value == 0xa5; }));
    }
    SECTION("storage read rejects a length larger than its output") {
        CHECK(Access::Read(storage, short_output, 8) == FileSys::ResultInvalidSize);
        CHECK(std::ranges::all_of(output, [](u8 value) { return value == 0xa5; }));
    }
    SECTION("file write rejects a length larger than its input") {
        CHECK(Access::Write(file, short_output, 8) == FileSys::ResultInvalidSize);
        CHECK(data->ReadAllBytes() == std::vector<u8>{1, 2, 3, 4, 5, 6, 7, 8});
    }
    SECTION("valid file read stays inside the output") {
        CHECK(Access::Read(file, short_output, 4, read) == ResultSuccess);
        CHECK(read == 4);
        CHECK(output == std::array<u8, 8>{1, 2, 3, 4, 0xa5, 0xa5, 0xa5, 0xa5});
    }
    SECTION("valid storage read stays inside the output") {
        CHECK(Access::Read(storage, short_output, 4) == ResultSuccess);
        CHECK(output == std::array<u8, 8>{1, 2, 3, 4, 0xa5, 0xa5, 0xa5, 0xa5});
    }
    SECTION("valid file write stays inside the input") {
        CHECK(Access::Write(file, short_output, 4) == ResultSuccess);
        CHECK(data->ReadAllBytes() == std::vector<u8>{0xa5, 0xa5, 0xa5, 0xa5, 5, 6, 7, 8});
    }
    SECTION("empty requests remain valid") {
        CHECK(Access::Read(file, {}, 0, read) == ResultSuccess);
        CHECK(read == 0);
        CHECK(Access::Read(storage, {}, 0) == ResultSuccess);
        CHECK(Access::Write(file, {}, 0) == ResultSuccess);
    }
    SECTION("negative lengths are rejected") {
        CHECK(Access::Read(file, short_output, -1, read) == FileSys::ResultInvalidSize);
        CHECK(Access::Read(storage, short_output, -1) == FileSys::ResultInvalidSize);
        CHECK(Access::Write(file, short_output, -1) == FileSys::ResultInvalidSize);
    }
};
