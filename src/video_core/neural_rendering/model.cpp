// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/neural_rendering/model.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

namespace VideoCore::NeuralRendering {
namespace {
constexpr std::size_t MaxDllBytes = 256 * 1024 * 1024;
constexpr std::size_t PackedBytes = 147683778;

[[noreturn]] void Invalid(const char* reason) {
    throw std::runtime_error(std::string{"Invalid neural rendering model: "} + reason);
}

/// Return a checked byte range, rejecting truncation without overflowing the bounds
/// calculation.
Bytes Slice(Bytes data, std::uint64_t offset, std::uint64_t size) {
    if (offset > data.size() || size > data.size() - offset) {
        Invalid("truncated data");
    }
    return data.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
}

/// Decode a little-endian integer from a checked byte range.
template <typename T>
T Read(Bytes data, std::uint64_t offset) {
    const auto bytes = Slice(data, offset, sizeof(T));
    T result{};
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        result |= static_cast<T>(bytes[i]) << (8 * i);
    }
    return result;
}

struct TensorName {
    unsigned block;
    unsigned layer;
    std::string parameter;
};

/// Parse a supported block/layer tensor name, rejecting malformed numbers and unexpected
/// suffixes.
TensorName ParseName(std::string_view name) {
    const auto number = [&](std::string_view prefix, char separator, std::string_view& remaining) {
        if (!remaining.starts_with(prefix)) {
            Invalid("unexpected tensor name");
        }
        remaining.remove_prefix(prefix.size());
        unsigned value{};
        const auto [end, error] =
            std::from_chars(remaining.data(), remaining.data() + remaining.size(), value);
        if (error != std::errc{} || end == remaining.data() + remaining.size() ||
            *end != separator) {
            Invalid("unexpected tensor name");
        }
        remaining.remove_prefix(static_cast<std::size_t>(end - remaining.data()) + 1);
        return value;
    };
    const unsigned block = number("block", '.', name);
    const unsigned layer = number("layer", '.', name);
    if (block > 70 || layer > 4 || name.empty() ||
        !std::ranges::all_of(name, [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; })) {
        Invalid("unexpected tensor name");
    }
    return {block, layer, std::string{name}};
}

/// Return the lowercase SHA-256 digest, throwing if OpenSSL cannot compute it.
std::string Digest(Bytes bytes) {
    std::array<unsigned char, 32> digest{};
    unsigned int size{};
    if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1 ||
        size != digest.size()) {
        throw std::runtime_error("Could not hash the neural rendering model");
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : digest) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

/// Read a nonempty model file within the size limit, rejecting incomplete reads.
std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path, std::size_t maximum) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const auto size = stream.tellg();
    if (!stream || size <= 0 || static_cast<std::uint64_t>(size) > maximum) {
        throw std::runtime_error(
            "Could not read the neural rendering model, or the file is too large");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error("Could not read the complete neural rendering model");
    }
    return bytes;
}

/// Write all model bytes and report write or close failures.
void WriteFile(const std::filesystem::path& path, Bytes bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) {
        throw std::runtime_error("Could not save the neural rendering model");
    }
}
} // namespace

