// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <openssl/evp.h>
#include <zstd.h>
#ifdef __linux__
#include <sys/stat.h>
#endif

#include "common/zstd_compression.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace {

namespace fs = std::filesystem;
using FileSys::NupContainer;

struct TempDir {
    // The counter keeps two directories made within one clock tick apart.
    inline static std::atomic<unsigned> made{};
    fs::path path = fs::temp_directory_path() /
                    ("nup-read-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     "-" + std::to_string(made++));
    TempDir() {
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void Put(std::vector<u8>& bytes, std::size_t offset, u64 value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        bytes[offset + i] = static_cast<u8>(value >> (8 * i));
    }
}

// Compressible bytes: literals from a skewed distribution and copies of recent data.
std::vector<u8> Content(std::size_t size, u64 seed) {
    std::vector<u8> out(size);
    std::mt19937_64 rng{seed};
    std::size_t pos = 0;
    while (pos < size) {
        const u64 r = rng();
        const std::size_t length = std::min<std::size_t>(8 + (r & 127), size - pos);
        if (pos > 1024 && (r >> 8) % 3 != 0) {
            const std::size_t back = 1 + (r >> 16) % std::min<std::size_t>(pos, 256 * 1024);
            for (std::size_t i = 0; i < length; ++i) {
                out[pos + i] = out[pos - back + i];
            }
        } else {
            for (std::size_t i = 0; i < length; ++i) {
                const u64 x = rng();
                out[pos + i] = static_cast<u8>(x & (x >> 8));
            }
        }
        pos += length;
    }
    return out;
}

std::vector<u8> Zstd(std::span<const u8> data) {
    return Common::Compression::CompressDataZSTD(data.data(), data.size(), 3);
}

struct Member {
    std::vector<u8> extent;
    u64 size;
    u8 codec;
    std::string name{};
    std::array<u8, 0x20> sha256{};
};

std::array<u8, 0x20> Sha256(std::span<const u8> data) {
    std::array<u8, 0x20> hash{};
    unsigned int length{};
    EVP_Digest(data.data(), data.size(), hash.data(), &length, EVP_sha256(), nullptr);
    return hash;
}

Member ZstdMember(const std::vector<u8>& content) {
    return {Zstd(content), content.size(), 1};
}

// A single-package container whose root directory holds the members as files 0..n-1.
std::vector<u8> BuildNup(const std::vector<Member>& members) {
    constexpr std::size_t TocOffset = 0x80;
    const std::size_t files_at = 0x30 + 0x40 + 0x18;
    const std::size_t children_at = files_at + members.size() * 0x50;
    const std::size_t pool_at = children_at + members.size() * 4;
    std::string pool;
    for (const auto& member : members) {
        pool += member.name;
    }
    const std::size_t toc_size = pool_at + pool.size();
    const std::size_t blob_offset = (TocOffset + toc_size + 15) & ~std::size_t{15};
    std::vector<u8> bytes(blob_offset);
    std::vector<u64> extent_offsets;
    for (const auto& member : members) {
        bytes.resize((bytes.size() + 15) & ~std::size_t{15});
        // An empty member has no extent: its offset is zero, as the reader requires.
        extent_offsets.push_back(member.extent.empty() && member.size == 0 ? 0 : bytes.size());
        bytes.insert(bytes.end(), member.extent.begin(), member.extent.end());
    }

    std::copy(NupContainer::HeaderMagic.begin(), NupContainer::HeaderMagic.end(), bytes.begin());
    Put(bytes, 0x04, 0x80, 2);
    Put(bytes, 0x08, TocOffset, 8);
    Put(bytes, 0x10, toc_size, 8);
    Put(bytes, 0x38, blob_offset, 8);
    Put(bytes, 0x40, bytes.size(), 8);
    Put(bytes, 0x48, 1, 4);
    const std::size_t toc = TocOffset;
    Put(bytes, toc + 0x00, 0x01434F54, 4);
    Put(bytes, toc + 0x04, 1, 4);
    Put(bytes, toc + 0x08, 1, 8);
    Put(bytes, toc + 0x10, members.size(), 8);
    Put(bytes, toc + 0x18, pool_at, 8);
    Put(bytes, toc + 0x20, pool.size(), 8);
    std::copy(pool.begin(), pool.end(), bytes.begin() + toc + pool_at);
    Put(bytes, toc + 0x30 + 0x0C, 0xFFFFFFFF, 4);
    Put(bytes, toc + 0x30 + 0x40 + 0x0C, members.size(), 4);
    std::size_t name_offset = 0;
    for (std::size_t i = 0; i < members.size(); ++i) {
        const std::size_t record = toc + files_at + i * 0x50;
        Put(bytes, record + 0x00, name_offset, 4);
        Put(bytes, record + 0x04, members[i].name.size(), 4);
        name_offset += members[i].name.size();
        Put(bytes, record + 0x08, members[i].size, 8);
        Put(bytes, record + 0x10, extent_offsets[i], 8);
        Put(bytes, record + 0x18, members[i].extent.size(), 8);
        std::copy(members[i].sha256.begin(), members[i].sha256.end(), bytes.begin() + record + 0x20);
        bytes[record + 0x40] = members[i].codec;
        Put(bytes, toc + children_at + i * 4, 1 + i, 4);
    }
    unsigned int length{};
    EVP_Digest(bytes.data() + toc, toc_size, bytes.data() + 0x18, &length, EVP_sha256(), nullptr);
    return bytes;
}

FileSys::VirtualFile Backing(std::vector<u8> bytes) {
    return std::make_shared<FileSys::VectorVfsFile>(std::move(bytes));
}

bool ReadMatches(const FileSys::VirtualFile& file, const std::vector<u8>& content,
                 std::size_t offset, std::size_t length) {
    std::vector<u8> buffer(length);
    const std::size_t expected =
        std::min(length, content.size() - std::min(offset, content.size()));
    return file->Read(buffer.data(), length, offset) == expected &&
           std::equal(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(expected),
                      content.begin() + static_cast<std::ptrdiff_t>(offset));
}

std::size_t DirectoryBytes(const fs::path& dir) {
    std::size_t total = 0;
    // Windows directory entries can report a stale size for a file that is still open.
    for (const auto& entry : fs::directory_iterator(dir)) {
        std::ifstream stream{entry.path(), std::ios::binary | std::ios::ate};
        total += static_cast<std::size_t>(stream.tellg());
    }
    return total;
}

// POSIX unlinks a spill file once it is open, so its bytes are counted through the descriptors
// that still hold it. Other POSIX systems have no way to see them.
#if defined(_WIN32) || defined(__linux__)
std::size_t SpillBytes(const fs::path& dir) {
    std::size_t total = DirectoryBytes(dir);
#ifdef __linux__
    std::error_code ec;
    const auto prefix = fs::canonical(dir, ec).string();
    for (const auto& fd : fs::directory_iterator("/proc/self/fd", ec)) {
        const auto target = fs::read_symlink(fd.path(), ec).string();
        if (target.starts_with(prefix) && target.ends_with(" (deleted)")) {
            const auto size = fs::file_size(fd.path(), ec);
            total += ec ? 0 : static_cast<std::size_t>(size);
        }
    }
#endif
    return total;
}
#endif

#if defined(_WIN32) || defined(__linux__)
/// The container's spill files under `dir`, each as its identity and size: on Linux the unlinked
/// ones still held open (their descriptor's target, which names the file and its inode is held),
/// with any named ones; elsewhere the named ones.
std::vector<std::pair<std::string, std::uintmax_t>> SpillFiles(const fs::path& dir) {
    std::vector<std::pair<std::string, std::uintmax_t>> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        files.emplace_back(entry.path().filename().string(), entry.file_size(ec));
    }
#ifdef __linux__
    const auto prefix = fs::canonical(dir, ec).string();
    for (const auto& fd : fs::directory_iterator("/proc/self/fd", ec)) {
        const auto target = fs::read_symlink(fd.path(), ec).string();
        if (target.starts_with(prefix) && target.ends_with(" (deleted)")) {
            struct stat st{};
            const std::string inode =
                stat(fd.path().c_str(), &st) == 0 ? std::to_string(st.st_ino) : target;
            files.emplace_back("inode " + inode, fs::file_size(fd.path(), ec));
        }
    }
#endif
    std::ranges::sort(files);
    return files;
}
#endif

} // namespace

