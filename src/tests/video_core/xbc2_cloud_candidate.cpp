// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

#include <sirit/sirit.h>
#include <spirv/unified1/GLSL.std.450.h>
#include <spirv/unified1/spirv.hpp11>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/textures/depth_aspect.h"
#include "video_core/xc2_cloud_policy.h"

namespace {
namespace IR = Shader::IR;
using PixelFormat = VideoCore::Surface::PixelFormat;
using Swizzle = Tegra::Texture::SwizzleSource;

class TextureEnvironment final : public Shader::Environment {
public:
    PixelFormat format{PixelFormat::X8_D24_UNORM};
    std::array<Swizzle, 4> swizzle{Swizzle::R, Swizzle::R, Swizzle::Zero, Swizzle::OneFloat};
    bool integer{};
    TextureEnvironment() {
        stage = Shader::Stage::Fragment;
    }
    u32 ReadCbufValue(u32, u32 offset) override {
        return offset;
    }
    Shader::TextureType ReadTextureType(u32) override {
        return Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return static_cast<Shader::TexturePixelFormat>(format);
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return integer;
    }
    u64 ReadInstruction(u32) override {
        throw std::logic_error("Unexpected instruction read");
    }
    u32 TextureBoundBuffer() const override {
        return 0;
    }
    u32 LocalMemorySize() const override {
        return 0;
    }
    u32 SharedMemorySize() const override {
        return 0;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {1, 1, 1};
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    void Dump(u64, u64) override {}
    u32 ReadViewportTransformState() override {
        return 1;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
};

struct Program {
    Shader::ObjectPool<IR::Inst> pool{512};
    IR::Block block{pool};
    IR::Program ir;
    explicit Program(Shader::Stage stage = Shader::Stage::Fragment) {
        ir.stage = stage;
        ir.blocks = {&block};
        ir.post_order_blocks = {&block};
        ir.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                          {{}, IR::AbstractSyntaxNode::Type::Return}};
    }
    std::vector<u32> Emit(u32 texture_index = 0) {
        Shader::Optimization::DeadCodeEliminationPass(ir);
        for (const auto* current_block : ir.blocks) {
            for (const auto& inst : current_block->Instructions()) {
                if (inst.GetOpcode() == IR::Opcode::SetFragColor) {
                    REQUIRE(ir.info.stores_frag_color.at(inst.Arg(0).U32()));
                }
            }
        }
        Shader::Profile profile;
        profile.supported_spirv = 0x00010300;
        profile.unified_descriptor_binding = true;
        Shader::RuntimeInfo runtime;
        runtime.previous_stage_stores = ir.info.loads;
        Shader::Backend::Bindings bindings;
        bindings.texture_scaling_index = texture_index;
        auto code = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, ir, bindings);
        REQUIRE(code.size() > 5);
        if (const char* directory = std::getenv("XBC2_TEST_SPIRV_DIR")) {
            static u32 module_index{};
            const std::filesystem::path path{directory};
            REQUIRE_FALSE(path.empty());
            std::filesystem::create_directories(path);
            const auto filename = path / ("xbc2-" + std::to_string(module_index++) + ".spv");
            std::ofstream output{filename, std::ios::binary};
            REQUIRE(output.is_open());
            output.write(reinterpret_cast<const char*>(code.data()),
                         static_cast<std::streamsize>(code.size() * sizeof(u32)));
            output.close();
            REQUIRE(output.good());
        }
        return code;
    }
};

// Execute the emitted helper's limited SPIR-V instruction set, not a duplicate quantizer.
// Unknown instructions fail the test. This does not simulate GPU float modes or rasterization.
struct Depth24Helper {
    using Value = std::array<u32, 4>;
    using Words = std::vector<u32>;
    std::map<u32, Value> constants;
    std::vector<Words> body;
    std::map<u32, size_t> labels;

    explicit Depth24Helper(const std::vector<u32>& code) {
        std::map<u32, std::vector<Words>> functions;
        u32 current{}, called{};
        for (size_t offset = 5; offset < code.size();) {
            const u32 count = code[offset] >> 16;
            REQUIRE(count > 0);
            REQUIRE(offset + count <= code.size());
            Words op(code.begin() + offset, code.begin() + offset + count);
            const auto opcode = static_cast<spv::Op>(op[0] & 0xffffU);
            if (opcode == spv::Op::OpFunction)
                current = op[2];
            else if (opcode == spv::Op::OpFunctionEnd)
                current = 0;
            else if (current)
                functions[current].push_back(op);
            if (opcode == spv::Op::OpFunctionCall)
                called = op[3];
            if (opcode == spv::Op::OpConstant)
                constants[op[2]] = {op[3], 0, 0, 0};
            if (opcode == spv::Op::OpConstantTrue || opcode == spv::Op::OpConstantFalse)
                constants[op[2]] = {opcode == spv::Op::OpConstantTrue ? 1U : 0U, 0, 0, 0};
            offset += count;
        }
        REQUIRE(called != 0);
        body = functions.at(called);
        for (size_t i = 0; i < body.size(); ++i) {
            if (static_cast<spv::Op>(body[i][0] & 0xffffU) == spv::Op::OpLabel)
                labels[body[i][1]] = i;
        }
    }

