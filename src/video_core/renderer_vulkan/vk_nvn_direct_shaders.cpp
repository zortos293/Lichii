// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef LICHII_NVN_DIRECT

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include "common/logging.h"
#include "common/settings.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ampere/program_override.h"
#include "shader_recompiler/frontend/ampere/trap_guards.h"
#include "shader_recompiler/frontend/ir/attribute.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/program_header.h"
#include "video_core/native_lowering.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/vk_nvn_direct_shaders.h"
#include "video_core/shader_stage_translation.h"
#include "video_core/surface.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

namespace {

using Shader::Ampere::Instruction;

using Shader::Backend::SPIRV::RenderAreaLayout;
using Shader::Backend::SPIRV::RescalingLayout;
static_assert(sizeof(RenderAreaLayout) == NVN_DIRECT_RENDER_AREA_SIZE);
static_assert(sizeof(RescalingLayout) <= NVN_DIRECT_SCALE_PUSH_SIZE &&
              offsetof(RescalingLayout, rescaling_images) == NVN_DIRECT_SCALE_IMAGES_OFFSET &&
              offsetof(RescalingLayout, down_factor) == NVN_DIRECT_SCALE_DOWN_FACTOR_OFFSET);

u32 Read32(const u8* bytes, size_t offset) {
    u32 value;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

u16 Read16(const u8* bytes, size_t offset) {
    u16 value;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

Shader::Stage StageOf(u32 nvn_stage) {
    switch (nvn_stage) {
    case 0:
        return Shader::Stage::VertexB;
    case 1:
        return Shader::Stage::Fragment;
    case 2:
        return Shader::Stage::Geometry;
    case 3:
        return Shader::Stage::TessellationControl;
    case 4:
        return Shader::Stage::TessellationEval;
    case 5:
        return Shader::Stage::Compute;
    case 6:
        return Shader::Stage::Task;
    case 7:
        return Shader::Stage::Mesh;
    }
    throw Shader::RuntimeError("NVN stage {}", nvn_stage);
}

Shader::TextureType TextureTypeOf(u32 nvn_target) {
    switch (nvn_target) {
    case 0: // 1D
        return Shader::TextureType::Color1D;
    case 2: // 3D
        return Shader::TextureType::Color3D;
    case 3: // 1D array
        return Shader::TextureType::ColorArray1D;
    case 4: // 2D array
    case 6: // 2D multisample array
        return Shader::TextureType::ColorArray2D;
    case 7: // rectangle
        return Shader::TextureType::Color2DRect;
    case 8: // cube map
        return Shader::TextureType::ColorCube;
    case 9: // cube map array
        return Shader::TextureType::ColorArrayCube;
    case 10: // buffer
        return Shader::TextureType::Buffer;
    default: // 2D, 2D multisample
        return Shader::TextureType::Color2D;
    }
}

bool IsNvn2Control(const u8* control) {
    return Read32(control, 0) == 0x98761234U && Read32(control, 4) == 0xc9;
}

/// A program stage as the recompiler reads it: an NVN2 stage's Ampere code captured and lowered
/// from the code area as GenericEnvironment::LowerNativeCode does from GPU memory, or an NVN1
/// stage's Maxwell code as it is; the control section in place of the engine registers the
/// guest's driver would set from it, and the draw's state through the replay's callbacks.
class NvnDirectEnvironment final : public Shader::Environment {
public:
    explicit NvnDirectEnvironment(const nvn_direct_shader_source& source_,
                                  const NvnDirectShaders& shaders_)
        : source{source_}, shaders{shaders_}, nvn2{IsNvn2Control(source_.control)} {
        stage = StageOf(source.nvn_stage);
        is_proprietary_driver = true;
        if (nvn2) {
            LoadAmpere();
        } else {
            LoadMaxwell();
        }
    }

    u64 ReadInstruction(u32 address) override {
        if (address / sizeof(u64) >= code.size()) {
            throw Shader::RuntimeError("Instruction address out of range: {:#x}", address);
        }
        return code[address / sizeof(u64)];
    }

    u32 TranslateProgramAddress(u32 address) const override {
        if (!nvn2) {
            return address;
        }
        if (const auto translated = Shader::Ampere::NativeProgramAddress(code, address)) {
            return *translated;
        }
        throw Shader::RuntimeError("Invalid native indirect branch address {:#x}", address);
    }

    bool IsNativeProgram() const override {
        return nvn2;
    }

    bool HasNativeInstructions() const override {
        return nvn2;
    }

private:
    /// An NVN1 stage (Switch 1): Maxwell code after the code area's 0x30-byte header, a graphics
    /// stage's 0x50-byte SPH first, its instructions at +0x80; compute's at +0x100 after a header of
    /// its own (magic 0x12345679), at +0 without one. Addresses are the code area's, which puts the
    /// scheduling words where the frontend expects them (every 32 bytes). Control +0x748 is
    /// compute's workgroup, +0x754 its shared memory.
    void LoadMaxwell() {
        const bool compute = stage == Shader::Stage::Compute;
        const bool compute_header = source.code_size >= 4 && Read32(source.code, 0) == 0x12345679U;
        start_address = !compute ? 0x30 : compute_header ? 0x100 : 0;
        if (source.code_size < start_address + (compute ? 0 : sizeof(sph))) {
            throw Shader::RuntimeError("Code area of {:#x} bytes", source.code_size);
        }
        code.resize(source.code_size / sizeof(u64));
        std::memcpy(code.data(), source.code, code.size() * sizeof(u64));
        if (compute) {
            workgroup_size = {Read32(source.control, 0x748), Read32(source.control, 0x74c),
                              Read32(source.control, 0x750)};
            shared_memory_size = Read32(source.control, 0x754);
        } else {
            std::memcpy(&sph, source.code + start_address, sizeof(sph));
            local_memory_size = static_cast<u32>(sph.LocalMemorySize()) +
                                sph.common3.shader_local_memory_crs_size;
        }
    }

    void LoadAmpere() {
        const u8* const control = source.control;
        const bool compute = stage == Shader::Stage::Compute;
        // Graphics code areas start with a 0x80-byte header (SPH version 4, whose first 0x50
        // bytes are version 3's), compute code with its instructions; control +0x38 is the
        // length of both together.
        const u32 header_size = compute ? 0 : 0x80;
        if (!compute) {
            if (source.code_size < header_size) {
                throw Shader::RuntimeError("Code area of {:#x} bytes", source.code_size);
            }
            std::memcpy(&sph, source.code, sizeof(sph));
        }
        const u32 end = (std::min)(Read32(control, 0x38), source.code_size);
        std::vector<Instruction> native;
        Shader::Ampere::ProgramCaptureState capture;
        for (u32 index = 0;; ++index) {
            const u32 offset = header_size + index * u32{sizeof(Instruction)};
            if (offset + sizeof(Instruction) > end ||
                index >= Shader::Ampere::ProgramCaptureState::MaximumInstructions) {
                break;
            }
            Instruction instruction;
            std::memcpy(&instruction, source.code + offset, sizeof(instruction));
            native.push_back(instruction);
            const auto status = capture.Observe(instruction, index);
            if (status == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) {
                throw Shader::RuntimeError("Branch outside the program at {:#x}", offset);
            }
            if (status == Shader::Ampere::ProgramCaptureState::Result::Complete) {
                break;
            }
        }
        if (native.empty()) {
            throw Shader::RuntimeError("No instructions");
        }
        // EDEN_NX2_SHADER_DUMP=<dir>: the program as GPU emulation keeps it (shader_environment.cpp),
        // so tools/nx2/shader_sweep.py tests the lowering offline on what direct mode translates too
        if (static const char* const dump_directory = std::getenv("EDEN_NX2_SHADER_DUMP");
            dump_directory != nullptr) {
            dump_key = Shader::Ampere::NativeProgramKey(native);
            const auto path = std::filesystem::path{dump_directory} /
                              fmt::format("{:016x}-{}-{:x}.bin", dump_key, compute ? "cs" : "gfx",
                                          source.code_address);
            if (!std::filesystem::exists(path)) {
                std::filesystem::create_directories(path.parent_path());
                std::ofstream{path, std::ios::binary}.write(
                    reinterpret_cast<const char*>(native.data()), native.size() * sizeof(Instruction));
                if (header_size != 0) {
                    auto header_path = path;
                    header_path.replace_extension(".header.bin");
                    std::ofstream{header_path, std::ios::binary}.write(
                        reinterpret_cast<const char*>(source.code), header_size);
                }
            }
        }
        const std::vector<u32> traps = VideoCommon::NativeTrapOffsets(native);
        const size_t trap_count = traps.empty() ? 0 : traps.back() / sizeof(Instruction) + 1;
        if (trap_count &&
            (!compute || !Shader::Ampere::VerifyTrapGuards(
                             std::span{native}.first(trap_count),
                             [this](u32 bank, u32 offset) -> std::optional<u32> {
                                 return ReadCbufValue(bank, offset);
                             }))) {
            throw Shader::Ampere::TrapGuardError{};
        }
        const VideoCommon::NativeLoweringInputs inputs{
            .geometry = stage == Shader::Stage::Geometry,
            .tessellation_stage = stage == Shader::Stage::TessellationControl
                                      ? Shader::Ampere::TessellationStage::Control
                                  : stage == Shader::Stage::TessellationEval
                                      ? Shader::Ampere::TessellationStage::Evaluation
                                      : Shader::Ampere::TessellationStage::None,
            .native_code_address = source.code_address + header_size,
        };
        Shader::Ampere::LoweredProgram lowered = VideoCommon::LowerNativeProgram(native, inputs);
        if (!lowered) {
            const size_t failed = lowered.error_offset / sizeof(Instruction);
            const auto words = failed < native.size() ? native[failed].words : std::array<u32, 4>{};
            throw Shader::RuntimeError("Instruction {:#x}: {} [{:08x} {:08x} {:08x} {:08x}]",
                                       lowered.error_offset, lowered.error, words[0], words[1],
                                       words[2], words[3]);
        }
        const auto lowered_code =
            VideoCommon::MakeLoweredNativeCode(native, sph, header_size, traps, lowered);
        code = lowered_code->code;
        texture_bound = lowered_code->texture_bound != ~0U ? lowered_code->texture_bound : 2;
        if (compute) {
            start_address = sizeof(sph);
            workgroup_size = {Read32(control, 0x58), Read32(control, 0x5c), Read32(control, 0x60)};
            shared_memory_size = Read32(control, 0x64);
        } else {
            local_memory_size = static_cast<u32>(sph.LocalMemorySize()) +
                                sph.common3.shader_local_memory_crs_size;
        }
        if (stage == Shader::Stage::Task || stage == Shader::Stage::Mesh) {
            // The mesh registers the driver sets from the control section (translate_program.cpp
            // reads them): +0x58 the workgroup's threads, +0x5a the task memory's bytes, +0x60 /
            // +0x62 the most vertices / primitives; triangles. +0x64: the header of the
            // per-primitive outputs in the code area (0 none).
            const u32 threads = Read16(control, 0x58) & 0x3f;
            const u32 lines = ((Read16(control, 0x5a) + 127U) / 128U) & 0x1ff;
            mesh_registers = {4U | (Read16(control, 0x60) & 0x1ffU) << 4 |
                                  (Read16(control, 0x62) & 0x3ffU) << 13,
                              threads << 20, threads | lines << 12 | lines << 22};
            const u32 primitive_header = Read32(control, 0x64);
            if (primitive_header != 0 &&
                primitive_header + sizeof(mesh_primitive_header) <= source.code_size) {
                std::memcpy(&mesh_primitive_header, source.code + primitive_header,
                            sizeof(mesh_primitive_header));
            }
        }
    }

public:
    bool IsCbufStorageBufferPointer(u32 index, u32 offset) override {
        const u64 address = ReadPointer(index, offset);
        u8 byte;
        return address != 0 && source.read_memory(source.context, address, &byte, 1) != 0;
    }

    bool IsCbufIndirectStorageBufferPointer(u32 index, u32 offset, u32 indirect) override {
        if (!IsCbufStorageBufferPointer(index, offset)) {
            return false;
        }
        u64 address = 0;
        u8 byte;
        return source.read_memory(source.context, ReadPointer(index, offset) + indirect,
                                  &address, sizeof(address)) != 0 &&
               address != 0 && source.read_memory(source.context, address, &byte, 1) != 0;
    }

    u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override {
        return source.read_cbuf(source.context, cbuf_index, cbuf_offset);
    }

    Shader::TextureType ReadTextureType(u32 handle) override {
        u32 target = 1;
        s32 format = 0;
        if (!source.texture_info(source.context, handle, &target, &format)) {
            return Shader::TextureType::Color2D;
        }
        return TextureTypeOf(target);
    }

    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override {
        u32 target = 1;
        s32 format = 0;
        if (!source.texture_info(source.context, handle, &target, &format)) {
            return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
        }
        return shaders.PixelFormat(static_cast<VkFormat>(format));
    }

    bool IsTexturePixelFormatInteger(u32 handle) override {
        return VideoCore::Surface::IsPixelFormatInteger(
            static_cast<VideoCore::Surface::PixelFormat>(ReadTexturePixelFormat(handle)));
    }

    u32 ReadViewportTransformState() override {
        return 1;
    }

    u32 TextureBoundBuffer() const override {
        return texture_bound;
    }

    u32 LocalMemorySize() const override {
        return local_memory_size;
    }

    u32 SharedMemorySize() const override {
        return shared_memory_size;
    }

    std::array<u32, 3> WorkgroupSize() const override {
        return workgroup_size;
    }

    /// The dumped program's key (EDEN_NX2_SHADER_DUMP), 0 when not dumped
    u64 DumpKey() const {
        return dump_key;
    }

    bool HasHLEMacroState() const override {
        return true;
    }

    // The draw's base instance, which GLSLC adds to Maxwell's instance ID from the driver's c[0]
    // (dmsh_abi.h DMSH_C0_BASE_INSTANCE), read as the BaseInstance the Vulkan draw passes. The
    // replay fills c[0] when it records a draw, so an indirect draw's c[0] held the record's 0
    // instead of the first instance the GPU wrote into its arguments: Mario Kart World's culled
    // instanced draws (trees, bushes) took other instances' data and came and went from frame to
    // frame. A direct draw passes the same value as its first instance.
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override {
        constexpr u32 base_instance_offset = 0x644;
        if (bank == 0 && offset == base_instance_offset &&
            (stage == Shader::Stage::VertexA || stage == Shader::Stage::VertexB)) {
            return Shader::ReplaceConstant::BaseInstance;
        }
        return std::nullopt;
    }

    void Dump(u64, u64) override {}

private:
    u64 ReadPointer(u32 index, u32 offset) {
        return u64{ReadCbufValue(index, offset)} | u64{ReadCbufValue(index, offset + 4)} << 32;
    }

    const nvn_direct_shader_source& source;
    const NvnDirectShaders& shaders;
    const bool nvn2;
    std::vector<u64> code;
    u32 texture_bound{2};
    u32 local_memory_size{};
    u64 dump_key{};
    u32 shared_memory_size{};
    std::array<u32, 3> workgroup_size{};
};

/// The binding after the last one a SPIR-V module decorates in set 0 (0 = none); the pools' set
/// (NVN_DIRECT_POOL_SET) is left out
u32 BindingEnd(std::span<const u32> spirv) {
    const auto decorations = [&](auto&& visit) {
        for (size_t at = 5; at < spirv.size();) {
            const u32 count = spirv[at] >> 16, opcode = spirv[at] & 0xffff;
            if (count == 0) {
                break;
            }
            if (opcode == 71 /* OpDecorate */ && count >= 4 && at + 3 < spirv.size()) {
                visit(spirv[at + 1], spirv[at + 2], spirv[at + 3]);
            }
            at += count;
        }
    };
    std::vector<u32> other_set;
    decorations([&](u32 target, u32 decoration, u32 value) {
        if (decoration == 34 /* DescriptorSet */ && value != 0) {
            other_set.push_back(target);
        }
    });
    u32 end = 0;
    decorations([&](u32 target, u32 decoration, u32 value) {
        if (decoration == 33 /* Binding */ && std::ranges::find(other_set, target) == other_set.end()) {
            end = (std::max)(end, value + 1);
        }
    });
    return end;
}

} // Anonymous namespace

NvnDirectShaders::NvnDirectShaders(const Device& device, const Shader::Profile& profile_,
                                   const Shader::HostTranslateInfo& host_info_)
    : profile{profile_}, host_info{host_info_} {
    // The replay sets no VK_EXT_depth_clip_control: the GL depth mode is converted in the shader.
    profile.support_native_ndc = false;
    profile.support_float32_atomic_add = device.IsNvnDirectFloat32AtomicAddEnabled();
    // A texture or image handle no constant buffer word gives is read from the replay's pool tables
    // by the value the shader computes (NVN_DIRECT_POOL_SET), as on the console, not by a handle
    // program evaluated for each draw, which the replay has no means to run.
    host_info.support_pool_textures = true;
    // The replay binds at most 512 descriptors a draw (vka_draw.c), over every stage: an array of
    // handles a shader indexes (the recompiler makes it up to 1,024 long) gets at most half of
    // them, which still covers the driver's c2 handle tables up into the separate samplers' from
    // the combined slots (Luigi's Mansion 3's shadow cascades index c2 from combined slot 12).
    constexpr u32 max_stage_textures = 256;
    host_info.max_per_stage_descriptor_sampled_images =
        (std::min)(host_info.max_per_stage_descriptor_sampled_images, max_stage_textures);
    host_info.max_descriptor_set_sampled_images =
        (std::min)(host_info.max_descriptor_set_sampled_images, max_stage_textures);
    for (size_t index = 0; index < VideoCore::Surface::MaxPixelFormat; ++index) {
        const auto format = static_cast<VideoCore::Surface::PixelFormat>(index);
        const VkFormat vk_format =
            MaxwellToVK::SurfaceFormat(device, FormatType::Optimal, true, format).format;
        pixel_formats.try_emplace(vk_format, static_cast<Shader::TexturePixelFormat>(index));
    }
    // The replay's 8-bit RGBA formats: the same bytes as Lichii's packed ones.
    pixel_formats.insert_or_assign(VK_FORMAT_R8G8B8A8_UNORM, Shader::TexturePixelFormat::A8B8G8R8_UNORM);
    pixel_formats.insert_or_assign(VK_FORMAT_R8G8B8A8_SNORM, Shader::TexturePixelFormat::A8B8G8R8_SNORM);
    pixel_formats.insert_or_assign(VK_FORMAT_R8G8B8A8_SINT, Shader::TexturePixelFormat::A8B8G8R8_SINT);
    pixel_formats.insert_or_assign(VK_FORMAT_R8G8B8A8_UINT, Shader::TexturePixelFormat::A8B8G8R8_UINT);
    pixel_formats.insert_or_assign(VK_FORMAT_R8G8B8A8_SRGB, Shader::TexturePixelFormat::A8B8G8R8_SRGB);
}

Shader::TexturePixelFormat NvnDirectShaders::PixelFormat(VkFormat format) const {
    const auto it = pixel_formats.find(format);
    return it != pixel_formats.end() ? it->second : Shader::TexturePixelFormat::A8B8G8R8_UNORM;
}

int NvnDirectShaders::Translate(void* context, const nvn_direct_shader_source* source,
                                nvn_direct_shader* shader) {
    *shader = {};
    try {
        static_cast<const NvnDirectShaders*>(context)->TranslateStage(*source, *shader);
        return 1;
    } catch (const std::exception& exception) {
        Free(context, shader);
        std::snprintf(shader->error, sizeof(shader->error), "%s", exception.what());
        return 0;
    }
}

void NvnDirectShaders::Free(void*, nvn_direct_shader* shader) {
    std::free(shader->spirv);
    std::free(shader->descs);
    shader->spirv = nullptr;
    shader->descs = nullptr;
    shader->spirv_words = 0;
    shader->desc_count = 0;
}

void NvnDirectShaders::TranslateStage(const nvn_direct_shader_source& source,
                                      nvn_direct_shader& shader) const {
    NvnDirectEnvironment env{source, *this};
    VideoCommon::ShaderPools pools;
    const Shader::Stage stage = env.ShaderStage();
    const u32 cfg_offset = env.StartAddress() +
                           (stage == Shader::Stage::Compute ? 0 : u32{sizeof(Shader::ProgramHeader)});
    Shader::Maxwell::Flow::CFG cfg{env, pools.flow_block, cfg_offset};
    Shader::IR::Program program{
        Shader::Maxwell::TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};
    std::array<Shader::IR::Program*, 1> programs{&program};
    Shader::Optimization::LimitPipelineSampledTextureArrays(programs, host_info);
    const Shader::Info& info = program.info;
    // The replay's pipeline layouts have push constants for the render area of a draw (nvn_direct.h
    // NVN_DIRECT_RENDER_AREA_OFFSET), and those of the resolution scale while it scales
    // (NVN_DIRECT_SCALE_PUSH_SIZE), each stage in its own bits.
    if (info.uses_render_area && stage == Shader::Stage::Compute) {
        throw Shader::RuntimeError("Reads Lichii's render area push constants in a compute shader");
    }
    if (info.uses_rescaling_uniform &&
        (info.texture_descriptors.size() > nvn_direct_scale_texture_bits(source.nvn_stage) ||
         info.image_descriptors.size() > nvn_direct_scale_image_bits(source.nvn_stage))) {
        throw Shader::RuntimeError("{} textures and {} images, more than the resolution scale's bits for the stage",
                                   info.texture_descriptors.size(), info.image_descriptors.size());
    }

    Shader::RuntimeInfo runtime_info;
    runtime_info.previous_stage_stores.mask.set();
    if (stage == Shader::Stage::VertexB) {
        for (size_t index = 0; index < runtime_info.generic_input_types.size(); ++index) {
            const u8 type = source.vertex_input_types[index];
            runtime_info.generic_input_types[index] =
                type == 1   ? Shader::AttributeType::SignedInt
                : type == 2 ? Shader::AttributeType::UnsignedInt
                            : Shader::AttributeType::Float;
        }
    }
    if (stage == Shader::Stage::Fragment) {
        runtime_info.per_primitive_generics = source.per_primitive_inputs;
    }
    if (stage == Shader::Stage::Geometry) {
        runtime_info.input_topology = static_cast<Shader::InputTopology>(source.input_topology);
    }
    if (stage == Shader::Stage::VertexB || stage == Shader::Stage::TessellationEval ||
        stage == Shader::Stage::Geometry || stage == Shader::Stage::Mesh) {
        runtime_info.convert_depth_mode = source.convert_depth != 0;
    }
    // (not task and mesh programs: their epilogue's markers expect the program's own returns)
    const bool loop_guard = stage != Shader::Stage::Task && stage != Shader::Stage::Mesh &&
                            !Settings::values.disable_shader_loop_safety_checks &&
                            std::ranges::any_of(program.syntax_list, [](const Shader::IR::AbstractSyntaxNode& node) {
                                return node.type == Shader::IR::AbstractSyntaxNode::Type::Repeat;
                            });
    if (loop_guard) {
        runtime_info.loop_guard_binding = source.first_binding + NVN_DIRECT_LOOP_GUARD_SLOT;
    }
    Shader::Maxwell::ConvertLegacyToGeneric(program, runtime_info);
    Shader::Backend::Bindings bindings;
    bindings.unified = source.first_binding;
    bindings.texture_scaling_index = nvn_direct_scale_texture_bit(source.nvn_stage);
    bindings.image_scaling_index = nvn_direct_scale_image_bit(source.nvn_stage);
    bindings.render_area_offset = NVN_DIRECT_RENDER_AREA_OFFSET;
    bindings.pool_set = NVN_DIRECT_POOL_SET;
    bindings.pool_texture_binding = NVN_DIRECT_POOL_TEXTURE_BINDING;
    bindings.pool_sampler_binding = NVN_DIRECT_POOL_SAMPLER_BINDING;
    bindings.pool_image_binding = NVN_DIRECT_POOL_IMAGE_BINDING;
    const std::vector<u32> spirv{
        Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings)};
    if (env.DumpKey() != 0) {
        // Beside the program: its translation as the replay builds it, for spirv-dis. A program
        // has a translation per draw state (depth mode, input types, a geometry shader's input
        // primitive), which can be made at once on two threads: each is named by its SPIR-V's
        // hash, written whole under a name of its own and then renamed into place.
        u64 hash = 0xcbf29ce484222325ULL;
        for (const u32 word : spirv) {
            hash = (hash ^ word) * 0x100000001b3ULL;
        }
        const auto path = std::filesystem::path{std::getenv("EDEN_NX2_SHADER_DUMP")} /
                          fmt::format("{:016x}-direct-{}-{:016x}.spv", env.DumpKey(),
                                      source.first_binding, hash);
        auto partial = path;
        partial += fmt::format(".{}.part", std::hash<std::thread::id>{}(std::this_thread::get_id()));
        std::ofstream file{partial, std::ios::binary};
        file.write(reinterpret_cast<const char*>(spirv.data()),
                   static_cast<std::streamsize>(spirv.size() * sizeof(u32)));
        file.close();
        std::error_code error;
        if (!file || (std::filesystem::rename(partial, path, error), error)) {
            std::filesystem::remove(partial, error);
        }
        LOG_INFO(Render_Vulkan, "NVN direct: dumped program {:016x} ({}): {} SPIR-V words",
                 env.DumpKey(), static_cast<int>(stage), spirv.size());
    }

    // The descriptors in the order the SPIR-V backend numbers them (EmitContext:
    // DefineConstantBuffers, DefineStorageBuffers, DefineTextureBuffers, DefineImageBuffers,
    // DefineTextures, DefineImages).
    std::vector<nvn_direct_shader_desc> descs;
    u32 binding = source.first_binding;
    const auto add = [&](nvn_direct_shader_desc desc, u32 slots) {
        desc.binding = static_cast<u16>(binding);
        descs.push_back(desc);
        binding += slots;
    };
    const auto handle = [](nvn_direct_shader_desc desc, const auto& from) {
        desc.cbuf_index = from.cbuf_index;
        desc.cbuf_offset = from.cbuf_offset;
        desc.count = from.count;
        desc.size_shift = from.size_shift;
        if (from.handle_program != ~0U) {
            desc.unsupported = NVN_DIRECT_UNSUPPORTED_HANDLE_PROGRAM;
        }
        return desc;
    };
    u32 cbuf_slots = 0;
    for (const Shader::ConstantBufferDescriptor& cbuf : info.constant_buffer_descriptors) {
        add({.kind = NVN_DIRECT_DESC_CBUF,
             .vk_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
             .count = cbuf.count,
             .cbuf_index = cbuf.index,
             .unsupported = static_cast<u8>(cbuf.count != 1 ? NVN_DIRECT_UNSUPPORTED_ARRAY
                                                            : NVN_DIRECT_UNSUPPORTED_NONE)},
            cbuf.count);
        cbuf_slots += cbuf.count;
    }
    if (profile.support_descriptor_aliasing) {
        binding += static_cast<u32>(info.constant_buffer_descriptors.size()) - cbuf_slots;
    }
    for (const Shader::StorageBufferDescriptor& ssbo : info.storage_buffers_descriptors) {
        add({.kind = NVN_DIRECT_DESC_STORAGE,
             .vk_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .count = 1,
             .cbuf_index = ssbo.cbuf_index,
             .cbuf_offset = ssbo.cbuf_offset,
             .is_written = ssbo.is_written,
             .indirect_offset = ssbo.indirect_offset},
            ssbo.count);
    }
    for (const Shader::TextureBufferDescriptor& texbuf : info.texture_buffer_descriptors) {
        nvn_direct_shader_desc desc{.kind = NVN_DIRECT_DESC_TEXTURE_BUFFER,
                                    .vk_type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                    .texture_type = static_cast<u8>(Shader::TextureType::Buffer)};
        desc = handle(desc, texbuf);
        desc.shift_left = texbuf.shift_left;
        desc.has_secondary = texbuf.has_secondary;
        add(desc, 1);
    }
    for (const Shader::ImageBufferDescriptor& imgbuf : info.image_buffer_descriptors) {
        nvn_direct_shader_desc desc{.kind = NVN_DIRECT_DESC_IMAGE_BUFFER,
                                    .vk_type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
                                    .texture_type = static_cast<u8>(Shader::TextureType::Buffer),
                                    .is_integer = imgbuf.is_integer,
                                    .is_written = imgbuf.is_written};
        desc = handle(desc, imgbuf);
        if (!desc.unsupported && imgbuf.count != 1) {
            desc.unsupported = NVN_DIRECT_UNSUPPORTED_ARRAY;
        }
        add(desc, 1);
    }
    for (const Shader::TextureDescriptor& texture : info.texture_descriptors) {
        nvn_direct_shader_desc desc{.kind = NVN_DIRECT_DESC_TEXTURE,
                                    .vk_type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                    .texture_type = static_cast<u8>(texture.type),
                                    .is_depth = texture.is_depth,
                                    .is_multisample = texture.is_multisample,
                                    .is_integer = texture.is_integer};
        desc = handle(desc, texture);
        desc.shift_left = texture.shift_left;
        desc.has_secondary = texture.has_secondary;
        desc.secondary_index = texture.secondary_cbuf_index;
        desc.secondary_offset = texture.secondary_cbuf_offset;
        desc.secondary_shift_left = texture.secondary_shift_left;
        if (texture.handle_is_pointer_word) {
            desc.unsupported = NVN_DIRECT_UNSUPPORTED_POINTER_WORD;
        }
        desc.pool = texture.is_pool;
        add(desc, 1);
    }
    for (const Shader::ImageDescriptor& image : info.image_descriptors) {
        nvn_direct_shader_desc desc{.kind = NVN_DIRECT_DESC_IMAGE,
                                    .vk_type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                    .texture_type = static_cast<u8>(image.type),
                                    .is_integer = image.is_integer,
                                    .is_written = image.is_written,
                                    .is_atomic = info.uses_atomic_image_u32 && image.is_integer};
        desc = handle(desc, image);
        if (!desc.unsupported && image.count != 1) {
            desc.unsupported = NVN_DIRECT_UNSUPPORTED_ARRAY;
        } else if (!desc.unsupported && image.is_byte_addressed) {
            desc.unsupported = NVN_DIRECT_UNSUPPORTED_BYTE_ADDRESSED;
        }
        desc.pool = image.is_pool;
        add(desc, 1);
    }
    if (binding > source.first_binding + NVN_DIRECT_LOOP_GUARD_SLOT) {
        throw Shader::RuntimeError("{} bindings, the stage's range has {}", binding - source.first_binding,
                                   NVN_DIRECT_LOOP_GUARD_SLOT);
    }
    if (loop_guard) {
        binding = source.first_binding + NVN_DIRECT_LOOP_GUARD_SLOT;
        add({.kind = NVN_DIRECT_DESC_LOOP_GUARD, .vk_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1}, 1);
    }
    if (const u32 end = BindingEnd(spirv); end > binding) {
        throw Shader::RuntimeError("The SPIR-V binds up to {}, its descriptors end at {}", end,
                                   binding);
    }

    shader.spirv = static_cast<u32*>(std::malloc(spirv.size() * sizeof(u32)));
    shader.descs = static_cast<nvn_direct_shader_desc*>(
        std::malloc((std::max)(descs.size(), size_t{1}) * sizeof(nvn_direct_shader_desc)));
    if (!shader.spirv || !shader.descs) {
        throw Shader::RuntimeError("Out of memory");
    }
    std::memcpy(shader.spirv, spirv.data(), spirv.size() * sizeof(u32));
    shader.spirv_words = spirv.size();
    std::memcpy(shader.descs, descs.data(), descs.size() * sizeof(nvn_direct_shader_desc));
    shader.desc_count = static_cast<u32>(descs.size());
    shader.local[0] = program.workgroup_size[0];
    shader.local[1] = program.workgroup_size[1];
    shader.local[2] = program.workgroup_size[2];
    shader.shared = program.shared_memory_size;
    for (u32 index = 0; index < 32; ++index) {
        if (stage == Shader::Stage::VertexB && info.loads.Generic(index)) {
            shader.input_mask |= 1U << index;
        }
        if (stage == Shader::Stage::Fragment && index < info.stores_frag_color.size() &&
            info.stores_frag_color[index]) {
            shader.output_mask |= 1U << index;
        }
    }
    for (const u32 line : program.mesh.primitive_lines) {
        const auto attribute{static_cast<Shader::IR::Attribute>(line)};
        if (stage == Shader::Stage::Mesh && Shader::IR::IsGeneric(attribute)) {
            shader.per_primitive_outputs |= 1U << Shader::IR::GenericAttributeIndex(attribute);
        }
    }
    shader.ssbo_align = static_cast<u32>(host_info.min_ssbo_alignment);
    shader.uses_render_area = info.uses_render_area ? 1 : 0;
}

} // namespace Vulkan

#endif