/// Locate the weights resource in an x64 PE file with bounds checks, without loading executable
/// code.
Bytes ReadWeightsResource(Bytes dll) {
    if (Read<std::uint16_t>(dll, 0) != 0x5a4d) {
        Invalid("not a Windows model file");
    }
    const std::uint64_t pe = Read<std::uint32_t>(dll, 0x3c);
    if (Read<std::uint32_t>(dll, pe) != 0x00004550 || Read<std::uint16_t>(dll, pe + 4) != 0x8664) {
        Invalid("not a 64-bit Windows model file");
    }
    const auto sections = Read<std::uint16_t>(dll, pe + 6);
    const auto optional_size = Read<std::uint16_t>(dll, pe + 20);
    const auto optional = Slice(dll, pe + 24, optional_size);
    if (sections == 0 || sections > 96 || optional.size() < 136 ||
        Read<std::uint16_t>(optional, 0) != 0x20b || Read<std::uint32_t>(optional, 108) < 3) {
        Invalid("unsupported PE header");
    }
    const auto table = Slice(dll, pe + 24 + optional_size, std::uint64_t{sections} * 40);
    const auto from_rva = [&](std::uint32_t rva, std::uint32_t size) -> Bytes {
        for (unsigned i = 0; i < sections; ++i) {
            const auto section = table.subspan(i * 40, 40);
            const auto address = Read<std::uint32_t>(section, 12);
            const auto raw_size = Read<std::uint32_t>(section, 16);
            const auto raw_offset = Read<std::uint32_t>(section, 20);
            if (rva >= address && std::uint64_t{rva} - address <= raw_size &&
                size <= raw_size - (std::uint64_t{rva} - address)) {
                return Slice(dll, std::uint64_t{raw_offset} + rva - address, size);
            }
        }
        Invalid("resource address is outside the file");
    };
    const auto resource =
        from_rva(Read<std::uint32_t>(optional, 128), Read<std::uint32_t>(optional, 132));
    const auto entries = [&](std::uint32_t directory) {
        const auto header = Slice(resource, directory, 16);
        const unsigned count =
            unsigned{Read<std::uint16_t>(header, 12)} + Read<std::uint16_t>(header, 14);
        return Slice(resource, std::uint64_t{directory} + 16, std::uint64_t{count} * 8);
    };
    const auto directory_offset = [&](std::uint32_t value) {
        if (!(value & 0x80000000u)) {
            Invalid("missing resource directory");
        }
        return value & 0x7fffffffu;
    };
    const auto find_entry = [&](Bytes list, auto match) {
        bool found = false;
        std::uint32_t result{};
        for (std::size_t i = 0; i < list.size(); i += 8) {
            if (match(Read<std::uint32_t>(list, i))) {
                if (found) {
                    Invalid("ambiguous model resource");
                }
                result = Read<std::uint32_t>(list, i + 4);
                found = true;
            }
        }
        if (!found) {
            Invalid("model weights resource was not found");
        }
        return result;
    };
    const auto type = find_entry(entries(0), [](std::uint32_t name) { return name == 10; });
    const auto named = find_entry(entries(directory_offset(type)), [&](std::uint32_t name) {
        if (!(name & 0x80000000u)) {
            return false;
        }
        const auto offset = name & 0x7fffffffu;
        const auto length = Read<std::uint16_t>(resource, offset);
        constexpr std::string_view expected = "WEIGHTS_HT";
        const auto text = Slice(resource, std::uint64_t{offset} + 2, std::uint64_t{length} * 2);
        if (length != expected.size()) {
            return false;
        }
        for (std::size_t i = 0; i < length; ++i) {
            if (Read<std::uint16_t>(text, i * 2) != expected[i]) {
                return false;
            }
        }
        return true;
    });
    const auto languages = entries(directory_offset(named));
    if (languages.size() != 8) {
        Invalid("ambiguous model resource language");
    }
    const auto data_offset = Read<std::uint32_t>(languages, 4);
    if (data_offset & 0x80000000u) {
        Invalid("unexpected resource nesting");
    }
    const auto data = Slice(resource, data_offset, 16);
    return from_rva(Read<std::uint32_t>(data, 0), Read<std::uint32_t>(data, 4));
}

/// Parse the packed tensor map into borrowed byte spans, rejecting invalid names, sizes, and
/// duplicates.
TensorMap ReadPackedWeights(Bytes resource) {
    if (Read<std::uint64_t>(resource, 0) != resource.size()) {
        Invalid("weight map size mismatch");
    }
    TensorMap tensors;
    std::uint64_t offset = 8;
    while (offset < resource.size()) {
        const auto name_size = Read<std::uint64_t>(resource, offset);
        offset += 8;
        if (!name_size || name_size > 256 || tensors.size() >= 153) {
            Invalid("invalid tensor count or name length");
        }
        const auto name_bytes = Slice(resource, offset, name_size);
        const std::string name(reinterpret_cast<const char*>(name_bytes.data()), name_bytes.size());
        ParseName(name);
        offset += name_size;
        const auto size = Read<std::uint64_t>(resource, offset);
        offset += 8;
        const auto record = Slice(resource, offset, size);
        offset += size;
        if (record.size() < 40 || Read<std::uint64_t>(record, 0) != size ||
            Read<std::uint32_t>(record, 16) != 1) {
            Invalid("invalid packed tensor record");
        }
        const auto byte_count = Read<std::uint64_t>(record, 8);
        const auto payload = Slice(record, 20, byte_count);
        const auto metadata = Slice(record, 20 + byte_count, record.size() - 20 - byte_count);
        const auto dimensions = Read<std::uint64_t>(metadata, 8);
        if (!dimensions || dimensions > 16 || metadata.size() != 16 + dimensions * 4) {
            Invalid("invalid tensor dimensions");
        }
        std::uint64_t expected_bytes = 2;
        for (std::size_t i = 0; i < dimensions; ++i) {
            const auto dimension = Read<std::uint32_t>(metadata, 16 + i * 4);
            if (!dimension ||
                expected_bytes > std::numeric_limits<std::uint64_t>::max() / dimension) {
                Invalid("invalid tensor dimensions");
            }
            expected_bytes *= dimension;
        }
        if (expected_bytes != byte_count || !tensors.emplace(name, payload).second) {
            Invalid("tensor size mismatch or duplicate tensor");
        }
    }
    if (tensors.empty()) {
        Invalid("empty weight map");
    }
    return tensors;
}