    std::array<float, 4> Run(std::array<float, 4> input, bool enabled,
                             size_t* rounded_components = nullptr) const {
        auto values = constants;
        size_t parameter{}, rounded{};
        for (size_t pc = 0; pc < body.size(); ++pc) {
            const auto& op = body[pc];
            const auto opcode = static_cast<spv::Op>(op[0] & 0xffffU);
            const auto u = [&](size_t operand) { return values.at(op.at(operand))[0]; };
            const auto f = [&](size_t operand) { return std::bit_cast<float>(u(operand)); };
            const auto put_u = [&](u32 value) { values[op[2]] = {value, 0, 0, 0}; };
            const auto put_f = [&](float value) { put_u(std::bit_cast<u32>(value)); };
            switch (opcode) {
            case spv::Op::OpFunctionParameter:
                values[op[2]] = parameter++ == 0 ? std::bit_cast<Value>(input)
                                               : Value{enabled ? 1U : 0U, 0, 0, 0};
                break;
            case spv::Op::OpLabel:
            case spv::Op::OpSelectionMerge:
                break;
            case spv::Op::OpBranchConditional:
                pc = labels.at(op[u(1) ? 2 : 3]);
                break;
            case spv::Op::OpCompositeExtract:
                put_u(values.at(op[3]).at(op[4]));
                break;
            case spv::Op::OpCompositeConstruct:
                REQUIRE(op.size() == 7);
                values[op[2]] = {u(3), u(4), u(5), u(6)};
                break;
            case spv::Op::OpFOrdGreaterThanEqual:
                put_u(f(3) >= f(4));
                break;
            case spv::Op::OpFOrdLessThanEqual:
                put_u(f(3) <= f(4));
                break;
            case spv::Op::OpLogicalAnd:
                put_u(u(3) && u(4));
                break;
            case spv::Op::OpSelect:
                values[op[2]] = values.at(op[u(3) ? 4 : 5]);
                break;
            case spv::Op::OpFMul:
                put_f(f(3) * f(4));
                break;
            case spv::Op::OpExtInst:
                REQUIRE(op[4] == GLSLstd450RoundEven);
                put_f(std::nearbyint(f(5)));
                ++rounded;
                break;
            case spv::Op::OpConvertFToU:
                REQUIRE(std::isfinite(f(3)));
                REQUIRE(f(3) >= 0);
                REQUIRE(f(3) <= 16777215.0f);
                put_u(static_cast<u32>(f(3)));
                break;
            case spv::Op::OpConvertUToF:
                put_f(static_cast<float>(u(3)));
                break;
            case spv::Op::OpBitcast:
                put_u(u(3));
                break;
            case spv::Op::OpINotEqual:
                put_u(u(3) != u(4));
                break;
            case spv::Op::OpIAdd:
                put_u(u(3) + u(4));
                break;
            case spv::Op::OpReturnValue:
                if (rounded_components)
                    *rounded_components = rounded;
                return std::bit_cast<std::array<float, 4>>(values.at(op[1]));
            default:
                throw std::logic_error("Unexpected D24 helper SPIR-V opcode " +
                                       std::to_string(static_cast<u32>(opcode)));
            }
        }
        throw std::logic_error("D24 helper did not return");
    }
};

const Depth24Helper& EmittedDepth24Helper() {
    static const Depth24Helper helper = [] {
        Program program;
        program.ir.info.uses_depth24_fetch = true;
        program.ir.info.stores_frag_color[0] = true;
        IR::IREmitter ir{program.block};
        const auto input = ir.CompositeConstruct(ir.Imm32(0.0f), ir.Imm32(1.0f),
                                                ir.Imm32(0.5f), ir.Imm32(-1.0f));
        const auto result = ir.QuantizeDepth24(input, ir.Imm1(true));
        for (size_t c = 0; c < 4; ++c)
            ir.SetFragColor(0, c, IR::F32{ir.CompositeExtract(result, c)});
        return Depth24Helper{program.Emit()};
    }();
    return helper;
}

// Evaluate the actual rewritten scalar IR; this does not simulate host rasterization.
struct Evaluate {
    std::map<IR::Attribute, float> attributes;
    bool depth24{true};
    bool point_depth24{true};
    std::array<float, 4> viewport{0, 0, 1280, 720};
    std::array<float, 4> fetch{0.12345678f, 0.12345678f, 0.0f, 1.0f};
    u32 Integer(const IR::Value& source) {
        const IR::Value value = source.IsImmediate() ? source : source.Resolve();
        if (value.IsImmediate())
            return value.U32();
        const IR::Inst& inst = *value.InstRecursive();
        switch (inst.GetOpcode()) {
        case IR::Opcode::ConvertU32F32:
            return static_cast<u32>(Scalar(inst.Arg(0)));
        case IR::Opcode::BitCastU32F32:
            return std::bit_cast<u32>(Scalar(inst.Arg(0)));
        case IR::Opcode::IAdd32:
            return Integer(inst.Arg(0)) + Integer(inst.Arg(1));
        case IR::Opcode::SelectU32:
            return Scalar(inst.Arg(0)) != 0 ? Integer(inst.Arg(1)) : Integer(inst.Arg(2));
        default:
            throw std::logic_error(std::string{IR::NameOf(inst.GetOpcode())});
        }
    }
    float Scalar(const IR::Value& source) {
        const IR::Value value = source.IsImmediate() ? source : source.Resolve();
        if (value.IsImmediate()) {
            if (value.Type() == IR::Type::U1)
                return value.U1() ? 1.0f : 0.0f;
            return value.F32();
        }
        const IR::Inst& inst = *value.InstRecursive();
        const auto a = [&](size_t index) { return Scalar(inst.Arg(index)); };
        switch (inst.GetOpcode()) {
        case IR::Opcode::IsTextureDepth24:
            return depth24;
        case IR::Opcode::IsTexturePointDepth24:
            return point_depth24;
        case IR::Opcode::GetAttribute:
            return attributes.at(inst.Arg(0).Attribute());
        case IR::Opcode::BitCastF32U32:
            return std::bit_cast<float>(Integer(inst.Arg(0)));
        case IR::Opcode::ConvertF32U32:
            return static_cast<float>(Integer(inst.Arg(0)));
        case IR::Opcode::INotEqual:
            return Integer(inst.Arg(0)) != Integer(inst.Arg(1));
        case IR::Opcode::FPAdd32:
            return a(0) + a(1);
        case IR::Opcode::FPMul32:
            return a(0) * a(1);
        case IR::Opcode::FPNeg32:
            return -a(0);
        case IR::Opcode::FPRecip32:
            return 1.0f / a(0);
        case IR::Opcode::FPFma32:
            return std::fma(a(0), a(1), a(2));
        case IR::Opcode::FPRoundEven32:
            return std::nearbyint(a(0));
        case IR::Opcode::FPOrdGreaterThanEqual32:
            return a(0) >= a(1);
        case IR::Opcode::FPOrdLessThanEqual32:
            return a(0) <= a(1);
        case IR::Opcode::LogicalAnd:
            return a(0) != 0 && a(1) != 0;
        case IR::Opcode::SelectF32:
            return a(0) != 0 ? a(1) : a(2);
        case IR::Opcode::CompositeExtractF32x4: {
            const IR::Inst& vector = *inst.Arg(0).InstRecursive();
            const auto component = inst.Arg(1).U32();
            if (vector.GetOpcode() == IR::Opcode::HostViewport)
                return viewport.at(component);
            if (vector.GetOpcode() == IR::Opcode::QuantizeDepth24) {
                const auto source = vector.Arg(0).InstRecursive()->GetOpcode();
                REQUIRE((source == IR::Opcode::ImageFetch ||
                         source == IR::Opcode::ImageSampleImplicitLod ||
                         source == IR::Opcode::ImageSampleExplicitLod));
                return EmittedDepth24Helper().Run(fetch, Scalar(vector.Arg(1)) != 0).at(component);
            }
            if (vector.GetOpcode() == IR::Opcode::ImageFetch ||
                vector.GetOpcode() == IR::Opcode::ImageSampleImplicitLod ||
                vector.GetOpcode() == IR::Opcode::ImageSampleExplicitLod)
                return fetch.at(component);
            if (vector.GetOpcode() == IR::Opcode::CompositeConstructF32x4)
                return Scalar(vector.Arg(component));
            throw std::logic_error("Unexpected vector opcode");
        }
        default:
            throw std::logic_error(std::string{IR::NameOf(inst.GetOpcode())});
        }
    }
};

size_t Count(const Program& program, IR::Opcode opcode) {
    size_t count{};
    for (const IR::Inst& inst : program.block.Instructions())
        count += inst.GetOpcode() == opcode;
    return count;
}

IR::Value Fetch(Program& p, bool sampled = false, bool implicit = false) {
    IR::IREmitter ir{p.block};
    IR::TextureInstInfo flags{};
    flags.type.Assign(Shader::TextureType::Color2D);
    const auto handle = ir.GetCbuf(ir.Imm32(0), ir.Imm32(0));
    const auto coords = ir.CompositeConstruct(ir.Imm32(0.5f), ir.Imm32(0.5f));
    const IR::Value result =
        !sampled   ? ir.ImageFetch(handle, ir.CompositeConstruct(ir.Imm32(10U), ir.Imm32(20U)), {},
                                   ir.Imm32(0U), {}, flags)
        : implicit ? ir.ImageSampleImplicitLod(handle, coords, {}, {}, {}, flags)
                   : ir.ImageSampleExplicitLod(handle, coords, ir.Imm32(0.0f), {}, flags);
    for (size_t c = 0; c < 4; ++c)
        ir.SetFragColor(0, c, IR::F32{ir.CompositeExtract(result, c)});
    p.ir.info.stores_frag_color[0] = true;
    return result;
}

void PositionPair(Program& vertex, Program& fragment, bool offset,
                  std::optional<IR::F32> custom_offset = std::nullopt, bool fused = false) {
    IR::IREmitter vs{vertex.block};
    IR::IREmitter fs{fragment.block};
    for (size_t c : {0U, 1U, 3U}) {
        const auto attr = IR::Attribute::Generic0X + c;
        const auto pos = IR::Attribute::PositionX + c;
        const auto input = IR::Attribute::Generic1X + c;
        const IR::F32 value = vs.GetAttribute(input);
        IR::F32 position = value;
        if (offset && c != 3) {
            const IR::F32 amount = custom_offset.value_or(vs.Imm32(0.125f));
            position = fused ? IR::F32{vs.FPFma(amount, vs.Imm32(1000000.0f), value)}
                             : IR::F32{vs.FPAdd(value, amount)};
        }
        vs.SetAttribute(attr, value, vs.Imm32(0));
        vs.SetAttribute(pos, position, vs.Imm32(0));
        vertex.ir.info.loads.Set(input);
        vertex.ir.info.stores.Set(attr);
        vertex.ir.info.stores.Set(pos);
        fragment.ir.info.loads.Set(attr);
        fs.SetFragColor(0, c, fs.GetAttribute(attr));
    }
    fragment.ir.info.interpolation[0] = Shader::Interpolation::Smooth;
    fragment.ir.info.stores_frag_color[0] = true;
}
} // namespace