TEST_CASE("NUP zstd random reads match the source in memory and spilled", "[nup][nup-read]") {
    const std::vector<std::size_t> sizes{1, 100, 64 * 1024, 300 * 1024, 3 * 1024 * 1024};
    std::vector<std::vector<u8>> contents;
    std::vector<Member> members;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        contents.push_back(Content(sizes[i], i + 1));
        members.push_back(ZstdMember(contents.back()));
    }
    const auto bytes = BuildNup(members);
    TempDir t;
    // 64 MiB keeps every member in memory; 128 KiB spills those above 64 KiB.
    for (const std::size_t cap : {std::size_t{64} << 20, std::size_t{128} << 10}) {
        NupContainer nup{Backing(bytes), cap, t.path};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        u8 byte{};
        REQUIRE(nup.OpenFile(4)->Read(&byte, 0, 10) == 0);
        std::mt19937_64 rng{cap};
        for (int i = 0; i < 400; ++i) {
            const std::size_t member = rng() % contents.size();
            const auto& content = contents[member];
            const std::size_t offset = rng() % (content.size() + 1);
            const std::size_t length = 1 + rng() % (200 * 1024);
            REQUIRE(ReadMatches(nup.OpenFile(member), content, offset, length));
        }
        for (std::size_t i = 0; i < contents.size(); ++i) {
            REQUIRE(nup.OpenFile(i)->ReadAllBytes() == contents[i]);
        }
    }
}