/// Verify the installed manifest, tensor table, and packed weights against the supported model
/// hashes.
bool IsInstalled(const std::filesystem::path& directory) {
    try {
        const auto data = ReadFile(directory / "manifest.json", 128 * 1024);
        const auto manifest = nlohmann::json::parse(data);
        const auto tensors = manifest.at("tensors").dump();
        constexpr std::string_view TensorTableSha256 =
            "a2944b94ebdcfae9269a64e8a27d9f0ef373446c8f59bd64bf95d376b86c9319";
        if (Digest({reinterpret_cast<const std::uint8_t*>(tensors.data()), tensors.size()}) !=
                TensorTableSha256 ||
            manifest.at("totals").at("blockCount") != 71 ||
            manifest.at("totals").at("tensorCount") != 153 || manifest.at("stages").size() != 1) {
            return false;
        }
        const auto& stage = manifest.at("stages").at(0);
        if (stage.at("id") != "packed" || stage.at("file") != "packed.bin" ||
            stage.at("packedByteLength") != PackedBytes ||
            stage.at("sha256") !=
                "37A293B5B781B9351A8B4B5FBEB2FF68FBD8A84891378950812FF3B99D161116") {
            return false;
        }
        const auto packed = ReadFile(directory / "model" / "packed.bin", PackedBytes);
        return packed.size() == PackedBytes && Digest(packed) == PackedSha256;
    } catch (const std::exception&) {
        return false;
    }
}

/// Validate and extract the supported DLL into a new installation, publishing only after all
/// model files are complete.
void ImportModel(const std::filesystem::path& dll, const std::filesystem::path& destination) {
    const auto source = ReadFile(dll, MaxDllBytes);
    const auto resource = ReadWeightsResource(source);
    if (Digest(resource) != ResourceSha256) {
        throw std::runtime_error(
            "This file does not contain the supported DLSS Neural Rendering 310.8.0 model");
    }
    const auto tensors = ReadPackedWeights(resource);
    if (tensors.size() != 153) {
        Invalid("unexpected tensor count");
    }
    std::vector<std::uint8_t> packed;
    packed.reserve(PackedBytes);
    auto entries = nlohmann::json::array();
    for (const auto& [name, bytes] : tensors) {
        const auto parsed = ParseName(name);
        entries.push_back({{"name", name},
                           {"block", parsed.block},
                           {"layer", parsed.layer},
                           {"parameter", parsed.parameter},
                           {"stage", "packed"},
                           {"stageOffset", packed.size()},
                           {"byteLength", bytes.size()}});
        packed.insert(packed.end(), bytes.begin(), bytes.end());
    }
    if (packed.size() != PackedBytes || Digest(packed) != PackedSha256) {
        Invalid("packed model checksum mismatch");
    }
    if (IsInstalled(destination)) {
        return;
    }
    std::string hash{PackedSha256};
    std::ranges::transform(hash, hash.begin(),
                           [](char c) { return c >= 'a' && c <= 'f' ? c - 'a' + 'A' : c; });
    const nlohmann::json manifest{
        {"totals", {{"blockCount", 71}, {"tensorCount", 153}}},
        {"stages", nlohmann::json::array({{{"id", "packed"},
                                           {"file", "packed.bin"},
                                           {"packedByteLength", packed.size()},
                                           {"sha256", hash}}})},
        {"tensors", entries},
        {"provenance",
         {{"model_version", ModelVersion},
          {"resource_sha256", ResourceSha256},
          {"source_sha256", Digest(source)}}}};
    // A directory is the publication unit. Never expose a manifest before the weights are complete.
    auto staging = destination;
    staging += ".importing";
    std::filesystem::create_directories(destination.parent_path());
    if (std::filesystem::exists(destination)) {
        throw std::runtime_error(
            "The neural rendering model destination exists but could not be verified: " +
            destination.string() + ". Move it aside, then import again.");
    }
    if (!std::filesystem::create_directory(staging)) {
        throw std::runtime_error(
            "The neural rendering model staging directory already exists: " + staging.string() +
            ". Wait for any active import to finish. If no import is running, move this directory "
            "aside and import again.");
    }
    try {
        std::filesystem::create_directory(std::filesystem::path{staging} / "model");
        WriteFile(std::filesystem::path{staging} / "model" / "packed.bin", packed);
        const auto text = manifest.dump(2) + "\n";
        WriteFile(std::filesystem::path{staging} / "manifest.json",
                  {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
        std::filesystem::rename(staging, destination);
    } catch (...) {
        std::error_code error;
        std::filesystem::remove_all(staging, error);
        throw;
    }
}

} // namespace VideoCore::NeuralRendering