TEST_CASE("D24 sampling aspect agrees with packed depth stencil swizzles", "[xbc2][depth24]") {
    for (const auto r : {Swizzle::R, Swizzle::G, Swizzle::Zero, Swizzle::OneFloat}) {
        for (const auto g : {Swizzle::R, Swizzle::G, Swizzle::Zero, Swizzle::OneFloat}) {
            const std::array swizzle{r, g, Swizzle::Zero, Swizzle::OneFloat};
            const bool any_r = r == Swizzle::R || g == Swizzle::R;
            CHECK(Tegra::Texture::SamplesDepth24(PixelFormat::D24_UNORM_S8_UINT, swizzle) == any_r);
            CHECK(Tegra::Texture::SamplesDepth24(PixelFormat::S8_UINT_D24_UNORM, swizzle) ==
                  !any_r);
            CHECK(Tegra::Texture::SamplesDepth24(PixelFormat::X8_D24_UNORM, swizzle));
            for (auto format :
                 {PixelFormat::D16_UNORM, PixelFormat::D32_FLOAT, PixelFormat::D32_FLOAT_S8_UINT,
                  PixelFormat::S8_UINT, PixelFormat::A8B8G8R8_UNORM})
                CHECK_FALSE(Tegra::Texture::SamplesDepth24(format, swizzle));
        }
    }
}

TEST_CASE("D24 candidate guards floating point texture reads independently", "[xbc2][depth24]") {
    for (auto format :
         {PixelFormat::X8_D24_UNORM, PixelFormat::D24_UNORM_S8_UINT, PixelFormat::S8_UINT_D24_UNORM,
          PixelFormat::D16_UNORM, PixelFormat::D32_FLOAT, PixelFormat::A8B8G8R8_UNORM}) {
        for (const bool enabled : {false, true}) {
            for (const bool filtered : {false, true}) {
                for (const bool integer : {false, true}) {
                    CAPTURE(format, enabled, filtered, integer);
                    TextureEnvironment env;
                    env.format = format;
                    env.integer = integer;
                    Program program;
                    Fetch(program, filtered);
                    Shader::HostTranslateInfo host;
                    host.ApplyDescriptorLimitPolicy();
                    host.emulate_depth24_reads = enabled;
                    Shader::Optimization::TexturePass(env, program.ir, host);
                    const bool changed = enabled && !integer;
                    CHECK(Count(program, IR::Opcode::QuantizeDepth24) == (changed ? 1 : 0));
                    CHECK(Count(program, IR::Opcode::IsTextureDepth24) ==
                          (changed && !filtered ? 1 : 0));
                    CHECK(Count(program, IR::Opcode::IsTexturePointDepth24) ==
                          (changed && filtered ? 1 : 0));
                    if (changed) {
                        // Ordinary depth fetches carry no depth-comparison flag.
                        CHECK_FALSE(program.ir.info.texture_descriptors.front().is_depth);
                        program.Emit();
                    }
                }
            }
        }
    }
}

TEST_CASE("D24 candidate leaves render-area and compute shaders alone", "[xbc2][depth24]") {
    for (const bool render_area : {false, true}) {
        TextureEnvironment env;
        Program program{render_area ? Shader::Stage::Fragment : Shader::Stage::Compute};
        program.ir.info.uses_render_area = render_area;
        Fetch(program);
        Shader::HostTranslateInfo host;
        host.ApplyDescriptorLimitPolicy();
        host.emulate_depth24_reads = true;
        Shader::Optimization::TexturePass(env, program.ir, host);
        CHECK_FALSE(program.ir.info.uses_depth24_fetch);
        CHECK(Count(program, IR::Opcode::IsTextureDepth24) == 0);
    }
}