TEST_CASE("NUP zstd reads are safe across threads and evictions", "[nup][nup-read]") {
    std::vector<std::vector<u8>> contents;
    std::vector<Member> members;
    for (int i = 0; i < 6; ++i) {
        contents.push_back(Content(std::size_t{1} << 20, 100 + i));
        members.push_back(ZstdMember(contents.back()));
    }
    const auto bytes = BuildNup(members);
    TempDir t;
    // 4 MiB holds three 1 MiB members at a time; 1 MiB spills them all.
    for (const std::size_t cap : {std::size_t{4} << 20, std::size_t{1} << 20}) {
        NupContainer nup{Backing(bytes), cap, t.path};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        std::vector<FileSys::VirtualFile> files;
        for (std::size_t i = 0; i < contents.size(); ++i) {
            files.push_back(nup.OpenFile(i));
        }
        std::atomic<int> mismatches{};
        std::vector<std::thread> threads;
        for (unsigned worker = 0; worker < 8; ++worker) {
            threads.emplace_back([&, worker] {
                std::mt19937_64 rng{worker * 7919 + cap};
                if (worker % 2 == 0) {
                    // Streams that decode the member a little at a time, racing each other.
                    const std::size_t member = worker / 2;
                    for (std::size_t offset = 0; offset < contents[member].size();
                         offset += 8 * 1024) {
                        mismatches += !ReadMatches(files[member], contents[member], offset, 8192);
                    }
                    return;
                }
                for (int i = 0; i < 300; ++i) {
                    const std::size_t member = rng() % contents.size();
                    const std::size_t offset = rng() % contents[member].size();
                    const std::size_t length = 1 + rng() % (64 * 1024);
                    mismatches += !ReadMatches(files[member], contents[member], offset, length);
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        REQUIRE(mismatches == 0);
    }
}

TEST_CASE("NUP zstd members of several frames decode in memory and spilled", "[nup][nup-read]") {
    const auto first = Content(200 * 1024, 7);
    const auto second = Content(300 * 1024, 8);
    std::vector<u8> content = first;
    content.insert(content.end(), second.begin(), second.end());
    Member member{Zstd(first), content.size(), 1};
    const auto tail = Zstd(second);
    member.extent.insert(member.extent.end(), tail.begin(), tail.end());
    const auto bytes = BuildNup({member});
    TempDir t;
    for (const std::size_t cap : {std::size_t{64} << 20, std::size_t{64} << 10}) {
        NupContainer nup{Backing(bytes), cap, t.path};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        REQUIRE(ReadMatches(nup.OpenFile(0), content, content.size() - 5000, 5000));
        REQUIRE(nup.OpenFile(0)->ReadAllBytes() == content);
    }
}

TEST_CASE("NUP zstd spill file grows only as far as reads need", "[nup][nup-read]") {
    const auto content = Content(8 << 20, 3);
    const auto bytes = BuildNup({ZstdMember(content)});
    TempDir t;
    NupContainer nup{Backing(bytes), 1 << 20, t.path};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    const auto file = nup.OpenFile(0);
    REQUIRE(ReadMatches(file, content, 0, 4096));
#if defined(_WIN32) || defined(__linux__)
    REQUIRE(SpillBytes(t.path) < content.size() / 4);
#endif
    REQUIRE(ReadMatches(file, content, content.size() - 4096, 4096));
#if defined(_WIN32) || defined(__linux__)
    REQUIRE(SpillBytes(t.path) == content.size());
#endif
}

TEST_CASE("NUP zstd reads fail cleanly on corrupt or truncated extents", "[nup][nup-read]") {
    const auto content = Content(2 << 20, 5);
    const auto good = ZstdMember(content);
    TempDir t;
    for (const std::size_t cap : {std::size_t{64} << 20, std::size_t{64} << 10}) {
        DYNAMIC_SECTION("truncated, cap " << cap) {
            Member truncated = good;
            truncated.extent.resize(truncated.extent.size() / 2);
            NupContainer nup{Backing(BuildNup({truncated})), cap, t.path};
            REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
            const auto file = nup.OpenFile(0);
            // The intact prefix still reads; anything past the cut fails every time.
            REQUIRE(ReadMatches(file, content, 1000, 4096));
            std::vector<u8> buffer(4096);
            for (int attempt = 0; attempt < 2; ++attempt) {
                REQUIRE(file->Read(buffer.data(), buffer.size(), content.size() - 4096) == 0);
            }
            REQUIRE(file->ReadAllBytes().empty());
            REQUIRE(nup.VerifyIntegrity({}) ==
                    Loader::ResultStatus::ErrorIntegrityVerificationFailed);
        }
        DYNAMIC_SECTION("bad frame header, cap " << cap) {
            Member corrupt = good;
            std::fill_n(corrupt.extent.begin(), 8, u8{0xA5});
            NupContainer nup{Backing(BuildNup({corrupt})), cap, t.path};
            REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
            std::vector<u8> buffer(4096);
            REQUIRE(nup.OpenFile(0)->Read(buffer.data(), buffer.size(), 0) == 0);
        }
        DYNAMIC_SECTION("recorded size smaller than the frame, cap " << cap) {
            Member oversized = good;
            oversized.size = content.size() - 1000;
            NupContainer nup{Backing(BuildNup({oversized})), cap, t.path};
            REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
            REQUIRE(nup.OpenFile(0)->ReadAllBytes().empty());
        }
        DYNAMIC_SECTION("recorded size ending on a decoder output block, cap " << cap) {
            // zstd hands spilled output over in 128 KiB blocks, so the recorded size is reached
            // exactly at a block end while the frame still holds more.
            Member oversized = good;
            oversized.size = content.size() - 128 * 1024;
            NupContainer nup{Backing(BuildNup({oversized})), cap, t.path};
            REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
            const auto file = nup.OpenFile(0);
            std::vector<u8> buffer(4096);
            REQUIRE(file->Read(buffer.data(), buffer.size(), oversized.size - 1) == 0);
            REQUIRE(file->ReadAllBytes().empty());
        }
        DYNAMIC_SECTION("recorded size larger than the frame, cap " << cap) {
            Member undersized = good;
            undersized.size = content.size() + 1000;
            NupContainer nup{Backing(BuildNup({undersized})), cap, t.path};
            REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
            REQUIRE(nup.OpenFile(0)->ReadAllBytes().empty());
        }
    }
}

TEST_CASE("NUP zstd spilled members resume after their decoder is freed", "[nup][nup-read]") {
    std::vector<std::vector<u8>> contents;
    std::vector<Member> members;
    for (int i = 0; i < 5; ++i) {
        contents.push_back(Content(std::size_t{2} << 20, 200 + i));
        members.push_back(ZstdMember(contents.back()));
    }
    TempDir t;
    NupContainer nup{Backing(BuildNup(members)), 1 << 20, t.path};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    // Starting every member leaves only the newest decoders alive; the older members decode
    // again from their first frame when read further.
    for (int round = 0; round < 3; ++round) {
        for (std::size_t i = 0; i < contents.size(); ++i) {
            const std::size_t offset = static_cast<std::size_t>(round) * 512 * 1024;
            REQUIRE(ReadMatches(nup.OpenFile(i), contents[i], offset, 4096));
        }
    }
    for (std::size_t i = 0; i < contents.size(); ++i) {
        REQUIRE(nup.OpenFile(i)->ReadAllBytes() == contents[i]);
    }
}

TEST_CASE("NUP serves a sound sibling in place of a .zs member with no zstd header",
          "[nup][nup-read]") {
    const auto zstd_file = [](u64 seed) { return Zstd(Content(200 * 1024, seed)); };
    auto damaged = zstd_file(1);
    std::fill_n(damaged.begin(), 12288, u8{0});
    const auto stand_in = zstd_file(2);
    const auto other_prefix = zstd_file(3);
    const auto other_kind = zstd_file(4);
    auto lonely = zstd_file(5);
    std::fill_n(lonely.begin(), 12288, u8{0});
    const std::vector<u8> zeros(4096, 0);
    Member unreadable = ZstdMember(zstd_file(6));
    std::fill_n(unreadable.extent.begin(), 8, u8{0xA5});

    const auto named = [](Member member, std::string name) {
        member.name = std::move(name);
        return member;
    };
    const std::vector<Member> members{
        named(ZstdMember(damaged), "Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs"),
        named(ZstdMember(stand_in), "Rock_WaterTemple_Gate_P_01_Nrm.bntx.zs"),
        named({other_prefix, other_prefix.size(), 0}, "Rock_WaterTemple_Wall_A_01_Nrm.bntx.zs"),
        named(ZstdMember(other_kind), "Rock_WaterTemple_Gate_M_01_Alb.bntx.zs"),
        named(ZstdMember(lonely), "Lonely_Mask.bntx.zs"),
        named(unreadable, "Broken_A_01_Nrm.bntx.zs"),
        named(ZstdMember(zeros), "Model.bfres"),
    };
    TempDir t;
    NupContainer nup{Backing(BuildNup(members)), std::size_t{64} << 20, t.path};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    const auto dir = nup.GetRomFSDir(0);
    REQUIRE(dir != nullptr);
    REQUIRE(dir->GetFiles().size() == members.size());
    const auto read = [&](std::string_view name) {
        const auto file = dir->GetFile(name);
        REQUIRE(file != nullptr);
        REQUIRE(file->GetName() == name);
        return file->ReadAllBytes();
    };

    // Same last name token, then the longest common prefix.
    REQUIRE(read("Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs") == stand_in);
    REQUIRE(read("Rock_WaterTemple_Gate_P_01_Nrm.bntx.zs") == stand_in);
    REQUIRE(read("Rock_WaterTemple_Wall_A_01_Nrm.bntx.zs") == other_prefix);
    REQUIRE(read("Rock_WaterTemple_Gate_M_01_Alb.bntx.zs") == other_kind);
    // Without a sibling of its kind a damaged member is served as stored.
    REQUIRE(read("Lonely_Mask.bntx.zs") == lonely);
    // A member that cannot be decoded is not known to be damaged, so it is not replaced.
    REQUIRE(read("Broken_A_01_Nrm.bntx.zs").empty());
    REQUIRE(read("Model.bfres") == zeros);

    // Extraction reads the package tree, which keeps the stored bytes.
    const auto package = nup.GetPackageDir(0);
    REQUIRE(package != nullptr);
    REQUIRE(package->GetFile("Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs")->ReadAllBytes() == damaged);
}

TEST_CASE("Verifying a NUP hashes its members as it reads them", "[nup][nup-read]") {
    // Stored and compressed members, one of exactly the 1 MiB input chunk and one of a chunk and
    // a part, through the same verify whatever the cache size.
    constexpr std::size_t Chunk = 1 << 20;
    const auto exact = Content(Chunk, 11);
    const auto longer = Content(Chunk * 5 + 12345, 12);
    const std::vector<Member> members{
        {exact, exact.size(), 0, {}, Sha256(exact)},
        {longer, longer.size(), 0, {}, Sha256(longer)},
        {Zstd(exact), exact.size(), 1, {}, Sha256(exact)},
        {Zstd(longer), longer.size(), 1, {}, Sha256(longer)},
    };
    std::size_t total = 0;
    for (const auto& member : members) {
        total += member.extent.size();
    }
    TempDir t;
    for (const std::size_t cap : {std::size_t{64} << 20, std::size_t{64} << 10}) {
        CAPTURE(cap);
        NupContainer nup{Backing(BuildNup(members)), cap, t.path};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        std::vector<std::pair<std::size_t, std::size_t>> reports;
        REQUIRE(nup.VerifyIntegrity([&](std::size_t done, std::size_t all) {
            reports.emplace_back(done, all);
            return true;
        }) == Loader::ResultStatus::Success);
        // In extent bytes, out of the extents' total, never going back, ending at the total: at
        // least once at each member's end and inside the stored 5 MiB one.
        REQUIRE(reports.size() >= members.size() + 1);
        REQUIRE(std::ranges::is_sorted(reports));
        REQUIRE(std::ranges::all_of(reports, [&](const auto& r) { return r.second == total; }));
        REQUIRE(reports.back() == std::pair{total, total});

        // A cancel stops it at the report it answered.
        std::size_t calls{};
        REQUIRE(nup.VerifyIntegrity([&](std::size_t, std::size_t) { return ++calls < 2; }) ==
                Loader::ResultStatus::ErrorIntegrityVerificationFailed);
        REQUIRE(calls == 2);
    }

    // A changed byte past the first chunk is found.
    auto changed = members;
    changed[1].extent[Chunk + 100] ^= 1;
    NupContainer nup{Backing(BuildNup(changed)), std::size_t{64} << 20, t.path};
    REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::ErrorIntegrityVerificationFailed);
}

TEST_CASE("Verifying a NUP of empty members reports nothing with a total of zero",
          "[nup][nup-read]") {
    // The desktop frontend divides by the total it is given.
    const Member empty{{}, 0, 0, {}, Sha256({})};
    NupContainer nup{Backing(BuildNup({empty, empty}))};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    bool zero_total{};
    REQUIRE(nup.VerifyIntegrity([&](std::size_t, std::size_t total) {
        zero_total |= total == 0;
        return true;
    }) == Loader::ResultStatus::Success);
    REQUIRE_FALSE(zero_total);
}

TEST_CASE("Verifying a NUP polls after an empty member as after any other", "[nup][nup-read]") {
    // One stored byte, then an empty member: the first poll comes at the byte's end, the second
    // after the empty member, with the same value, and a cancel there is a cancel.
    const std::vector<u8> one{42};
    const Member stored{one, 1, 0, {}, Sha256(one)};
    const Member empty{{}, 0, 0, {}, Sha256({})};
    NupContainer nup{Backing(BuildNup({stored, empty}))};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    std::vector<std::pair<std::size_t, std::size_t>> reports;
    REQUIRE(nup.VerifyIntegrity([&](std::size_t done, std::size_t all) {
        reports.emplace_back(done, all);
        return reports.size() < 2;
    }) == Loader::ResultStatus::ErrorIntegrityVerificationFailed);
    REQUIRE(reports == std::vector<std::pair<std::size_t, std::size_t>>{{1, 1}, {1, 1}});
}

TEST_CASE("Verifying a NUP checks a compressed member's frames against its size",
          "[nup][nup-read]") {
    const auto first = Content(200 * 1024, 7);
    const auto second = Content(300 * 1024, 8);
    std::vector<u8> content = first;
    content.insert(content.end(), second.begin(), second.end());
    const auto head = Zstd(first);
    const auto tail = Zstd(second);
    Member member{head, content.size(), 1, {}, Sha256(content)};
    member.extent.insert(member.extent.end(), tail.begin(), tail.end());
    const auto verify = [](const Member& m) {
        TempDir t;
        NupContainer nup{Backing(BuildNup({m})), std::size_t{64} << 20, t.path};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        return nup.VerifyIntegrity({});
    };
    // Concatenated frames are one stream.
    CHECK(verify(member) == Loader::ResultStatus::Success);
    // The second frame cut short: the data ends inside a frame.
    Member truncated = member;
    truncated.extent.resize(head.size() + tail.size() / 2);
    CHECK(verify(truncated) == Loader::ResultStatus::ErrorIntegrityVerificationFailed);
    // Bytes after the last frame are read as the next one, which is not a frame (the cache's
    // reader fails on them too).
    Member trailing = member;
    trailing.extent.insert(trailing.extent.end(), {1, 2, 3, 4, 5, 6, 7, 8});
    CHECK(verify(trailing) == Loader::ResultStatus::ErrorIntegrityVerificationFailed);
    // A recorded size larger than the frames hold: the data ends short.
    Member shorter = member;
    shorter.size = content.size() + 1000;
    CHECK(verify(shorter) == Loader::ResultStatus::ErrorIntegrityVerificationFailed);
    // A recorded size smaller than the frames hold: the data runs past it.
    Member longer = member;
    longer.size = content.size() - 1000;
    CHECK(verify(longer) == Loader::ResultStatus::ErrorIntegrityVerificationFailed);
}

TEST_CASE("Verifying a NUP accepts a frame with a large window exactly when reading does",
          "[nup][nup-read]") {
    // A frame declaring a 256 MiB window and no content size, made by hand: the stream decoder
    // refuses it (zstd's default limit is 128 MiB), so verification hashes the member through the
    // reader, which reads it when it decodes the member in memory and refuses it when it spills.
    std::vector<u8> content(64 * 1024);
    std::mt19937 rng{16};
    std::ranges::generate(content, [&] { return static_cast<u8>(rng()); });
    std::vector<u8> frame{0x28, 0xB5, 0x2F, 0xFD, // magic
                          0x00,                   // no content size, not single segment
                          18 << 3};               // window descriptor: 2^(10 + 18)
    const u32 block = static_cast<u32>(content.size() << 3) | 1; // last block, raw
    frame.insert(frame.end(), {static_cast<u8>(block), static_cast<u8>(block >> 8),
                               static_cast<u8>(block >> 16)});
    frame.insert(frame.end(), content.begin(), content.end());
    REQUIRE(ZSTD_getFrameContentSize(frame.data(), frame.size()) == ZSTD_CONTENTSIZE_UNKNOWN);
    {
        // Refused at the frame header by a decoder at the default limit, before any window.
        std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> dctx{ZSTD_createDCtx(),
                                                                  &ZSTD_freeDCtx};
        std::vector<u8> scratch(16);
        ZSTD_outBuffer out{scratch.data(), scratch.size(), 0};
        ZSTD_inBuffer in{frame.data(), frame.size(), 0};
        REQUIRE(ZSTD_isError(ZSTD_decompressStream(dctx.get(), &out, &in)));
    }
    const Member sound{frame, content.size(), 1, {}, Sha256(content)};
    Member flipped = sound;
    flipped.extent[1000] ^= 1;
    TempDir t;
    // Decoded in memory: read, so verified, and a changed byte is found.
    {
        NupContainer nup{Backing(BuildNup({sound})), std::size_t{64} << 20, t.path};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        REQUIRE(ReadMatches(nup.OpenFile(0), content, 0, content.size()));
        REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);
        NupContainer changed{Backing(BuildNup({flipped})), std::size_t{64} << 20, t.path};
        REQUIRE(changed.VerifyIntegrity({}) ==
                Loader::ResultStatus::ErrorIntegrityVerificationFailed);
    }
    // Spilled: the reader keeps the default limit and refuses it, and so does verification.
    {
        NupContainer nup{Backing(BuildNup({sound})), std::size_t{64} << 10, t.path};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        REQUIRE(nup.OpenFile(0)->ReadAllBytes().empty());
        REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::ErrorIntegrityVerificationFailed);
    }
}

TEST_CASE("Verifying a NUP through the reader leaves the game's cache as it was",
          "[nup][nup-read]") {
    // A backing file that counts its reads: a game member still cached is read without it.
    struct CountingFile : FileSys::VectorVfsFile {
        using VectorVfsFile::VectorVfsFile;
        std::size_t Read(u8* out, std::size_t length, std::size_t offset) const override {
            ++reads;
            return VectorVfsFile::Read(out, length, offset);
        }
        mutable std::atomic<std::size_t> reads{};
    };
    // A frame declaring a 256 MiB window, in raw blocks of at most 128 KiB, which only the reader
    // decodes, and only in memory.
    const auto large_window = [](const std::vector<u8>& content) {
        std::vector<u8> frame{0x28, 0xB5, 0x2F, 0xFD, 0x00, 18 << 3};
        for (std::size_t at = 0; at < content.size();) {
            const std::size_t size = std::min<std::size_t>(128 * 1024, content.size() - at);
            const bool last = at + size == content.size();
            const u32 block = static_cast<u32>(size << 3) | (last ? 1u : 0u);
            frame.insert(frame.end(), {static_cast<u8>(block), static_cast<u8>(block >> 8),
                                       static_cast<u8>(block >> 16)});
            frame.insert(frame.end(), content.begin() + static_cast<std::ptrdiff_t>(at),
                         content.begin() + static_cast<std::ptrdiff_t>(at + size));
            at += size;
        }
        return frame;
    };
    const auto member_of = [](const std::vector<u8>& content) {
        Member member = ZstdMember(content);
        member.sha256 = Sha256(content);
        return member;
    };
    SECTION("in memory") {
        // 2 MiB holds the game's two 700 KiB members, but not those and a third 700 KiB one even
        // with no decoder charged: read through the game's cache, the retry would evict one.
        const auto first = Content(700 * 1024, 41);
        const auto second = Content(700 * 1024, 43);
        const auto wide = Content(700 * 1024, 42);
        const Member wide_member{large_window(wide), wide.size(), 1, {}, Sha256(wide)};
        // After the wide member, whose retry frees the stream decoder, a stored member and a
        // compressed one, which verifies with the decoder made again.
        const auto stored = Content(4096, 45);
        const auto after = Content(300 * 1024, 46);
        const Member stored_member{stored, stored.size(), 0, {}, Sha256(stored)};
        const auto backing = std::make_shared<CountingFile>(BuildNup(
            {member_of(first), member_of(second), wide_member, stored_member, member_of(after)}));
        TempDir t;
        NupContainer nup{backing, std::size_t{2} << 20, t.path};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        const auto first_file = nup.OpenFile(0);
        const auto second_file = nup.OpenFile(1);
        REQUIRE(ReadMatches(first_file, first, 0, first.size()));
        REQUIRE(ReadMatches(second_file, second, 0, second.size()));
        REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);
        backing->reads = 0;
        REQUIRE(ReadMatches(first_file, first, 0, first.size()));
        REQUIRE(ReadMatches(second_file, second, 0, second.size()));
        REQUIRE(backing->reads == 0);
        // The compressed member after the wide one is checked, not skipped.
        Member broken = member_of(after);
        broken.sha256[0] ^= 1;
        NupContainer changed{Backing(BuildNup({wide_member, broken})), std::size_t{2} << 20,
                             t.path};
        REQUIRE(changed.GetStatus() == Loader::ResultStatus::Success);
        REQUIRE(changed.VerifyIntegrity({}) ==
                Loader::ResultStatus::ErrorIntegrityVerificationFailed);
    }
    SECTION("spilled") {
        // 64 KiB spills both. The game has read the start of a normal member, which keeps its
        // temporary file and decoder; the large window is refused as reading it is, without
        // writing anything there, and the game's member reads on as if nothing had happened.
        const auto game = Content(std::size_t{2} << 20, 44);
        const auto wide = Content(700 * 1024, 42);
        const Member wide_member{large_window(wide), wide.size(), 1, {}, Sha256(wide)};
        const auto bytes = BuildNup({member_of(game), wide_member});
        // The same reads on a container nobody verifies, for the backing reads they take.
        const auto backing = std::make_shared<CountingFile>(bytes);
        const auto control_backing = std::make_shared<CountingFile>(bytes);
        TempDir t;
        TempDir control_dir;
        NupContainer nup{backing, std::size_t{64} << 10, t.path};
        NupContainer control{control_backing, std::size_t{64} << 10, control_dir.path};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        REQUIRE(control.GetStatus() == Loader::ResultStatus::Success);
        const auto file = nup.OpenFile(0);
        const auto control_file = control.OpenFile(0);
        REQUIRE(ReadMatches(file, game, 0, 8 * 1024));
        REQUIRE(ReadMatches(control_file, game, 0, 8 * 1024));
#if defined(_WIN32) || defined(__linux__)
        // The game's member holds one spill file (on POSIX unlinked since #2649, and seen through
        // its descriptor). Verification opens none of its own, and leaves that one, its identity
        // and its size, as they were.
        const auto before = SpillFiles(t.path);
        REQUIRE(before.size() == 1);
#endif
        REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::ErrorIntegrityVerificationFailed);
#if defined(_WIN32) || defined(__linux__)
        REQUIRE(SpillFiles(t.path) == before);
#endif
        backing->reads = 0;
        control_backing->reads = 0;
        REQUIRE(ReadMatches(file, game, 8 * 1024, 64 * 1024));
        REQUIRE(ReadMatches(control_file, game, 8 * 1024, 64 * 1024));
        REQUIRE(backing->reads == control_backing->reads);
        REQUIRE(ReadMatches(file, game, 0, game.size()));
    }
}

