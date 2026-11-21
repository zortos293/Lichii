// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Inputs the parser fuzzers (src/fuzz) found, replayed through the same entry points as the
// harnesses, so a regression shows up in every test run without libFuzzer or a sanitizer. Each
// input is synthetic, kept as small as the bug allows, and names the bug and the PR that fixed
// it. Where the old code allocated gigabytes, the fixed code must reject the input before it
// allocates; those cases would exhaust memory on a build without the fix.

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "common/fs/path_util.h"
#include "common/scope_exit.h"
#include "core/crypto/key_manager.h"
#include "core/crypto/ncz_decompressor.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/ips_layer.h"
#include "core/file_sys/kernel_executable.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"
#include "core/memory/cheat_engine.h"
#include "tests/common/restore_eden_path.h"

namespace {

using namespace FileSys;

void Put32(std::vector<u8>& out, std::size_t offset, u32 value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

void Put64(std::vector<u8>& out, std::size_t offset, u64 value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

VirtualFile File(std::vector<u8> bytes, std::string name = "fuzz.bin") {
    return std::make_shared<VectorVfsFile>(std::move(bytes), std::move(name));
}

VirtualFile Text(std::string_view text) {
    return File(std::vector<u8>(text.begin(), text.end()));
}

// The cheat parser on a buffer exactly as long as the text, as the cheat file is read.
std::vector<Core::Memory::CheatEntry> ParseCheats(std::string_view text) {
    const std::vector<char> exact(text.begin(), text.end());
    return Core::Memory::TextCheatParser{}.Parse(std::string_view{exact.data(), exact.size()});
}

// --- KIP / INI1 ---

// A KIP whose text section is `section`, decompressing to `decompressed` bytes.
std::vector<u8> Kip(const std::vector<u8>& section, u32 decompressed) {
    std::vector<u8> bytes(0x100);
    Put32(bytes, 0, Common::MakeMagic('K', 'I', 'P', '1'));
    Put32(bytes, 0x24, decompressed);
    Put32(bytes, 0x28, static_cast<u32>(section.size()));
    bytes.insert(bytes.end(), section.begin(), section.end());
    return bytes;
}

// The 28-byte BLZ stream of kip_blz.cpp: "ABCDEFGH" four times, then the footer (compressed
// size, footer size, added size).
std::vector<u8> Blz(u32 compressed = 28, u32 footer = 0xC, u32 added = 4) {
    std::vector<u8> blz{0x05, 0x50, 0x05, 0x50, 0x05, 0x50, 0xFF};
    for (const char c : std::string_view{"ABCDEFGH"}) {
        blz.push_back(static_cast<u8>(c));
    }
    blz.push_back(0x00);
    blz.resize(blz.size() + 0xC);
    Put32(blz, 16, compressed);
    Put32(blz, 20, footer);
    Put32(blz, 24, added);
    return blz;
}

Loader::ResultStatus KipStatus(const std::vector<u8>& bytes) {
    return KIP{File(bytes, "fuzz.kip")}.GetStatus();
}

// --- NPDM ---

// The 0x480-byte NPDM of npdm_kac.cpp, with the fields the fuzzers hit as parameters.
std::vector<u8> Npdm(u32 kac_size, u32 kac_offset = 0x60, u32 fac_offset = 0x240,
                     u32 fah_offset = 0x40) {
    std::vector<u8> bytes(0x480);
    std::memcpy(bytes.data(), "META", 4);
    Put32(bytes, 0x70, 0x400);
    Put32(bytes, 0x74, 0x40);
    Put32(bytes, 0x78, 0x100);
    Put32(bytes, 0x7C, 0x240);
    std::memcpy(bytes.data() + 0x300, "ACID", 4);
    Put32(bytes, 0x320, fac_offset);
    Put32(bytes, 0x324, 0x2C);
    std::memcpy(bytes.data() + 0x400, "ACI0", 4);
    Put32(bytes, 0x420, fah_offset);
    Put32(bytes, 0x424, 0x1C);
    Put32(bytes, 0x430, kac_offset);
    Put32(bytes, 0x434, kac_size);
    std::memset(bytes.data() + 0x460, 0x41, 0x20);
    return bytes;
}

// --- NCZ ---

constexpr std::size_t NczHeader = 0x4000;

// An NCZ with one plaintext section over `size` bytes after the NCA header, and an NCZBLOCK
// header with `count` block sizes (left zero) for `decompressed` bytes in blocks of 2^exponent.
std::vector<u8> NczBlock(u8 exponent, u32 count, u64 decompressed, u64 offset = NczHeader,
                         u64 size = 0x100) {
    std::vector<u8> bytes(NczHeader);
    const std::string_view sections = "NCZSECTN";
    bytes.insert(bytes.end(), sections.begin(), sections.end());
    bytes.resize(bytes.size() + 8 + 0x40);
    Put64(bytes, NczHeader + 8, 1);
    Put64(bytes, NczHeader + 16, offset);
    Put64(bytes, NczHeader + 24, size);
    Put64(bytes, NczHeader + 32, 1);
    const std::string_view block = "NCZBLOCK";
    bytes.insert(bytes.end(), block.begin(), block.end());
    bytes.push_back(2);
    bytes.push_back(1);
    bytes.push_back(0);
    bytes.push_back(exponent);
    const std::size_t fields = bytes.size();
    bytes.resize(fields + 12 + std::size_t{count} * 4);
    Put32(bytes, fields, count);
    Put64(bytes, fields + 4, decompressed);
    return bytes;
}

// What the ncz harness does: the two probes, then opening it. True if it opened.
bool OpensAsNcz(const std::vector<u8>& bytes) {
    const auto file = File(bytes, "fuzz.ncz");
    (void)Core::Crypto::IsBlockCompressedNCZ(file);
    const bool supported = Core::Crypto::IsSupportedNCZ(file);
    const bool opened = Core::Crypto::DecompressNCZ(file, "fuzz.nca") != nullptr;
    CHECK(supported == opened);
    return opened;
}

} // Anonymous namespace

TEST_CASE("Fuzzed cheat text: unterminated names", "[core][fuzz_regressions]") {
    // #1882: ExtractName read one byte past the text before checking its size.
    CHECK(ParseCheats("{M").empty());
    CHECK(ParseCheats("[Inf HP").empty());
    CHECK(ParseCheats("[").empty());
    CHECK(ParseCheats("{").empty());
}

TEST_CASE("Fuzzed IPSwitch text: short lines", "[core][fuzz_regressions]") {
    // #1878: "@n" made substr(8) throw, which nothing caught.
    const std::array<u8, 3> short_bid{'@', 'n', 0x00};
    const IPSwitchCompiler nso_bid{File({short_bid.begin(), short_bid.end()})};
    CHECK(nso_bid.GetBuildID() == std::array<u8, 0x20>{});

    // #1878: strtoll read the value past the line, which is not null-terminated: as the last
    // line it read past the file, and with no value it parsed the next lines ("0x20").
    // The first read past the file is only visible to ASan; here it must not crash.
    const IPSwitchCompiler at_end{Text("@enabled\n@flag offset_shift 0x10")};
    const IPSwitchCompiler no_value{Text("@flag offset_shift\n\n0x20\n@enabled\n00000000 AB\n")};
    const auto out = no_value.Apply(File(std::vector<u8>(0x40)))->ReadAllBytes();
    CHECK(out[0] == 0xAB);
    CHECK(out[0x20] == 0);
}

TEST_CASE("Fuzzed KIP: BLZ footers", "[core][fuzz_regressions]") {
    // The valid stream still decodes.
    REQUIRE(KipStatus(Kip(Blz(), 32)) == Loader::ResultStatus::Success);

    // #1879: footers that made DecompressBLZ index the buffer with wrapped offsets.
    const std::array<std::vector<u8>, 5> bad{
        Blz(0x1000),          // compressed size larger than the section
        Blz(28, 29),          // footer size larger than the compressed size
        Blz(28, 0xC, 0x1000), // literals run out of input before the output is full
        // #1879 (second commit): the u32 sum compressed + added wrapped to 12 and to 0.
        Blz(28, 0xC, 0xFFFFFFF0),
        Blz(28, 0xC, 0xFFFFFFE4),
    };
    for (const auto& section : bad) {
        CHECK(KipStatus(Kip(section, 32)) == Loader::ResultStatus::ErrorBLZDecompressionFailed);
    }
    // #1879: the same with literals in place of the copies, which read the next control byte
    // from before the section.
    auto literals = Blz(28, 0xC, 0x1000);
    literals[6] = 0x00;
    CHECK(KipStatus(Kip(literals, 32)) == Loader::ResultStatus::ErrorBLZDecompressionFailed);
}

TEST_CASE("Fuzzed KIP: a .bss larger than memory", "[core][fuzz_regressions]") {
    // #1881: a .bss with no stored bytes was allocated at its declared size, here 4 GiB - 1. The
    // fixed code allocates nothing for it.
    std::vector<u8> bytes = Kip({'t', 'e', 'x', 't'}, 4);
    Put32(bytes, 0x54, 0xFFFFFFFF);
    const KIP kip{File(bytes, "fuzz.kip")};
    REQUIRE(kip.GetStatus() == Loader::ResultStatus::Success);
    CHECK(kip.GetBSSSize() == 0xFFFFFFFF);
    CHECK(kip.GetSectionDecompressed(3).empty());
}

TEST_CASE("Fuzzed INI1: every KIP is read", "[core][fuzz_regressions]") {
    // #1883: the offset never advanced, so N KIPs gave the first one N times.
    std::vector<u8> bytes(0x10);
    Put32(bytes, 0, Common::MakeMagic('I', 'N', 'I', '1'));
    Put32(bytes, 8, 2);
    for (const char* name : {"FS", "spl"}) {
        auto kip = Kip({'x'}, 1);
        std::memcpy(kip.data() + 4, name, std::strlen(name));
        bytes.insert(bytes.end(), kip.begin(), kip.end());
    }
    const INI ini{File(bytes, "ini1.bin")};
    REQUIRE(ini.GetKIPs().size() == 2);
    CHECK(ini.GetKIPs()[0].GetName() == "FS");
    CHECK(ini.GetKIPs()[1].GetName() == "spl");
}

TEST_CASE("Fuzzed INI1: a later KIP's BLZ stream that starts with no input", "[core][fuzz_regressions]") {
    // CodeRabbit's review of #1883: now that the INI1 reaches every KIP, a second KIP whose
    // 12-byte text section is only a BLZ footer (compressed 12, footer 12, added 1) starts the
    // decoder with no input left. Without #1879 the cursor wrapped and the read went past the
    // section; with it that KIP fails and the INI1 stops before it.
    std::vector<u8> bytes(0x10);
    Put32(bytes, 0, Common::MakeMagic('I', 'N', 'I', '1'));
    Put32(bytes, 8, 2);
    auto first = Kip({}, 0);
    std::memcpy(first.data() + 4, "FS", 2);
    bytes.insert(bytes.end(), first.begin(), first.end());
    std::vector<u8> footer_only(0xC);
    Put32(footer_only, 0, 12);
    Put32(footer_only, 4, 12);
    Put32(footer_only, 8, 1);
    auto second = Kip(footer_only, 13);
    std::memcpy(second.data() + 4, "spl", 3);
    CHECK(KipStatus(second) == Loader::ResultStatus::ErrorBLZDecompressionFailed);
    bytes.insert(bytes.end(), second.begin(), second.end());
    const INI ini{File(bytes, "ini1.bin")};
    REQUIRE(ini.GetKIPs().size() == 1);
    CHECK(ini.GetKIPs()[0].GetName() == "FS");
}

TEST_CASE("Fuzzed NPDM: kernel capability and file access fields", "[core][fuzz_regressions]") {
    const auto load = [](std::vector<u8> bytes) {
        ProgramMetadata npdm;
        const auto status = npdm.Load(File(std::move(bytes), "main.npdm"));
        return std::make_pair(status, npdm.GetKernelCapabilities().size());
    };
    REQUIRE(load(Npdm(8)) == std::make_pair(Loader::ResultStatus::Success, std::size_t{2}));

    // #1877: five bytes were read into a one-word vector.
    CHECK(load(Npdm(5)) == std::make_pair(Loader::ResultStatus::Success, std::size_t{1}));
    // #1877: allocated 3.75 GiB before the read failed; now rejected before allocating.
    CHECK(load(Npdm(0xF0000000)).first ==
          Loader::ResultStatus::ErrorBadKernelCapabilityDescriptors);
    // #1877: 32-bit offset sums that wrapped back into the NPDM header and loaded.
    CHECK(load(Npdm(8, 0xFFFFFC60)).first ==
          Loader::ResultStatus::ErrorBadKernelCapabilityDescriptors);
    CHECK(load(Npdm(8, 0x60, 0xFFFFFF40)).first == Loader::ResultStatus::ErrorBadFileAccessControl);
    CHECK(load(Npdm(8, 0x60, 0x240, 0xFFFFFC40)).first ==
          Loader::ResultStatus::ErrorBadFileAccessHeader);
}

TEST_CASE("Fuzzed NCZ: NCZBLOCK headers", "[core][fuzz_regressions]") {
    // The builder's stream itself opens.
    REQUIRE(OpensAsNcz(NczBlock(14, 1, 0x100)));

    // #1880: 2^32-byte blocks with two of them for 0x4100 bytes made the first block allocate
    // 4 GiB; the count does not fit the size, so the header is rejected.
    CHECK_FALSE(OpensAsNcz(NczBlock(32, 2, 0x4100)));
    CHECK_FALSE(OpensAsNcz(NczBlock(14, 3, 0x4100)));

    // #1892: block mode skipped the section checks of a solid
    // stream, so these opened as files of about 2^63 bytes or with a section before the header.
    CHECK_FALSE(OpensAsNcz(NczBlock(14, 1, 0x100, 0x7FFFFFFFFFFFFFF0, 0x100)));
    CHECK_FALSE(OpensAsNcz(NczBlock(14, 1, 0x100, static_cast<u64>(-0x1000), 0x5000)));
    CHECK_FALSE(OpensAsNcz(NczBlock(14, 1, 0x100, NczHeader, 0x7FFFFFFFFFFF0000)));
}

TEST_CASE("Fuzzed keys file: keyblob indices past the table", "[core][fuzz_regressions]") {
    // #1876: keyblob_ff wrote 0x90 bytes about 36 KiB past the 0x20-entry table. Plain builds
    // cannot see that write; with the fix the lines are skipped and no other key changes.
    auto& keys = Core::Crypto::KeyManager::Instance();
    // A folder this run creates itself (create_directory fails on one that exists), so another
    // tests process can't remove it before ReloadKeys reads it.
    std::filesystem::path directory;
    std::random_device random;
    do {
        directory = std::filesystem::temp_directory_path() /
                    ("lichii-parser-regressions-" + std::to_string(random()));
    } while (!std::filesystem::create_directory(directory));
    {
        std::ofstream file{directory / "prod.keys"};
        file << "keyblob_ff = " << std::string(0x90 * 2, 'a') << '\n';
        file << "encrypted_keyblob_ff = " << std::string(0xB0 * 2, 'b') << '\n';
        file << "keyblob_20 = " << std::string(0x90 * 2, 'c') << '\n';
    }
    const auto source = keys.GetKey(Core::Crypto::S128KeyType::Source,
                                    u64(Core::Crypto::SourceKeyType::Titlekek));
    const auto previous = Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir);
    {
        // Put KeysDir back and read the user's keys again even if a check fails, then remove the
        // folder. A plain SetEdenPath is dropped on a fresh profile, where the keys folder does
        // not exist yet, and KeysDir kept naming the removed folder.
        bool reloaded = false;
        SCOPE_EXIT {
            Tests::RestoreEdenPath(Common::FS::EdenPath::KeysDir, previous);
            if (reloaded) {
                keys.ReloadKeys();
            }
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        };
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, directory);
        reloaded = true;
        keys.ReloadKeys();
        CHECK(keys.GetKey(Core::Crypto::S128KeyType::Source,
                          u64(Core::Crypto::SourceKeyType::Titlekek)) == source);
    }
    CHECK(Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir) == previous);
    CHECK_FALSE(std::filesystem::exists(directory));
}

TEST_CASE("Fuzzed metadata: the meta harness's readers on every input above",
          "[core][fuzz_regressions]") {
    // The meta harness also runs NACP and CNMT on each input. None of these may throw.
    std::vector<std::vector<u8>> inputs{Kip(Blz(), 32), Npdm(5), Npdm(0xF0000000)};
    for (const auto& bytes : inputs) {
        const auto file = File(bytes);
        const NACP nacp{file};
        (void)nacp.GetApplicationName();
        const CNMT cnmt{file};
        (void)cnmt.GetTitleID();
        const INI ini{file};
        (void)ini.GetKIPs();
    }
}