TEST_CASE("D24 rewritten IR preserves swizzles and normalized endpoints", "[xbc2][depth24]") {
    TextureEnvironment env;
    Program program;
    const auto result = Fetch(program);
    Shader::HostTranslateInfo host;
    host.ApplyDescriptorLimitPolicy();
    host.emulate_depth24_reads = true;
    Shader::Optimization::TexturePass(env, program.ir, host);
    IR::IREmitter ir{program.block};
    Evaluate eval;
    for (const float depth :
         {0.0f, 1.0f, 0.000001f, 0.125f, 0.12345678f, 0.999999f, -1.0f, 255.0f}) {
        CAPTURE(depth);
        eval.fetch = {depth, depth, 0.0f, 1.0f};
        const double expected =
            depth >= 0 && depth <= 1
                ? std::nearbyint(static_cast<double>(depth) * 16777215.0) / 16777215.0
                : depth;
        for (size_t c : {0U, 1U}) {
            const auto value = ir.CompositeExtract(result, c);
            CHECK(eval.Scalar(value) == Catch::Approx(expected).margin(6.0e-8));
        }
        CHECK(eval.Scalar(ir.CompositeExtract(result, 2)) == 0.0f);
        CHECK(eval.Scalar(ir.CompositeExtract(result, 3)) == 1.0f);
    }
}

TEST_CASE("D24 rewritten IR normalizes packed values bit exactly", "[xbc2][depth24]") {
    TextureEnvironment env;
    Program program;
    const auto result = Fetch(program);
    Shader::HostTranslateInfo host;
    host.ApplyDescriptorLimitPolicy();
    host.emulate_depth24_reads = true;
    Shader::Optimization::TexturePass(env, program.ir, host);
    REQUIRE(Count(program, IR::Opcode::QuantizeDepth24) == 1);
    IR::IREmitter ir{program.block};
    const auto red = ir.CompositeExtract(result, 0);
    Evaluate eval;
    const auto check_packed = [&](u32 packed) {
        const float expected = static_cast<float>(static_cast<double>(packed) / 16777215.0);
        eval.fetch[0] = expected;
        CHECK(std::bit_cast<u32>(eval.Scalar(red)) == std::bit_cast<u32>(expected));
    };
    for (u32 packed : {0U, 1U, 2U, 0x7fffffU, 0x800000U, 0xfeffffU, 0xff0000U, 0xff0001U, 0xfffffeU,
                       0xffffffU}) {
        CAPTURE(packed);
        check_packed(packed);
    }
    for (u32 bit = 1; bit < 24; ++bit) {
        for (int delta = -1; delta <= 1; ++delta)
            check_packed((1U << bit) + delta);
    }
    for (u32 packed = 0; packed < 0x1000000U; packed += 4093)
        check_packed(packed);
    for (float special : {-1.0f, 255.0f, std::numeric_limits<float>::infinity(),
                          std::bit_cast<float>(0x7fc12345U)}) {
        eval.fetch[0] = special;
        CHECK(std::bit_cast<u32>(eval.Scalar(red)) == std::bit_cast<u32>(special));
    }
    program.Emit();
}

TEST_CASE("D24 helper bypasses colour arithmetic and preserves exceptional bit patterns",
          "[xbc2][depth24][spirv]") {
    const auto& helper = EmittedDepth24Helper();
    for (const bool enabled : {false, true}) {
        for (const float value : {0.0f, -0.0f, 1.0f, 0.12345678f, -1.0f, 2.0f,
                                 std::numeric_limits<float>::infinity(),
                                 -std::numeric_limits<float>::infinity(),
                                 std::bit_cast<float>(0x7fc12345U),
                                 std::bit_cast<float>(0xffc54321U)}) {
            CAPTURE(enabled, std::bit_cast<u32>(value));
            size_t rounded{};
            const auto result = helper.Run({value, value, value, value}, enabled, &rounded);
            CHECK(rounded == (enabled ? 4 : 0));
            float expected = value;
            if (enabled && value >= 0 && value <= 1) {
                const auto packed = static_cast<u32>(std::nearbyint(value * 16777215.0f));
                expected = static_cast<float>(static_cast<double>(packed) / 16777215.0);
            }
            for (const float actual : result)
                CHECK(std::bit_cast<u32>(actual) == std::bit_cast<u32>(expected));
        }
    }
}

TEST_CASE("D24 helper keeps caller loop and selection Phi predecessors intact",
          "[xbc2][depth24][spirv]") {
    Program program;
    IR::Block header{program.pool}, body{program.pool}, branch{program.pool},
        merge{program.pool}, back{program.pool}, exit{program.pool};
    auto& entry = program.block;
    entry.AddBranch(&header);
    header.AddBranch(&body);
    body.AddBranch(&branch);
    body.AddBranch(&merge);
    branch.AddBranch(&merge);
    merge.AddBranch(&back);
    back.AddBranch(&header);
    back.AddBranch(&exit);
    program.ir.blocks = {&entry, &header, &body, &branch, &merge, &back, &exit};
    program.ir.post_order_blocks = {&exit, &back, &merge, &branch, &body, &header, &entry};
    for (u32 i = 0; i < program.ir.blocks.size(); ++i)
        program.ir.blocks[i]->SetOrder(i);
    IR::IREmitter first{entry};
    const auto handle = first.GetCbuf(first.Imm32(0U), first.Imm32(0U));
    header.AppendNewInst(IR::Opcode::Phi, {});
    auto& iteration = header.back();
    iteration.SetFlags(IR::Type::U32);
    const auto sample = [&](IR::Block& block) {
        IR::IREmitter ir{block};
        IR::TextureInstInfo flags{};
        flags.type.Assign(Shader::TextureType::Color2D);
        return ir.ImageSampleImplicitLod(
            handle, ir.CompositeConstruct(ir.Imm32(0.25f), ir.Imm32(0.75f)), {}, {}, {}, flags);
    };
    const auto before_branch = sample(body);
    IR::IREmitter conditional{body};
    const auto condition = conditional.ConditionRef(
        conditional.IsTexturePointDepth24(conditional.Imm32(0U)));
    const auto in_branch = sample(branch);
    merge.AppendNewInst(IR::Opcode::Phi, {});
    auto& selected = merge.back();
    selected.SetFlags(IR::Type::F32x4);
    selected.AddPhiOperand(&body, before_branch);
    selected.AddPhiOperand(&branch, in_branch);
    IR::IREmitter next{back};
    const IR::U32 incremented{next.IAdd(IR::U32{IR::Value{&iteration}}, next.Imm32(1U))};
    iteration.AddPhiOperand(&entry, first.Imm32(0U));
    iteration.AddPhiOperand(&back, incremented);
    const auto repeat = next.ConditionRef(next.INotEqual(incremented, next.Imm32(2U)));
    for (size_t c = 0; c < 4; ++c)
        next.SetFragColor(0, c, IR::F32{next.CompositeExtract(IR::Value{&selected}, c)});
    program.ir.info.stores_frag_color[0] = true;
    using Node = IR::AbstractSyntaxNode;
    program.ir.syntax_list = {
        {{.block = &entry}, Node::Type::Block},
        {{.block = &header}, Node::Type::Block},
        {{.loop{&body, &back, &exit}}, Node::Type::Loop},
        {{.block = &body}, Node::Type::Block},
        {{.if_node{condition, &branch, &merge}}, Node::Type::If},
        {{.block = &branch}, Node::Type::Block},
        {{.end_if{&merge}}, Node::Type::EndIf},
        {{.block = &merge}, Node::Type::Block},
        {{.block = &back}, Node::Type::Block},
        {{.repeat{repeat, &header, &exit}}, Node::Type::Repeat},
        {{.block = &exit}, Node::Type::Block},
        {{}, Node::Type::Return},
    };
    TextureEnvironment env;
    Shader::HostTranslateInfo host;
    host.ApplyDescriptorLimitPolicy();
    host.emulate_depth24_reads = true;
    Shader::Optimization::TexturePass(env, program.ir, host);
    const auto code = program.Emit();
    size_t calls{}, samples{}, phis{};
    for (size_t offset = 5; offset < code.size();) {
        const u32 count = code[offset] >> 16;
        REQUIRE(count > 0);
        const auto opcode = static_cast<spv::Op>(code[offset] & 0xffffU);
        if (opcode == spv::Op::OpImageSampleImplicitLod)
            ++samples;
        if (opcode == spv::Op::OpFunctionCall) {
            ++calls;
            CHECK(samples >= calls); // Texture instructions are emitted before their helper call.
        }
        if (opcode == spv::Op::OpPhi) {
            ++phis;
            REQUIRE(count == 7);
            const u32 parent_a = code[offset + 4], parent_b = code[offset + 6];
            const bool loop_phi = parent_a == entry.Definition<Sirit::Id>().value &&
                                  parent_b == back.Definition<Sirit::Id>().value;
            const bool selection_phi = parent_a == body.Definition<Sirit::Id>().value &&
                                       parent_b == branch.Definition<Sirit::Id>().value;
            CHECK((loop_phi || selection_phi));
        }
        offset += count;
    }
    CHECK(samples == 2);
    CHECK(calls == 2);
    CHECK(phis == 2);
}