TEST_CASE("Verifying a NUP refuses a small large-window frame wherever a read refuses it",
          "[nup][nup-read]") {
    // zstd decodes a frame with a known content size in one pass, without the window limit, when
    // the whole frame is in the input it is given. A small frame declaring a 256 MiB window that
    // starts 100 bytes before a 128 KiB boundary reaches a spilled reader in two pieces, which
    // refuses it; the verifier must too.
    const auto raw_block = [](std::vector<u8>& frame, std::span<const u8> data) {
        const u32 block = static_cast<u32>(data.size() << 3) | 1; // last block, raw
        frame.insert(frame.end(), {static_cast<u8>(block), static_cast<u8>(block >> 8),
                                   static_cast<u8>(block >> 16)});
        frame.insert(frame.end(), data.begin(), data.end());
    };
    const auto filler = Content(128 * 1024 - 9 - 100, 47);
    const auto small = Content(4096, 48);
    // No content size, a 128 KiB window: ends 100 bytes before the boundary.
    std::vector<u8> extent{0x28, 0xB5, 0x2F, 0xFD, 0x00, 7 << 3};
    raw_block(extent, filler);
    REQUIRE(extent.size() == 128 * 1024 - 100);
    // A 4-byte content size of 4096 and a 2^28 window.
    extent.insert(extent.end(), {0x28, 0xB5, 0x2F, 0xFD, 0x80, 18 << 3, 0x00, 0x10, 0x00, 0x00});
    raw_block(extent, small);
    std::vector<u8> content = filler;
    content.insert(content.end(), small.begin(), small.end());
    const Member member{extent, content.size(), 1, {}, Sha256(content)};
    TempDir t;
    // 64 KiB spills the member.
    NupContainer nup{Backing(BuildNup({member})), std::size_t{64} << 10, t.path};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::ErrorIntegrityVerificationFailed);
    // A read gets the first frame and fails at the second.
    const auto file = nup.OpenFile(0);
    REQUIRE(ReadMatches(file, content, 0, filler.size()));
    std::vector<u8> buffer(small.size());
    REQUIRE(file->Read(buffer.data(), buffer.size(), filler.size()) == 0);
}

