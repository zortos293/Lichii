// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/nx2_lowering.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "shader_recompiler/frontend/ampere/instruction.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ampere/program_override.h"
#include "shader_recompiler/program_header.h"

namespace VideoCommon::Nx2Lowering {
namespace {

using Shader::Stage;
using Shader::Ampere::Instruction;
using Bytes = std::span<const u8>;

constexpr u64 NativeHashTag = 0x4f554e4345000002ULL; // shader_environment.cpp NativeShaderHash
constexpr size_t SphSize = sizeof(Shader::ProgramHeader);
constexpr size_t NativeHeaderSize = 128;

template <typename T>
T Load(Bytes bytes, size_t offset) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        throw std::runtime_error(fmt::format("truncated input at {:#x}", offset));
    }
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

} // Anonymous namespace

u64 NativeShaderHash(std::span<const u64> code) {
    return Common::CityHash64(reinterpret_cast<const char*>(code.data()),
                              code.size() * sizeof(u64)) ^
           NativeHashTag;
}

std::optional<Lowered> Lower(Stage stage, std::span<const u8> control, std::span<const u8> code_area,
                             std::string& error) {
    const bool compute = stage == Stage::Compute;
    Shader::ProgramHeader sph{};
    size_t start = 0;
    size_t available = 0;
    if (compute) {
        if (control.size() < 0x3c) {
            error = "short control section";
            return std::nullopt;
        }
        available = std::min<size_t>(Load<u32>(control, 56), code_area.size());
    } else {
        if (code_area.size() <= NativeHeaderSize) {
            error = "short code area";
            return std::nullopt;
        }
        std::memcpy(&sph, code_area.data(), SphSize);
        start = NativeHeaderSize;
        available = code_area.size() - NativeHeaderSize;
    }
    std::vector<Instruction> native;
    Shader::Ampere::ProgramCaptureState capture;
    bool complete = false;
    const size_t count = available / sizeof(Instruction);
    for (u32 index = 0; index < Shader::Ampere::ProgramCaptureState::MaximumInstructions; ++index) {
        if (index >= count) {
            break; // the runtime reads on past the code area; the game file holds only the area
        }
        Instruction instruction;
        std::memcpy(&instruction, code_area.data() + start + index * sizeof(Instruction),
                    sizeof(Instruction));
        native.push_back(instruction);
        const auto status = capture.Observe(instruction, index);
        if (status == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) {
            error = "branch outside capture bounds";
            return std::nullopt;
        }
        if (status == Shader::Ampere::ProgramCaptureState::Result::Complete) {
            complete = true;
            break;
        }
    }
    if (!complete) {
        error = "capture incomplete inside the code area";
        return std::nullopt;
    }
    std::vector<u32> traps;
    for (u32 i = 0; i < native.size(); ++i) {
        if (native[i].Bits(0, 12) == 0x95c) {
            traps.push_back(i * sizeof(Instruction));
        }
    }
    if (!traps.empty()) {
        error = "trap guards"; // checked against live constants; never serialized
        return std::nullopt;
    }
    const auto lowered = Shader::Ampere::LowerToMaxwell(
        native, {.geometry = false,
                 .tensor = true,
                 .native_code_address = 0x100000000ULL + start,
                 .verified_traps = traps,
                 .tessellation_stage = Shader::Ampere::TessellationStage::None});
    if (!lowered) {
        error = fmt::format("lowering failed at {:#x}: {}", lowered.error_offset, lowered.error);
        return std::nullopt;
    }
    Lowered result;
    result.native_key = Shader::Ampere::NativeProgramKey(native);
    result.texture_bound = lowered.required_texture_buffer;
    auto& code = result.code;
    code.resize(SphSize / sizeof(u64));
    std::memcpy(code.data(), &sph, SphSize);
    for (const u64 instruction : lowered.instructions) {
        if ((code.size() * sizeof(u64)) % 32 == 0) {
            code.push_back(0);
        }
        code.push_back(instruction);
    }
    code.push_back(0);
    Shader::Ampere::AppendNativeAddressMap(code, lowered.source_instruction_offsets);
    result.hash = NativeShaderHash(code);
    return result;
}

} // namespace VideoCommon::Nx2Lowering