TEST_CASE("D24 compute fixtures export the real helper for device validation",
          "[xbc2][depth24][spirv][fixture]") {
    // Contract shared with verify_xbc2_d24_fixture.py: 64 invocations, two u32 outputs per lane.
    // Normalized D24 boundary values include the reciprocal-rounding regression at 0xff0000.
    constexpr std::array<u32, 32> inputs{
        0x00000000U, 0x80000000U, 0x3f800000U, 0x3f000000U,
        0x3e000000U, 0x3dfcd6e9U, 0x3f7fffffU, 0x3f800001U,
        0xbf800000U, 0x40000000U, 0x7f800000U, 0xff800000U,
        0x7fc12345U, 0xffc54321U, 0x00800000U, 0x80800000U,
        0x33800001U, 0x34000001U, 0x3efffffdU, 0x3effffffU,
        0x3f000001U, 0x3f000002U, 0x3f7f0000U, 0x3f7f0001U,
        0x3f7f0002U, 0x3f7fffffU, 0x358637bdU, 0x38d1b717U,
        0x3f7fffefU, 0x3eaaaaabU, 0x3f000001U, 0x3effffffU,
    };
    const std::array names{"uniform-off", "uniform-on", "lane-odd"};
    for (size_t mode = 0; mode < names.size(); ++mode) {
        CAPTURE(mode);
        Program fixture{Shader::Stage::Compute};
        fixture.ir.workgroup_size = {64, 1, 1};
        auto& info = fixture.ir.info;
        info.uses_depth24_fetch = true; // Exercise the same helper without enabling TexturePass.
        info.uses_local_invocation_id = true;
        info.uses_workgroup_id = mode != 2;
        info.used_storage_buffer_types = IR::Type::U32;
        info.storage_buffers_descriptors = {{0, 0, 1, true}};
        info.uses_fp32_denorms_flush = true; // Exercise the helper with supported FP32 flush controls.
        IR::IREmitter ir{fixture.block};
        const auto lane = ir.LocalInvocationIdX();
        const auto component = [&](u32 shift) {
            const IR::U32 index{ir.BitwiseAnd(IR::U32{ir.IAdd(lane, ir.Imm32(shift))},
                                             ir.Imm32(31U))};
            IR::U32 bits = ir.Imm32(inputs[0]);
            for (u32 i = 1; i < inputs.size(); ++i)
                bits = IR::U32{ir.Select(ir.IEqual(index, ir.Imm32(i)), ir.Imm32(inputs[i]), bits)};
            return ir.BitCast<IR::F32>(bits);
        };
        const auto input = ir.CompositeConstruct(component(0), component(7), component(19),
                                                 component(13));
        // WorkgroupId is uniform but unknown to the compiler. Dispatch exactly one group at 0.
        const auto enabled = mode == 2
                                 ? ir.INotEqual(ir.BitwiseAnd(lane, ir.Imm32(1U)), ir.Imm32(0U))
                                 : mode == 1 ? ir.IEqual(ir.WorkgroupIdX(), ir.Imm32(0U))
                                             : ir.INotEqual(ir.WorkgroupIdX(), ir.Imm32(0U));
        const auto result = ir.QuantizeDepth24(input, enabled);
        const auto offset = ir.ShiftLeftLogical(lane, ir.Imm32(3U));
        fixture.block.AppendNewInst(
            IR::Opcode::WriteStorage32,
            {ir.Imm32(0U), offset, ir.BitCast<IR::U32>(IR::F32{ir.CompositeExtract(result, 0)})});
        fixture.block.AppendNewInst(
            IR::Opcode::WriteStorage32,
            {ir.Imm32(0U), ir.IAdd(offset, ir.Imm32(4U)),
             ir.BitCast<IR::U32>(IR::F32{ir.CompositeExtract(result, 3)})});
        Shader::Optimization::DeadCodeEliminationPass(fixture.ir);
        Shader::Profile profile;
        profile.supported_spirv = 0x00010300;
        profile.unified_descriptor_binding = true;
        profile.support_float_controls = true;
        profile.support_fp32_denorm_flush = true;
        profile.support_fp32_signed_zero_nan_preserve = true;
        const auto code = Shader::Backend::SPIRV::EmitSPIRV(profile, fixture.ir);
        REQUIRE(code.size() > 5);
        size_t calls{}, stores{};
        for (size_t offset = 5; offset < code.size();) {
            const u32 count = code[offset] >> 16;
            REQUIRE(count > 0);
            REQUIRE(offset + count <= code.size());
            const auto opcode = static_cast<spv::Op>(code[offset] & 0xffffU);
            calls += opcode == spv::Op::OpFunctionCall;
            stores += opcode == spv::Op::OpStore;
            offset += count;
        }
        CHECK(calls == 1);
        CHECK(stores == 2);
        if (const char* directory = std::getenv("XBC2_TEST_SPIRV_DIR")) {
            const std::filesystem::path path{directory};
            REQUIRE_FALSE(path.empty());
            std::filesystem::create_directories(path);
            const auto filename = path / (std::string{"xbc2-d24-"} + names[mode] + ".spv");
            std::ofstream output{filename, std::ios::binary};
            REQUIRE(output.is_open());
            output.write(reinterpret_cast<const char*>(code.data()),
                         static_cast<std::streamsize>(code.size() * sizeof(u32)));
            output.close();
            REQUIRE(output.good());
        }
    }
}