TEST_CASE("Verifying a NUP polls its callback while a small extent decodes to much",
          "[nup][nup-read]") {
    // 64 MiB of zeros in a few KiB: one input chunk decodes to far more than the 4 MiB step, and
    // the callback, which is also the cancel, is called while it does.
    const std::vector<u8> zeros(64 << 20);
    const Member member{Zstd(zeros), zeros.size(), 1, {}, Sha256(zeros)};
    REQUIRE(member.extent.size() < (1 << 20));
    TempDir t;
    NupContainer nup{Backing(BuildNup({member})), std::size_t{64} << 20, t.path};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    std::size_t calls{};
    REQUIRE(nup.VerifyIntegrity([&](std::size_t, std::size_t) {
        ++calls;
        return true;
    }) == Loader::ResultStatus::Success);
    REQUIRE(calls >= (64 << 20) / (4 << 20));
    // A cancel inside the member stops it there.
    calls = 0;
    REQUIRE(nup.VerifyIntegrity([&](std::size_t, std::size_t) { return ++calls < 3; }) ==
            Loader::ResultStatus::ErrorIntegrityVerificationFailed);
    REQUIRE(calls == 3);
}

TEST_CASE("Verifying a NUP with an impossible member size fails without reading it whole",
          "[nup][nup-read]") {
    // Found by fuzz_nup: a compressed member recorded 0x250000000200 bytes. Verification read each
    // member whole, so this allocated that size up front and std::bad_alloc ended the emulator
    // ("Verify integrity" on a damaged .nup).
    const auto content = Content(64 * 1024, 15);
    Member member = ZstdMember(content);
    member.size = 0x250000000200;
    member.sha256 = Sha256(content);
    TempDir t;
    NupContainer nup{Backing(BuildNup({member})), std::size_t{8} << 20, t.path};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    Loader::ResultStatus status{};
    REQUIRE_NOTHROW(status = nup.VerifyIntegrity({}));
    REQUIRE(status == Loader::ResultStatus::ErrorIntegrityVerificationFailed);
}
