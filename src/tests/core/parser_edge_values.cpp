// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Each size and offset field of the KIP/BLZ, INI1, NPDM, NCZ and NUP headers, and of the IPS,
// PFS0/HFS0, RomFS, CNMT and XCI containers, set to the values
// that break unchecked arithmetic (0, 1, 2^31 - 1, 2^31, 2^32 - 1, and the value that makes it
// wrap with the field it is added to) while the rest of the header stays valid. The parser must
// reject the input, or accept it with sizes the input can hold: no success with an empty or
// shrunken section, or with a size the file cannot back. This catches a 32-bit wrap without a
// sanitizer.

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <openssl/evp.h>

#include "common/common_funcs.h"
#include "core/crypto/ncz_decompressor.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/ips_layer.h"
#include "core/file_sys/kernel_executable.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/partition_filesystem.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"

namespace {

using namespace FileSys;

template <typename T>
void Put(std::vector<u8>& out, std::size_t offset, T value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

template <typename T>
T Get(const std::vector<u8>& in, std::size_t offset) {
    T value{};
    std::memcpy(&value, in.data() + offset, sizeof(value));
    return value;
}

VirtualFile File(std::vector<u8> bytes) {
    return std::make_shared<VectorVfsFile>(std::move(bytes), "edge.bin");
}

// The values for a u32 field at `offset` that is added to `partner` (0 when it has none).
std::vector<u32> Edges32(u32 partner) {
    std::vector<u32> values{0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF};
    if (partner != 0) {
        values.push_back(static_cast<u32>(0u - partner));       // wraps to 0
        values.push_back(static_cast<u32>(0u - partner + 0x10)); // wraps to 0x10
    }
    return values;
}

std::vector<u64> Edges64(u64 partner) {
    std::vector<u64> values{0,           1,           0x7FFFFFFF,          0x80000000,
                            0xFFFFFFFF,  0x100000000, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000,
                            ~u64{0}};
    if (partner != 0) {
        values.push_back(0 - partner);
        values.push_back(0 - partner + 0x10);
    }
    return values;
}

// --- KIP and BLZ ---

// The 28-byte BLZ stream of kip_blz.cpp ("ABCDEFGH" four times) in a KIP's text section.
std::vector<u8> BlzKip() {
    std::vector<u8> blz{0x05, 0x50, 0x05, 0x50, 0x05, 0x50, 0xFF};
    for (const char c : std::string_view{"ABCDEFGH"}) {
        blz.push_back(static_cast<u8>(c));
    }
    blz.push_back(0x00);
    blz.resize(blz.size() + 0xC);
    Put<u32>(blz, 16, 28);
    Put<u32>(blz, 20, 0xC);
    Put<u32>(blz, 24, 4);
    std::vector<u8> bytes(0x100);
    Put<u32>(bytes, 0, Common::MakeMagic('K', 'I', 'P', '1'));
    Put<u32>(bytes, 0x24, 32);
    Put<u32>(bytes, 0x28, static_cast<u32>(blz.size()));
    bytes.insert(bytes.end(), blz.begin(), blz.end());
    return bytes;
}

// A KIP with a stored (uncompressed) 16-byte text section.
std::vector<u8> StoredKip() {
    std::vector<u8> bytes(0x100);
    Put<u32>(bytes, 0, Common::MakeMagic('K', 'I', 'P', '1'));
    Put<u32>(bytes, 0x24, 16);
    Put<u32>(bytes, 0x28, 16);
    bytes.resize(0x110, 0x5A);
    return bytes;
}

// Accepted only with every stored section the size the header gives it, and that many bytes in
// the file.
void CheckKip(const std::vector<u8>& bytes) {
    const KIP kip{File(bytes)};
    if (kip.GetStatus() != Loader::ResultStatus::Success) {
        return;
    }
    u64 stored = 0x100;
    for (u8 i = 0; i < 3; ++i) {
        const u32 decompressed = Get<u32>(bytes, 0x24 + i * 0x10);
        const u32 compressed = Get<u32>(bytes, 0x28 + i * 0x10);
        stored += compressed;
        CHECK(kip.GetSectionDecompressed(i).size() == decompressed);
    }
    CHECK(stored <= bytes.size());
}

TEST_CASE("KIP BLZ footer fields at edge values", "[core][file_sys][edge_values]") {
    // Footer at the end of the 28-byte section: compressed size, footer size, added size.
    constexpr std::size_t footer = 0x100 + 16;
    const std::array<std::pair<std::size_t, u32>, 3> fields{{
        {footer + 0, 0},  // compressed size
        {footer + 4, 28}, // footer size, subtracted from the compressed size
        {footer + 8, 28}, // added size, added to the compressed size
    }};
    for (const auto& [offset, partner] : fields) {
        for (const u32 value : Edges32(partner)) {
            INFO("footer field at " << offset << " = " << value);
            auto bytes = BlzKip();
            Put<u32>(bytes, offset, value);
            // Keep the declared size in step with what the footer asks for, so a wrapped
            // output cannot pass as the right size.
            const u64 out = u64{Get<u32>(bytes, footer)} + Get<u32>(bytes, footer + 8);
            if (out <= std::numeric_limits<u32>::max()) {
                Put<u32>(bytes, 0x24, static_cast<u32>(out));
            }
            CheckKip(bytes);
        }
    }
}

TEST_CASE("KIP section fields at edge values", "[core][file_sys][edge_values]") {
    // Section 0 of a stored KIP: offset, decompressed size, compressed size. Both sizes move
    // together, as a stored section has them equal; a compressed size of 0 with a large
    // decompressed size is a known allocation and is left out.
    for (const u32 value : Edges32(0)) {
        INFO("stored section size = " << value);
        auto bytes = StoredKip();
        Put<u32>(bytes, 0x24, value);
        Put<u32>(bytes, 0x28, value);
        CheckKip(bytes);
    }
    for (const u32 value : Edges32(0)) {
        INFO("section offset = " << value);
        auto bytes = StoredKip();
        Put<u32>(bytes, 0x20, value);
        CheckKip(bytes);
    }
}

// --- INI1 ---

TEST_CASE("INI1 KIP count at edge values", "[core][file_sys][edge_values]") {
    // Besides the edges, counts up to the 0x50 an INI1 may hold, which the old code met by
    // reading the first KIP again.
    auto counts = Edges32(0);
    counts.insert(counts.end(), {2, 3, 0x50, 0x51});
    for (const u32 value : counts) {
        INFO("kip_count = " << value);
        std::vector<u8> bytes(0x10);
        Put<u32>(bytes, 0, Common::MakeMagic('I', 'N', 'I', '1'));
        Put<u32>(bytes, 8, value);
        for (int i = 0; i < 2; ++i) {
            const auto kip = StoredKip();
            bytes.insert(bytes.end(), kip.begin(), kip.end());
        }
        const INI ini{File(bytes)};
        if (ini.GetStatus() != Loader::ResultStatus::Success) {
            continue;
        }
        // Never more KIPs than counted, nor than the file holds.
        CHECK(ini.GetKIPs().size() <= std::min<u64>(value, 2));
    }
}

// --- NPDM ---

std::vector<u8> Npdm() {
    std::vector<u8> bytes(0x480);
    std::memcpy(bytes.data(), "META", 4);
    Put<u32>(bytes, 0x70, 0x400); // aci_offset
    Put<u32>(bytes, 0x74, 0x40);  // aci_size
    Put<u32>(bytes, 0x78, 0x100); // acid_offset
    Put<u32>(bytes, 0x7C, 0x240); // acid_size
    std::memcpy(bytes.data() + 0x300, "ACID", 4);
    Put<u32>(bytes, 0x320, 0x240); // fac_offset
    Put<u32>(bytes, 0x324, 0x2C);  // fac_size
    std::memcpy(bytes.data() + 0x400, "ACI0", 4);
    Put<u32>(bytes, 0x420, 0x40); // fah_offset
    Put<u32>(bytes, 0x424, 0x1C); // fah_size
    Put<u32>(bytes, 0x430, 0x60); // kac_offset
    Put<u32>(bytes, 0x434, 8);    // kac_size
    std::memset(bytes.data() + 0x460, 0x41, 0x20);
    return bytes;
}

TEST_CASE("NPDM offset and size fields at edge values", "[core][file_sys][edge_values]") {
    const std::array<std::pair<std::size_t, u32>, 10> fields{{
        {0x70, 0},      // aci_offset
        {0x74, 0},      // aci_size
        {0x78, 0},      // acid_offset
        {0x7C, 0},      // acid_size
        {0x320, 0x100}, // fac_offset, added to acid_offset
        {0x324, 0},     // fac_size
        {0x420, 0x400}, // fah_offset, added to aci_offset
        {0x424, 0},     // fah_size
        {0x430, 0x400}, // kac_offset, added to aci_offset
        {0x434, 0},     // kac_size
    }};
    for (const auto& [offset, partner] : fields) {
        for (const u32 value : Edges32(partner)) {
            INFO("field at " << offset << " = " << value);
            auto bytes = Npdm();
            Put<u32>(bytes, offset, value);
            ProgramMetadata npdm;
            if (npdm.Load(File(bytes)) != Loader::ResultStatus::Success) {
                continue;
            }
            // Accepted: the regions it read lie in the file, in 64-bit arithmetic. The ACI0 and
            // ACID fields are read where the moved offsets put them.
            const u64 aci = Get<u32>(bytes, 0x70);
            const u64 acid = Get<u32>(bytes, 0x78);
            REQUIRE(aci + 0x40 <= bytes.size());
            REQUIRE(acid + 0x240 <= bytes.size());
            CHECK(acid + Get<u32>(bytes, acid + 0x220) + 0x2C <= bytes.size());
            CHECK(aci + Get<u32>(bytes, aci + 0x20) + 0x1C <= bytes.size());
            const u64 kac = aci + Get<u32>(bytes, aci + 0x30);
            const u64 kac_size = Get<u32>(bytes, aci + 0x34) / 4 * 4;
            CHECK(kac + kac_size <= bytes.size());
            CHECK(npdm.GetKernelCapabilities().size() == kac_size / 4);
        }
    }
}

// --- NCZ ---

constexpr std::size_t NczHeader = 0x4000;
constexpr std::size_t NczSection = NczHeader + 16;
constexpr std::size_t NczBlock = NczSection + 0x40;

// A block NCZ: one plaintext section of 0x100 bytes after the NCA header, and one 2^14-byte
// block of 0x100 bytes whose size entry is 0.
std::vector<u8> BlockNcz() {
    std::vector<u8> bytes(NczBlock + 24 + 4);
    std::memcpy(bytes.data() + NczHeader, "NCZSECTN", 8);
    Put<u64>(bytes, NczHeader + 8, 1);
    Put<u64>(bytes, NczSection, NczHeader);
    Put<u64>(bytes, NczSection + 8, 0x100);
    Put<u64>(bytes, NczSection + 16, 1);
    std::memcpy(bytes.data() + NczBlock, "NCZBLOCK", 8);
    bytes[NczBlock + 8] = 2;
    bytes[NczBlock + 9] = 1;
    bytes[NczBlock + 11] = 14;
    Put<u32>(bytes, NczBlock + 12, 1);
    Put<u64>(bytes, NczBlock + 16, 0x100);
    return bytes;
}

void CheckNcz(const std::vector<u8>& bytes) {
    const auto source = File(bytes);
    const bool supported = Core::Crypto::IsSupportedNCZ(source);
    const auto nca = Core::Crypto::DecompressNCZ(source, "edge.nca");
    CHECK(supported == (nca != nullptr));
    if (!nca) {
        return;
    }
    // The decompressed NCA is the header and what the block header says the blocks hold.
    CHECK(nca->GetSize() >= NczHeader);
    CHECK(nca->GetSize() - NczHeader <= Get<u64>(bytes, NczBlock + 16));
}

TEST_CASE("NCZ section and block fields at edge values", "[core][crypto][edge_values]") {
    REQUIRE(Core::Crypto::DecompressNCZ(File(BlockNcz()), "edge.nca") != nullptr);
    for (const u64 value : Edges64(NczHeader)) {
        INFO("section offset = " << value);
        auto bytes = BlockNcz();
        Put<u64>(bytes, NczSection, value);
        CheckNcz(bytes);
    }
    for (const u64 value : Edges64(NczHeader)) {
        INFO("section size = " << value);
        auto bytes = BlockNcz();
        Put<u64>(bytes, NczSection + 8, value);
        CheckNcz(bytes);
    }
    for (const u64 value : Edges64(0)) {
        INFO("section count = " << value);
        auto bytes = BlockNcz();
        Put<u64>(bytes, NczHeader + 8, value);
        CheckNcz(bytes);
    }
    for (const u32 value : Edges32(0)) {
        INFO("block count = " << value);
        auto bytes = BlockNcz();
        Put<u32>(bytes, NczBlock + 12, value);
        CheckNcz(bytes);
    }
    for (const u64 value : Edges64(0)) {
        INFO("decompressed size = " << value);
        auto bytes = BlockNcz();
        Put<u64>(bytes, NczBlock + 16, value);
        CheckNcz(bytes);
    }
    for (const u8 exponent : {u8{0}, u8{13}, u8{14}, u8{32}, u8{33}, u8{63}, u8{64}, u8{255}}) {
        INFO("block exponent = " << int{exponent});
        auto bytes = BlockNcz();
        bytes[NczBlock + 11] = exponent;
        CheckNcz(bytes);
    }
}

// --- NUP ---

// The one-directory, one-file container of nup_validation.cpp.
constexpr std::size_t NupToc = 0x80;
constexpr std::size_t NupPackage = NupToc + 0x30;
constexpr std::size_t NupDir = NupPackage + 0x40;
constexpr std::size_t NupFile = NupDir + 0x18;

std::vector<u8> Nup(std::size_t& toc_size) {
    const std::size_t children = NupFile + 0x50;
    toc_size = children + 4 - NupToc;
    const std::size_t blob = (NupToc + toc_size + 15) & ~std::size_t{15};
    std::vector<u8> bytes(blob + 16);
    std::copy(NupContainer::HeaderMagic.begin(), NupContainer::HeaderMagic.end(), bytes.begin());
    Put<u16>(bytes, 0x04, 0x80);
    Put<u64>(bytes, 0x08, NupToc);
    Put<u64>(bytes, 0x10, toc_size);
    Put<u64>(bytes, 0x38, blob);
    Put<u64>(bytes, 0x40, bytes.size());
    Put<u32>(bytes, 0x48, 1);
    Put<u32>(bytes, NupToc, 0x01434F54);
    Put<u32>(bytes, NupToc + 0x04, 1);
    Put<u64>(bytes, NupToc + 0x08, 1);
    Put<u64>(bytes, NupToc + 0x10, 1);
    Put<u64>(bytes, NupToc + 0x18, toc_size);
    Put<u32>(bytes, NupDir + 0x0C, 1);
    Put<u32>(bytes, children, 1);
    Put<u64>(bytes, NupFile + 0x08, 1);
    Put<u64>(bytes, NupFile + 0x10, blob);
    Put<u64>(bytes, NupFile + 0x18, 1);
    bytes[blob] = 0x42;
    return bytes;
}

void HashToc(std::vector<u8>& bytes, std::size_t toc_size) {
    std::array<u8, 32> hash{};
    unsigned int length{};
    REQUIRE(EVP_Digest(bytes.data() + NupToc, toc_size, hash.data(), &length, EVP_sha256(),
                       nullptr) == 1);
    std::copy(hash.begin(), hash.end(), bytes.begin() + 0x18);
}

TEST_CASE("NUP size and offset fields at edge values", "[core][file_sys][edge_values]") {
    struct Field {
        std::size_t offset;
        std::size_t width;
    };
    const std::array<Field, 12> fields{{
        {NupToc + 0x08, 8}, // dir_count
        {NupToc + 0x10, 8}, // file_count
        {NupToc + 0x18, 8}, // pool_offset
        {NupToc + 0x20, 8}, // pool_size
        {NupDir + 0x00, 4}, // directory name offset
        {NupDir + 0x04, 4}, // directory name length
        {NupDir + 0x08, 4}, // first child
        {NupDir + 0x0C, 4}, // child count
        {NupFile + 0x08, 8}, // file size
        {NupFile + 0x10, 8}, // extent offset
        {NupFile + 0x18, 8}, // extent size
        {NupFile + 0x00, 4}, // file name offset
    }};
    for (const auto& field : fields) {
        const auto values = field.width == 8 ? Edges64(0) : std::vector<u64>{};
        std::vector<u64> all = values;
        if (field.width == 4) {
            for (const u32 value : Edges32(0)) {
                all.push_back(value);
            }
        }
        for (const u64 value : all) {
            INFO("field at " << field.offset << " = " << value);
            std::size_t toc_size{};
            auto bytes = Nup(toc_size);
            if (field.width == 8) {
                Put<u64>(bytes, field.offset, value);
            } else {
                Put<u32>(bytes, field.offset, static_cast<u32>(value));
            }
            HashToc(bytes, toc_size);
            NupContainer nup{File(bytes)};
            if (nup.GetStatus() != Loader::ResultStatus::Success) {
                continue;
            }
            // Accepted: every file it lists opens with its recorded size, and its extent lies in
            // the file.
            for (std::size_t i = 0; i < nup.GetFileCount(); ++i) {
                const auto file = nup.OpenFile(i);
                REQUIRE(file != nullptr);
                u64 recorded{};
                std::memcpy(&recorded, bytes.data() + NupFile + 0x08, sizeof(recorded));
                CHECK(file->GetSize() == recorded);
                CHECK(file->GetSize() <= bytes.size());
            }
        }
    }
}


// --- Containers: every file they list must lie in the input ---

// Walks a parsed container (at most 64 files a directory, 8 levels deep) and checks that reading
// a file never returns more bytes than the input has. A file may still report a larger size
// (RomFS and PFS0 take an entry's size as given and clamp the reads); that is not checked here.
void CheckFiles(const VirtualDir& dir, std::size_t input, int depth = 0) {
    if (!dir || depth > 8) {
        return;
    }
    std::size_t seen = 0;
    for (const auto& file : dir->GetFiles()) {
        if (++seen > 64) {
            return;
        }
        INFO("file " << file->GetName());
        CHECK(file->ReadAllBytes().size() <= input);
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        CheckFiles(sub, input, depth + 1);
    }
}

// A PFS0 (or HFS0, with 0x40-byte entries) holding "a" with 4 bytes and "bc" with 2.
std::vector<u8> Pfs0(bool hfs = false) {
    const std::size_t entry = hfs ? 0x40 : 0x18;
    const std::string_view strtab{"a\0bc\0\0\0", 8};
    std::vector<u8> bytes(0x10 + 2 * entry);
    Put<u32>(bytes, 0, hfs ? Common::MakeMagic('H', 'F', 'S', '0')
                           : Common::MakeMagic('P', 'F', 'S', '0'));
    Put<u32>(bytes, 4, 2);
    Put<u32>(bytes, 8, static_cast<u32>(strtab.size()));
    Put<u64>(bytes, 0x10, 0);
    Put<u64>(bytes, 0x18, 4);
    Put<u32>(bytes, 0x20, 0);
    Put<u64>(bytes, 0x10 + entry, 4);
    Put<u64>(bytes, 0x18 + entry, 2);
    Put<u32>(bytes, 0x20 + entry, 2);
    bytes.insert(bytes.end(), strtab.begin(), strtab.end());
    bytes.insert(bytes.end(), {'d', 'a', 't', 'a', 'h', 'i'});
    return bytes;
}

TEST_CASE("PFS0 and HFS0 fields at edge values", "[core][file_sys][edge_values]") {
    for (const bool hfs : {false, true}) {
        const std::size_t entry = hfs ? 0x40 : 0x18;
        REQUIRE(PartitionFilesystem{File(Pfs0(hfs))}.GetStatus() ==
                Loader::ResultStatus::Success);
        for (const u32 value : Edges32(0)) {
            for (const std::size_t field : {std::size_t{4}, std::size_t{8}, 0x20 + entry}) {
                INFO((hfs ? "HFS0" : "PFS0") << " u32 field at " << field << " = " << value);
                auto bytes = Pfs0(hfs);
                Put<u32>(bytes, field, value);
                PartitionFilesystem pfs{File(bytes)};
                if (pfs.GetStatus() == Loader::ResultStatus::Success) {
                    CHECK(pfs.GetFiles().size() <= std::min<u64>(Get<u32>(bytes, 4), 2));
                    for (const auto& file : pfs.GetFiles()) {
                        CHECK(file->ReadAllBytes().size() <= bytes.size());
                    }
                }
            }
        }
        for (const u64 value : Edges64(0)) {
            for (const std::size_t field : {0x10 + entry, 0x18 + entry}) {
                INFO((hfs ? "HFS0" : "PFS0") << " entry field at " << field << " = " << value);
                auto bytes = Pfs0(hfs);
                Put<u64>(bytes, field, value);
                PartitionFilesystem pfs{File(bytes)};
                if (pfs.GetStatus() == Loader::ResultStatus::Success) {
                    for (const auto& file : pfs.GetFiles()) {
                        CHECK(file->ReadAllBytes().size() <= bytes.size());
                    }
                }
            }
        }
    }
}

// The RomFS of file_sys_parsers.cpp: a root holding directory "a" holding file "f" with "hi".
std::vector<u8> RomFS() {
    std::vector<u8> dirs(0x34); // two 0x18-byte entries, "a" padded to 4 bytes
    const std::array<u32, 6> root{0, 0xFFFFFFFF, 0x18, 0xFFFFFFFF, 0, 0};
    const std::array<u32, 6> a{0, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 1};
    for (std::size_t i = 0; i < 6; ++i) {
        Put<u32>(dirs, i * 4, root[i]);
        Put<u32>(dirs, 0x18 + i * 4, a[i]);
    }
    dirs[0x18 + 0x18] = 'a';
    std::vector<u8> files(0x24);
    Put<u32>(files, 0, 0x18);
    Put<u32>(files, 4, 0xFFFFFFFF);
    Put<u64>(files, 8, 0);
    Put<u64>(files, 0x10, 2);
    Put<u32>(files, 0x18, 0);
    Put<u32>(files, 0x1C, 1);
    files[0x20] = 'f';
    std::vector<u8> bytes(0x50);
    Put<u64>(bytes, 0x00, 0x50);
    Put<u64>(bytes, 0x18, 0x50);
    Put<u64>(bytes, 0x20, dirs.size());
    Put<u64>(bytes, 0x38, 0x50 + dirs.size());
    Put<u64>(bytes, 0x40, files.size());
    Put<u64>(bytes, 0x48, 0x50 + dirs.size() + files.size());
    bytes.insert(bytes.end(), dirs.begin(), dirs.end());
    bytes.insert(bytes.end(), files.begin(), files.end());
    bytes.insert(bytes.end(), {'h', 'i'});
    return bytes;
}

TEST_CASE("RomFS fields at edge values", "[core][file_sys][edge_values]") {
    REQUIRE(ExtractRomFS(File(RomFS())) != nullptr);
    const std::size_t file_entry = 0x50 + 0x34;
    const std::array<std::size_t, 8> u64_fields{
        0x18, 0x20, 0x38, 0x40, 0x48,          // table offsets and sizes, data offset
        file_entry + 0x08, file_entry + 0x10, // the file's data offset and size
        0x00,                                  // header size
    };
    for (const auto field : u64_fields) {
        for (const u64 value : Edges64(0)) {
            INFO("RomFS u64 field at " << field << " = " << value);
            auto bytes = RomFS();
            Put<u64>(bytes, field, value);
            CheckFiles(ExtractRomFS(File(bytes)), bytes.size());
        }
    }
    const std::array<std::size_t, 4> u32_fields{
        0x50 + 0x18 + 0x14, // directory name length
        file_entry + 0x1C,  // file name length
        0x50 + 0x08,        // root's first child directory
        0x50 + 0x18 + 0x0C, // "a"'s first file
    };
    for (const auto field : u32_fields) {
        for (const u32 value : Edges32(0)) {
            INFO("RomFS u32 field at " << field << " = " << value);
            auto bytes = RomFS();
            Put<u32>(bytes, field, value);
            CheckFiles(ExtractRomFS(File(bytes)), bytes.size());
        }
    }
}

// An IPS (or IPS32) patch with one 2-byte record at 0x10 and one 4-byte RLE record at 0x20.
std::vector<u8> Ips(bool ips32) {
    const std::size_t off = ips32 ? 4 : 3;
    std::vector<u8> bytes;
    const std::string_view magic = ips32 ? "IPS32" : "PATCH";
    bytes.insert(bytes.end(), magic.begin(), magic.end());
    const auto record = [&](u32 offset, std::initializer_list<u8> rest) {
        for (std::size_t i = off; i-- > 0;) {
            bytes.push_back(static_cast<u8>(offset >> (8 * i)));
        }
        bytes.insert(bytes.end(), rest);
    };
    record(0x10, {0x00, 0x02, 0xAB, 0xCD});
    record(0x20, {0x00, 0x00, 0x00, 0x04, 0x11});
    const std::string_view end = ips32 ? "EEOF" : "EOF";
    bytes.insert(bytes.end(), end.begin(), end.end());
    return bytes;
}

TEST_CASE("IPS record fields at edge values", "[core][file_sys][edge_values]") {
    const auto base = File(std::vector<u8>(0x40, 0xAA));
    for (const bool ips32 : {false, true}) {
        REQUIRE(PatchIPS(base, File(Ips(ips32))) != nullptr);
        const std::size_t off = ips32 ? 4 : 3;
        const std::size_t first = 5;                      // first record's offset
        const std::size_t second = first + off + 4;      // second record's offset
        const std::size_t sizes[] = {first + off, second + off, second + off + 2};
        for (const u32 value : Edges32(0)) {
            for (std::size_t field = 0; field < 4; ++field) {
                INFO((ips32 ? "IPS32" : "IPS") << " field " << field << " = " << value);
                auto bytes = Ips(ips32);
                if (field < 1) {
                    for (std::size_t i = 0; i < off; ++i) {
                        bytes[first + i] = static_cast<u8>(value >> (8 * (off - 1 - i)));
                    }
                } else {
                    // Record sizes are big-endian u16s.
                    bytes[sizes[field - 1]] = static_cast<u8>(value >> 8);
                    bytes[sizes[field - 1] + 1] = static_cast<u8>(value);
                }
                const auto patched = PatchIPS(base, File(bytes));
                if (patched) {
                    // Patching never grows or shrinks the file.
                    CHECK(patched->GetSize() == base->GetSize());
                    CHECK(patched->ReadAllBytes().size() == base->GetSize());
                }
            }
        }
    }
}

TEST_CASE("CNMT table fields at edge values", "[core][file_sys][edge_values]") {
    // An application CNMT: header, optional header, then one content and one meta record.
    std::vector<u8> base(0x20 + 0x10 + 0x38 + 0x10);
    base[0x0C] = 0x80;
    Put<u16>(base, 0x0E, 0x10);
    Put<u16>(base, 0x10, 1);
    Put<u16>(base, 0x12, 1);
    for (const std::size_t field : {std::size_t{0x0E}, std::size_t{0x10}, std::size_t{0x12}}) {
        for (const u16 value : {u16{0}, u16{1}, u16{0x7FFF}, u16{0x8000}, u16{0xFFFF}}) {
            INFO("CNMT field at " << field << " = " << value);
            auto bytes = base;
            Put<u16>(bytes, field, value);
            const CNMT cnmt{File(bytes)};
            const u64 table = 0x20 + u64{Get<u16>(bytes, 0x0E)};
            // Only records read in full are kept.
            const auto contents = cnmt.GetContentRecords().size();
            const auto metas = cnmt.GetMetaRecords().size();
            CHECK(contents <= Get<u16>(bytes, 0x10));
            CHECK((contents == 0 || table + contents * 0x38 <= bytes.size()));
            CHECK(metas <= Get<u16>(bytes, 0x12));
            CHECK((metas == 0 || table + metas * 0x10 <= bytes.size()));
        }
    }
}

// The XCI of file_sys_parsers.cpp: a root HFS0 with update, normal and secure partitions, each
// an empty HFS0.
std::vector<u8> Xci() {
    std::vector<u8> partition(0x10);
    Put<u32>(partition, 0, Common::MakeMagic('H', 'F', 'S', '0'));
    const std::string_view strtab{"update\0normal\0secure\0", 21};
    std::vector<u8> root(0x10 + 3 * 0x40);
    Put<u32>(root, 0, Common::MakeMagic('H', 'F', 'S', '0'));
    Put<u32>(root, 4, 3);
    Put<u32>(root, 8, static_cast<u32>(strtab.size()));
    const u32 names[] = {0, 7, 14};
    for (std::size_t i = 0; i < 3; ++i) {
        Put<u64>(root, 0x10 + i * 0x40, i * partition.size());
        Put<u64>(root, 0x18 + i * 0x40, partition.size());
        Put<u32>(root, 0x20 + i * 0x40, names[i]);
    }
    root.insert(root.end(), strtab.begin(), strtab.end());
    for (int i = 0; i < 3; ++i) {
        root.insert(root.end(), partition.begin(), partition.end());
    }
    std::vector<u8> bytes(0x200);
    Put<u32>(bytes, 0x100, Common::MakeMagic('H', 'E', 'A', 'D'));
    Put<u64>(bytes, 0x130, 0x200);
    Put<u64>(bytes, 0x138, root.size());
    bytes.insert(bytes.end(), root.begin(), root.end());
    return bytes;
}

TEST_CASE("XCI partition fields at edge values", "[core][file_sys][edge_values]") {
    REQUIRE(XCI{File(Xci())}.GetStatus() == Loader::ResultStatus::Success);
    const std::array<std::size_t, 6> fields{
        0x130,             // root HFS0 offset
        0x138,             // root HFS0 size
        0x200 + 0x10,      // update partition offset
        0x200 + 0x18,      // update partition size
        0x200 + 0x10 + 0x80, // secure partition offset
        0x200 + 0x18 + 0x80, // secure partition size
    };
    for (const auto field : fields) {
        for (const u64 value : Edges64(0)) {
            INFO("XCI field at " << field << " = " << value);
            auto bytes = Xci();
            Put<u64>(bytes, field, value);
            XCI xci{File(bytes)};
            if (xci.GetStatus() != Loader::ResultStatus::Success) {
                continue;
            }
            for (const auto partition : {XCIPartition::Update, XCIPartition::Normal,
                                         XCIPartition::Secure}) {
                const auto raw = xci.GetPartitionRaw(partition);
                if (raw) {
                    CHECK(raw->ReadAllBytes().size() <= bytes.size());
                }
            }
        }
    }
}

} // Anonymous namespace