TEST_CASE("Position varying reconstruction retains pixel position with large clip coordinates",
          "[xbc2][position]") {
    for (const bool offset : {false, true}) {
        Program vertex{Shader::Stage::VertexB}, fragment;
        PositionPair(vertex, fragment, offset);
        Shader::Optimization::PositionVaryingPass(vertex.ir, fragment.ir);
        REQUIRE(fragment.ir.info.uses_host_viewport);
        Evaluate eval;
        eval.attributes[IR::Attribute::PositionX] = 639.5f;
        eval.attributes[IR::Attribute::PositionY] = 359.5f;
        eval.attributes[IR::Attribute::PositionW] = 1.0e-8f;
        // Uniform offsets are transported as flat attributes by the vertex shader.
        for (const IR::Inst& store : vertex.block.Instructions()) {
            if (store.GetOpcode() == IR::Opcode::SetAttribute &&
                IR::IsGeneric(store.Arg(0).Attribute()) &&
                IR::GenericAttributeIndex(store.Arg(0).Attribute()) != 0) {
                eval.attributes[store.Arg(0).Attribute()] = eval.Scalar(store.Arg(1));
            }
        }
        for (const IR::Inst& color : fragment.block.Instructions()) {
            if (color.GetOpcode() != IR::Opcode::SetFragColor)
                continue;
            const auto c = color.Arg(1).U32();
            const float reconstructed = eval.Scalar(color.Arg(2));
            if (c == 3)
                CHECK(reconstructed == Catch::Approx(1.0e8f));
            else {
                const float original = reconstructed + (offset ? 0.125f : 0.0f);
                const float window = (original / 1.0e8f + 1.0f) * eval.viewport[c + 2] / 2;
                CHECK(window == Catch::Approx(c == 0 ? 639.5f : 359.5f).margin(0.001f));
            }
        }
        vertex.Emit();
        fragment.Emit();
    }
}

TEST_CASE("Position reconstruction uses the translated and flipped host viewport",
          "[xbc2][position]") {
    for (const bool offset : {false, true}) {
        Program vertex{Shader::Stage::VertexB}, fragment;
        PositionPair(vertex, fragment, offset);
        Shader::Optimization::PositionVaryingPass(vertex.ir, fragment.ir);
        REQUIRE(fragment.ir.info.uses_host_viewport);
        Evaluate eval;
        eval.viewport = {100.0f, 200.0f, 640.0f, -360.0f};
        eval.attributes[IR::Attribute::PositionX] = 137.5f;
        eval.attributes[IR::Attribute::PositionY] = 86.5f;
        eval.attributes[IR::Attribute::PositionW] = 0.5f;
        for (const IR::Inst& store : vertex.block.Instructions()) {
            if (store.GetOpcode() == IR::Opcode::SetAttribute &&
                IR::IsGeneric(store.Arg(0).Attribute()) &&
                IR::GenericAttributeIndex(store.Arg(0).Attribute()) != 0) {
                eval.attributes[store.Arg(0).Attribute()] = eval.Scalar(store.Arg(1));
            }
        }
        for (const IR::Inst& color : fragment.block.Instructions()) {
            if (color.GetOpcode() != IR::Opcode::SetFragColor) {
                continue;
            }
            const u32 component = color.Arg(1).U32();
            const float reconstructed = eval.Scalar(color.Arg(2));
            if (component == 3) {
                CHECK(reconstructed == 2.0f);
            } else {
                const float clip = reconstructed + (offset ? 0.125f : 0.0f);
                const float window =
                    (clip / 2.0f + 1.0f) * eval.viewport[component + 2] / 2.0f +
                    eval.viewport[component];
                CHECK(window == Catch::Approx(component == 0 ? 137.5f : 86.5f).margin(0.001f));
            }
        }
        vertex.Emit();
        fragment.Emit();
    }
}

TEST_CASE("Position candidate rejects unsupported interpolation and varying stores",
          "[xbc2][position]") {
    for (int guard = 0; guard < 7; ++guard) {
        Program vertex{Shader::Stage::VertexB}, fragment;
        PositionPair(vertex, fragment, false);
        switch (guard) {
        case 0:
            fragment.ir.info.interpolation[0] = Shader::Interpolation::Flat;
            break;
        case 1:
            fragment.ir.info.interpolation[0] = Shader::Interpolation::NoPerspective;
            break;
        case 2:
            vertex.ir.info.stores_indexed_attributes = true;
            break;
        case 3:
            fragment.ir.info.loads_indexed_attributes = true;
            break;
        case 4:
            vertex.ir.info.stores.Set(IR::Attribute::ViewportIndex);
            break;
        case 5:
            vertex.ir.info.uses_render_area = true;
            break;
        case 6: {
            IR::IREmitter ir{vertex.block};
            ir.SetAttribute(IR::Attribute::PositionX, ir.Imm32(2.0f), ir.Imm32(0));
            break;
        }
        }
        Shader::Optimization::PositionVaryingPass(vertex.ir, fragment.ir);
        CHECK_FALSE(fragment.ir.info.uses_host_viewport);
        CHECK(Count(fragment, IR::Opcode::HostViewport) == 0);
    }
}

TEST_CASE("Ordinary descriptors do not initialize D24 runtime masks", "[xbc2][depth24]") {
    Vulkan::RescalingPushConstant constants;
    REQUIRE_FALSE(constants.HasDepth24Data());
    for (u32 index = 0; index < 96; ++index) {
        constants.PushTexture(index == 42);
    }
    CHECK_FALSE(constants.HasDepth24Data());
    CHECK(constants.Data()[1] == (1U << 10));
    constants.PushTexture(false, true, true);
    CHECK(constants.HasDepth24Data());
    CHECK(constants.Depth24Data()[3] == 1U);
    CHECK(constants.Depth24PointData()[3] == 1U);
    CHECK(constants.Depth24Data()[0] == 0U);
}

TEST_CASE("D24 runtime mask follows rebinding and ignores descriptors beyond capacity",
          "[xbc2][depth24]") {
    TextureEnvironment env;
    env.format = PixelFormat::A8B8G8R8_UNORM;
    Program program;
    const auto result = Fetch(program);
    Shader::HostTranslateInfo host;
    host.ApplyDescriptorLimitPolicy();
    host.emulate_depth24_reads = true;
    Shader::Optimization::TexturePass(env, program.ir, host);
    IR::IREmitter ir{program.block};
    const auto red = ir.CompositeExtract(result, 0);
    Evaluate eval;
    eval.fetch[0] = 0.000001f;
    const float unquantized = eval.fetch[0];
    // One compiled shader, color -> D24 -> stencil -> D24 -> color.
    for (bool depth : {false, true, false, true, false}) {
        Vulkan::RescalingPushConstant masks;
        masks.PushTexture(false, depth, false);
        eval.depth24 = (masks.Depth24Data()[0] & 1U) != 0;
        const auto output = eval.Scalar(red);
        if (!depth)
            CHECK(std::bit_cast<u32>(output) == std::bit_cast<u32>(unquantized));
        else
            CHECK(output != unquantized);
    }
    Vulkan::RescalingPushConstant masks;
    // Exercise only the new masks' 128-entry bound, inside the stock six-word storage.
    // The ordinary rescaling cursor's legacy overflow behavior is unchanged.
    for (u32 index = 0; index < 140; ++index)
        masks.PushTexture(false, index == 127 || index >= 128, true);
    CHECK(masks.Depth24Data()[0] == 0);
    CHECK(masks.Depth24Data()[3] == 0x80000000U);
    CHECK(masks.Depth24PointData()[0] == 0);
    CHECK(masks.Depth24PointData()[3] == 0x80000000U);
    program.Emit();
}

TEST_CASE("Point D24 sample IR follows actual sampler and depth rebinding", "[xbc2][depth24]") {
    for (const bool implicit : {false, true}) {
        TextureEnvironment env;
        Program program;
        const auto sampled = Fetch(program, true, implicit);
        Shader::HostTranslateInfo host;
        host.ApplyDescriptorLimitPolicy();
        host.emulate_depth24_reads = true;
        Shader::Optimization::TexturePass(env, program.ir, host);
        REQUIRE(Count(program, IR::Opcode::IsTexturePointDepth24) == 1);
        REQUIRE(Count(program, IR::Opcode::IsTextureDepth24) == 0);
        IR::IREmitter ir{program.block};
        const auto red = ir.CompositeExtract(sampled, 0);
        Evaluate eval;
        eval.fetch[0] = 0.000001f;
        const auto original_bits = std::bit_cast<u32>(eval.fetch[0]);
        for (int sampler_case = 0; sampler_case < 13; ++sampler_case) {
            CAPTURE(implicit, sampler_case);
            VkSamplerCreateInfo ci{};
            ci.magFilter = ci.minFilter = VK_FILTER_NEAREST;
            ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            ci.maxAnisotropy = 1.0f;
            ci.addressModeU = ci.addressModeV = ci.addressModeW =
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            auto reduction = VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE_EXT;
            switch (sampler_case) {
            case 1:
                ci.magFilter = VK_FILTER_LINEAR;
                break;
            case 2:
                ci.minFilter = VK_FILTER_LINEAR;
                break;
            case 3:
                ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                break;
            case 4:
                ci.anisotropyEnable = VK_TRUE;
                ci.maxAnisotropy = 4.0f;
                break;
            case 5:
                ci.anisotropyEnable = VK_TRUE;
                break;
            case 6:
                ci.compareEnable = VK_TRUE;
                break;
            case 7:
                reduction = VK_SAMPLER_REDUCTION_MODE_MIN_EXT;
                break;
            case 8:
                reduction = VK_SAMPLER_REDUCTION_MODE_MAX_EXT;
                break;
            case 9:
                ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                break;
            case 10:
                ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                break;
            case 11:
                ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                break;
            default:
                break;
            }
            const bool point = Vulkan::IsPointDepthSampler(ci, reduction);
            CHECK(point == (sampler_case == 0 || sampler_case == 12));
            for (const bool depth : {false, true, false, true}) {
                Vulkan::RescalingPushConstant masks;
                masks.PushTexture(false, depth, point);
                eval.depth24 = (masks.Depth24Data()[0] & 1U) != 0;
                eval.point_depth24 = (masks.Depth24PointData()[0] & 1U) != 0;
                CHECK(eval.depth24 == depth); // Fetch does not depend on the sampler.
                const auto actual = std::bit_cast<u32>(eval.Scalar(red));
                if (depth && point)
                    CHECK(actual != original_bits);
                else
                    CHECK(actual == original_bits);
            }
        }
        program.Emit();
    }
    using namespace Shader::Backend::SPIRV;
    CHECK(DEPTH24_POINT_LAYOUT_OFFSET == 64);
    CHECK(sizeof(RescalingLayout) + sizeof(RenderAreaLayout) == 96);
    CHECK(sizeof(RescalingLayout) + sizeof(RenderAreaLayout) <= 128);
}

TEST_CASE("D24 sample candidate never quantizes comparison operations", "[xbc2][depth24]") {
    for (bool implicit : {false, true}) {
        TextureEnvironment env;
        Program program;
        IR::IREmitter ir{program.block};
        IR::TextureInstInfo flags{};
        flags.type.Assign(Shader::TextureType::Color2D);
        flags.is_depth.Assign(1);
        const auto handle = ir.GetCbuf(ir.Imm32(0), ir.Imm32(0));
        const auto coords = ir.CompositeConstruct(ir.Imm32(0.5f), ir.Imm32(0.5f));
        const auto result = implicit ? ir.ImageSampleDrefImplicitLod(handle, coords, ir.Imm32(0.5f),
                                                                     {}, {}, {}, flags)
                                     : ir.ImageSampleDrefExplicitLod(handle, coords, ir.Imm32(0.5f),
                                                                     ir.Imm32(0.0f), {}, flags);
        ir.SetFragColor(0, 0, result);
        Shader::HostTranslateInfo host;
        host.ApplyDescriptorLimitPolicy();
        host.emulate_depth24_reads = true;
        Shader::Optimization::TexturePass(env, program.ir, host);
        CHECK_FALSE(program.ir.info.uses_depth24_fetch);
        CHECK(Count(program, IR::Opcode::IsTextureDepth24) == 0);
        CHECK(Count(program, IR::Opcode::IsTexturePointDepth24) == 0);
        CHECK(Count(program, IR::Opcode::QuantizeDepth24) == 0);
    }
}

TEST_CASE("D24 masks never read past the 128-texture push constant", "[xbc2][depth24]") {
    for (const bool sampled : {false, true}) {
        for (const u32 index : {0U, 127U, 128U, 0xffffffffU}) {
            CAPTURE(sampled, index);
            Program program;
            program.ir.info.uses_depth24_fetch = true;
            program.ir.info.stores_frag_color[0] = true;
            IR::IREmitter ir{program.block};
            const auto condition = sampled ? ir.IsTexturePointDepth24(ir.Imm32(0U))
                                           : ir.IsTextureDepth24(ir.Imm32(0U));
            ir.SetFragColor(0, 0, IR::F32{ir.Select(condition, ir.Imm32(1.0f), ir.Imm32(0.0f))});
            const auto code = program.Emit(index);
            size_t mask_reads{};
            for (size_t offset = 5; offset < code.size();) {
                const u32 words = code[offset] >> 16;
                REQUIRE(words > 0);
                REQUIRE(offset + words <= code.size());
                mask_reads += (code[offset] & 0xffffU) == 199U; // OpBitwiseAnd
                offset += words;
            }
            CHECK(mask_reads == (index < 128 ? 1 : 0));
        }
    }
}

TEST_CASE("Position offsets accept only constant-buffer arithmetic with uniform addresses",
          "[xbc2][position]") {
    for (const bool composite : {false, true}) {
        Program vertex{Shader::Stage::VertexB}, fragment;
        IR::IREmitter vs{vertex.block};
        const auto address = vs.IAdd(vs.Imm32(16U), vs.GetCbuf(vs.Imm32(0U), vs.Imm32(0U)));
        IR::F32 offset = vs.GetFloatCbuf(vs.Imm32(0U), IR::U32{address});
        if (composite) {
            const auto vector = vs.CompositeConstruct(offset, vs.Imm32(0.5f));
            offset = IR::F32{vs.FPMul(IR::F32{vs.CompositeExtract(vector, 0)}, vs.Imm32(2.0f))};
        }
        PositionPair(vertex, fragment, true, offset);
        Shader::Optimization::PositionVaryingPass(vertex.ir, fragment.ir);
        CHECK(fragment.ir.info.uses_host_viewport);
        CHECK(Count(fragment, IR::Opcode::HostViewport) > 0);
    }
}

TEST_CASE("Position reconstruction accepts a synthetic uniform visibility clamp",
          "[xbc2][position]") {
    for (const bool uniform : {true, false}) {
        CAPTURE(uniform);
        Program vertex{Shader::Stage::VertexB}, fragment;
        IR::IREmitter vs{vertex.block};
        // Synthetic visibility offset: position.xy = Fma(1 - clamp(cbuf, 0, 1),
        // 1000000, varying.xy), with the same w. A per-vertex clamp must stay rejected.
        const IR::U32 source = uniform ? vs.GetCbuf(vs.Imm32(6U), vs.Imm32(0U))
                                      : vs.GetAttributeU32(IR::Attribute::VertexId);
        const IR::U32 clamped = vs.SMax(vs.Imm32(0U), vs.SMin(vs.Imm32(1U), source));
        const IR::U32 enabled{vs.IAdd(vs.INeg(clamped), vs.Imm32(1U))};
        const IR::F32 offset{vs.ConvertSToF(32, 32, enabled)};
        PositionPair(vertex, fragment, true, offset, true);
        Shader::Optimization::PositionVaryingPass(vertex.ir, fragment.ir);
        CHECK(fragment.ir.info.uses_host_viewport == uniform);
        CHECK((Count(fragment, IR::Opcode::HostViewport) > 0) == uniform);
        if (uniform) {
            // The transported offset is the uniform product, not the vertex-dependent FMA.
            size_t offset_stores{};
            for (const IR::Inst& inst : vertex.block.Instructions()) {
                if (inst.GetOpcode() != IR::Opcode::SetAttribute ||
                    !IR::IsGeneric(inst.Arg(0).Attribute()) ||
                    IR::GenericAttributeIndex(inst.Arg(0).Attribute()) == 0 ||
                    inst.Arg(1).IsImmediate()) {
                    continue;
                }
                const IR::Inst* product = inst.Arg(1).InstRecursive();
                REQUIRE(product->GetOpcode() == IR::Opcode::FPMul32);
                CHECK(product->Arg(0).InstRecursive() == offset.InstRecursive());
                CHECK(product->Arg(1).F32() == 1000000.0f);
                ++offset_stores;
            }
            CHECK(offset_stores == 2);
            vertex.ir.info.constant_buffer_descriptors.push_back({.index = 6, .count = 1});
            vertex.ir.info.constant_buffer_mask = 1U << 6;
            vertex.ir.info.constant_buffer_used_sizes[6] = sizeof(u32);
            vertex.ir.info.used_constant_buffer_types = IR::Type::U32;
            vertex.Emit();
            fragment.Emit();
        }
    }
}

TEST_CASE("Position offsets reject unapproved sources and varying constant-buffer addresses",
          "[xbc2][position]") {
    for (int source = 0; source < 8; ++source) {
        CAPTURE(source);
        Program vertex{Shader::Stage::VertexB}, fragment;
        IR::IREmitter vs{vertex.block};
        const IR::F32 offset = [&]() -> IR::F32 {
            switch (source) {
            case 0:
                return vs.BitCast<IR::F32>(vs.InvocationId());
            case 1:
                return vs.BitCast<IR::F32>(vs.IsbeRead(vs.Imm32(0U)));
            case 2:
                return vs.GetPatch(IR::Patch::Component0);
            case 3:
                return vs.BitCast<IR::F32>(vs.SampleId());
            case 4:
                return vs.BitCast<IR::F32>(vs.GetReg(IR::Reg::R0));
            case 5:
                return IR::F32{vs.Select(vs.GetPred(IR::Pred::P0), vs.Imm32(0.0f), vs.Imm32(1.0f))};
            case 6:
                return IR::F32{vs.ConvertUToF(32, 32, vs.GetAttributeU32(IR::Attribute::VertexId))};
            default:
                return vs.GetFloatCbuf(vs.Imm32(0U), vs.GetAttributeU32(IR::Attribute::VertexId));
            }
        }();
        // Wrap the source in an approved operation so rejection must be recursive.
        const IR::F32 wrapped{vs.FPAdd(offset, vs.Imm32(0.125f))};
        PositionPair(vertex, fragment, true, wrapped);
        Shader::Optimization::PositionVaryingPass(vertex.ir, fragment.ir);
        CHECK_FALSE(fragment.ir.info.uses_host_viewport);
        CHECK(Count(fragment, IR::Opcode::HostViewport) == 0);
    }
}

TEST_CASE("Xenoblade cloud candidate is restricted to the requested title ID", "[xbc2][policy]") {
    using VideoCore::IsXenoblade2CloudTitle;
    STATIC_REQUIRE(IsXenoblade2CloudTitle(0x0100E95004038000ULL));
    STATIC_REQUIRE_FALSE(IsXenoblade2CloudTitle(0));
    STATIC_REQUIRE_FALSE(IsXenoblade2CloudTitle(0x0400486024DEC000ULL)); // OOT
    STATIC_REQUIRE_FALSE(IsXenoblade2CloudTitle(0x01007EF00011E000ULL)); // BOTW
    STATIC_REQUIRE_FALSE(
        IsXenoblade2CloudTitle(0x0100F3400332C000ULL)); // Untested title ID
    STATIC_REQUIRE_FALSE(IsXenoblade2CloudTitle(0x0100E95004038800ULL)); // Untested ID
}
