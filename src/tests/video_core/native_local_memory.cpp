// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache_records.h"
#include "video_core/renderer_vulkan/vk_shader_cache_v2.h"
#include "video_core/shader_cache_v2/cache_format.h"
#include "video_core/shader_environment.h"

namespace {
namespace IR = Shader::IR;
constexpr u64 MaxwellExit = 0xe30000000007000fULL;

class LocalEnvironment final : public VideoCommon::GenericEnvironment {
public:
    bool native{true};

    LocalEnvironment(u32 low, u32 high) {
        stage = Shader::Stage::Compute;
        local_memory_size = low + high;
        local_memory_high_size = high;
        code = {0, 0};
        cached_lowest = 0;
        cached_highest = 8;
        workgroup_size = {1, 1, 1};
    }

    void SetProgram(u64 instruction) {
        start_address = sizeof(sph);
        code.assign(start_address / sizeof(u64), 0);
        code.push_back(instruction);
        code.push_back(MaxwellExit);
        Shader::Ampere::AppendNativeAddressMap(code, std::array{0U, 1U});
        cached_highest = static_cast<u32>((code.size() - 1) * sizeof(u64));
    }

    void AddPointer(u32 offset) {
        storage_pointer_types.emplace((u64{3} << 32) | offset, u8{1});
    }

    bool IsNativeProgram() const override { return native; }
    u32 ReadCbufValue(u32, u32) override { throw Shader::LogicError("Unexpected cbuf read"); }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
};

struct LocalMachine {
    LocalEnvironment env;
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    std::array<u32, IR::NUM_REGS> registers{};
    std::vector<u32> memory;
    std::unordered_map<const IR::Inst*, u32> values;

    LocalMachine(u32 low, u32 high) : env{low, high}, memory((low + high) / 4) {}

    u32 Value(const IR::Value& value) {
        return value.IsImmediate() ? value.U32() : values.at(value.InstRecursive());
    }

    void Run() {
        for (const auto& inst : block) {
            const auto arg = [&](size_t i) { return Value(inst.Arg(i)); };
            u32 result{};
            switch (inst.GetOpcode()) {
            case IR::Opcode::GetRegister:
                result = registers[static_cast<size_t>(inst.Arg(0).Reg())];
                break;
            case IR::Opcode::SetRegister:
                registers[static_cast<size_t>(inst.Arg(0).Reg())] = arg(1);
                break;
            case IR::Opcode::IAdd32: result = arg(0) + arg(1); break;
            case IR::Opcode::BitwiseAnd32: result = arg(0) & arg(1); break;
            case IR::Opcode::ShiftRightArithmetic32:
                result = static_cast<s32>(arg(0)) >> arg(1);
                break;
            case IR::Opcode::ULessThan: result = arg(0) < arg(1); break;
            case IR::Opcode::SelectU32: result = arg(0) ? arg(1) : arg(2); break;
            case IR::Opcode::LoadLocal:
                REQUIRE(arg(0) < memory.size());
                result = memory[arg(0)];
                break;
            case IR::Opcode::WriteLocal:
                REQUIRE(arg(0) < memory.size());
                memory[arg(0)] = arg(1);
                break;
            default: FAIL("Unexpected local memory IR opcode");
            }
            values.emplace(&inst, result);
        }
    }

    void StoreLoad(u32 address, u32 expected_word) {
        registers[1] = address;
        registers[2] = 0x1234abcd;
        visitor.STL(0xef54000000070102ULL);
        visitor.LDL(0xef44000000070103ULL);
        Run();
        CHECK(memory.at(expected_word) == 0x1234abcd);
        CHECK(registers[3] == 0x1234abcd);
    }
};
} // namespace

TEST_CASE("Native local memory maps the Fast Fusion stack inside its high allocation",
          "[video_core][native_local_memory]") {
    for (const u32 displacement : {0U, 76U}) {
        LocalMachine machine{0, 656};
        machine.StoreLoad(0xfffdc0 - 80 + displacement, displacement / 4);
    }
}

TEST_CASE("Native low and high local allocations occupy disjoint host words",
          "[video_core][native_local_memory]") {
    for (const auto& [address, word] : std::array<std::pair<u32, u32>, 4>{
             {{0xffffe0, 0}, {0xfffffff0, 4}, {0, 8}, {12, 11}}}) {
        LocalMachine machine{16, 32};
        machine.StoreLoad(address, word);
    }
    LocalMachine low_only{32, 0};
    low_only.StoreLoad(12, 3);
    LocalMachine maxwell{32, 0};
    maxwell.env.native = false;
    maxwell.StoreLoad(12, 3);
}

TEST_CASE("Native local memory without a high allocation translates as before",
          "[video_core][native_local_memory]") {
    const auto opcodes = [](bool native) {
        LocalMachine machine{32, 0};
        machine.env.native = native;
        machine.StoreLoad(12, 3);
        std::vector<IR::Opcode> result;
        for (const auto& inst : machine.block) {
            result.push_back(inst.GetOpcode());
        }
        return result;
    };
    CHECK(opcodes(true) == opcodes(false));
}

namespace {
std::string Record(u32 low, u32 high) {
    LocalEnvironment env{low, high};
    std::ostringstream out{std::ios::binary};
    env.Serialize(out, 783);
    return out.str();
}

/// The record as builds before the high allocation key wrote it.
std::string WithoutHighKey(std::string record) {
    const u64 key = 0xffffffff00000003ULL;
    const std::string key_bytes{reinterpret_cast<const char*>(&key), sizeof(key)};
    const auto position = record.find(key_bytes);
    REQUIRE(position != std::string::npos);
    record.erase(position, 12);
    u64 count;
    std::memcpy(&count, record.data() + 24, sizeof(count));
    --count;
    std::memcpy(record.data() + 24, &count, sizeof(count));
    return record;
}

VideoCommon::FileEnvironment Read(const std::string& bytes) {
    std::istringstream input{bytes, std::ios::binary};
    VideoCommon::FileEnvironment result;
    result.Deserialize(input, static_cast<std::streamoff>(bytes.size()));
    return result;
}

constexpr u64 ImmediateLocalStore(u32 address) {
    return 0xef5400000007ff02ULL | (u64{address} << 20);
}

struct LocalTranslation {
    Vulkan::ComputePipelineLookupKey key;
    std::vector<u32> stored_words;
};

LocalTranslation TranslateLocalProgram(Shader::Environment& env,
                                       const Vulkan::ComputePipelineLookupKey& lookup) {
    VideoCommon::ShaderPools pools;
    Shader::Maxwell::Flow::CFG cfg{env, pools.flow_block, env.StartAddress()};
    const auto program = Shader::Maxwell::TranslateProgram(pools.inst, pools.block, env, cfg, {});
    LocalTranslation result{.key = Vulkan::TranslatedComputeKey(lookup, cfg.HasLocalMemoryAccesses()),
                            .stored_words = {}};
    for (const auto* block : program.blocks) {
        for (const auto& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::WriteLocal) {
                REQUIRE(inst.Arg(0).IsImmediate());
                result.stored_words.push_back(inst.Arg(0).U32());
            }
        }
    }
    return result;
}
} // namespace

TEST_CASE("Recorded local layouts round trip and old incomplete layouts request recapture",
          "[video_core][native_local_memory]") {
    auto recorded = Read(Record(16, 656));
    REQUIRE(recorded.LocalMemorySize() == 672);
    REQUIRE(recorded.LocalMemoryHighSize() == 656);
    REQUIRE_FALSE(recorded.LacksRecordedValue(783));

    auto incomplete = Read(WithoutHighKey(Record(16, 656)));
    REQUIRE_THROWS_AS(incomplete.LocalMemoryHighSize(), Shader::LogicError);
    REQUIRE(incomplete.LacksRecordedValue(783));
    REQUIRE(incomplete.LacksRecordedValue(0));
}

TEST_CASE("Pruned local stores still select a layout-specific compute cache key",
          "[video_core][native_local_memory]") {
    const Vulkan::ComputePipelineCacheKey key{.unique_hash = 1, .shared_memory_size = 0,
                                              .workgroup_size{1, 1, 1}};
    const Vulkan::ComputePipelineLookupKey small_lookup{key, 16, 0};
    const Vulkan::ComputePipelineLookupKey large_lookup{key, 32, 0};
    LocalEnvironment small_env{16, 0};
    LocalEnvironment large_env{32, 0};
    small_env.SetProgram(ImmediateLocalStore(20));
    large_env.SetProgram(ImmediateLocalStore(20));

    const auto small_translation = TranslateLocalProgram(small_env, small_lookup);
    CHECK(small_translation.stored_words.empty());
    CHECK(small_translation.key == small_lookup);

    std::ostringstream out{std::ios::binary};
    small_env.Serialize(out, 0);
    auto recorded = Read(out.str());
    REQUIRE(recorded.IsNativeProgram());
    const auto cached_translation =
        TranslateLocalProgram(recorded, Vulkan::RecordLookupKey(key, recorded));
    CHECK(cached_translation.key == small_lookup);
    CHECK(cached_translation.key.DriverBinaryKey() == small_translation.key.DriverBinaryKey());
    std::unordered_map<Vulkan::ComputePipelineLookupKey, int> cache{{cached_translation.key, 1}};
    CHECK(Vulkan::FindComputePipeline(cache, large_lookup) == cache.end());

    const auto large_translation = TranslateLocalProgram(large_env, large_lookup);
    CHECK(large_translation.stored_words == std::vector<u32>{5});
    CHECK(large_translation.key == large_lookup);
    CHECK(large_translation.key.DriverBinaryKey() != small_translation.key.DriverBinaryKey());
}

TEST_CASE("Native immediate local stores check bounds after rotating the address",
          "[video_core][native_local_memory]") {
    const Vulkan::ComputePipelineLookupKey lookup{{1, 0, {1, 1, 1}}, 48, 32};
    for (const auto& [address, words] : std::array<std::pair<u32, std::vector<u32>>, 3>{
             {{4096, {}}, {0xffffe0, {0}}, {12, {11}}}}) {
        CAPTURE(address);
        LocalEnvironment env{16, 32};
        env.SetProgram(ImmediateLocalStore(address));
        CHECK(TranslateLocalProgram(env, lookup).stored_words == words);
    }
}

TEST_CASE("Vulkan boot restores the recorded layouts that a compute shader needs",
          "[video_core][native_local_memory]") {
    namespace V2 = VideoCommon::ShaderCacheV2;
    constexpr u32 read_properties = 783;
    constexpr u64 pointer_bank = u64{3} << 32;
    const bool uses_local_memory = GENERATE(true, false);
    CAPTURE(uses_local_memory);
    const Vulkan::ComputePipelineCacheKey key{.unique_hash = 1, .shared_memory_size = 0,
                                              .workgroup_size{1, 1, 1}};
    const Vulkan::ComputePipelineLookupKey low_only{key, 48, 0};
    const Vulkan::ComputePipelineLookupKey split{key, 48, 32};
    const auto header = V2::EncodeFileHeader(1);
    std::vector<u8> bytes(header.begin(), header.end());
    const auto append = [&](u32 high, std::initializer_list<u32> pointers) {
        LocalEnvironment env{48 - high, high};
        env.SetProgram(uses_local_memory ? ImmediateLocalStore(12) : MaxwellExit);
        for (const u32 offset : pointers) {
            env.AddPointer(offset);
        }
        const std::array<const VideoCommon::GenericEnvironment*, 1> envs{&env};
        const auto payload = VideoCommon::SerializePipelinePayload(
            {reinterpret_cast<const char*>(&key), sizeof(key)}, envs, read_properties);
        REQUIRE_FALSE(payload.empty());
        V2::EncodeRecord(bytes, V2::RecordKind::Compute, Vulkan::ComputeRecordKey(payload), payload);
    };
    append(0, {0x10});
    append(32, {0x20});
    append(32, {0x10, 0x18, 0x38});
    append(0, {0x10, 0x18});
    append(32, {0x20, 0x28});

    const auto log = V2::ParseLog(bytes);
    REQUIRE(log.live.size() == 5);
    std::vector<Vulkan::CachedComputeTranslation> candidates;
    for (const auto& record : log.live) {
        const size_t index = candidates.size();
        candidates.push_back(Vulkan::ReadCachedComputeTranslation(
            record.payload, {.begin = index, .end = index + 1, .hash = record.key}, key));
    }
    struct Restored {
        std::vector<u32> words;
        std::vector<u64> pointers;
        u64 binary_key;
    };
    std::unordered_map<Vulkan::ComputePipelineLookupKey, Restored> restored;
    const auto load = Vulkan::RestoreCachedComputeLayouts(
        key, std::move(candidates), read_properties,
        [&](const Vulkan::ComputePipelineLookupKey& lookup, VideoCommon::FileEnvironment& env,
            Vulkan::CachedComputeScope scope) {
            auto translation = TranslateLocalProgram(env, lookup);
            const bool layout_dependent = translation.key == lookup;
            if (scope == Vulkan::CachedComputeScope::AnyLayoutOnly && layout_dependent) {
                return Vulkan::CachedComputeResult::ExactLayout;
            }
            const auto stored_key =
                scope == Vulkan::CachedComputeScope::ExactLayoutOnly ? lookup : translation.key;
            REQUIRE(restored.emplace(stored_key, Restored{std::move(translation.stored_words),
                                                          env.MappedStoragePointerKeys(),
                                                          translation.key.DriverBinaryKey()})
                        .second);
            return layout_dependent ? Vulkan::CachedComputeResult::ExactLayout
                                    : Vulkan::CachedComputeResult::AnyLayout;
        });
    CHECK(load.incomplete.empty());
    CHECK(load.copies.empty());
    const auto low_pipeline = Vulkan::FindComputePipeline(restored, low_only);
    const auto split_pipeline = Vulkan::FindComputePipeline(restored, split);
    REQUIRE(low_pipeline != restored.end());
    REQUIRE(split_pipeline != restored.end());
    if (!uses_local_memory) {
        CHECK(load.layouts == 1);
        REQUIRE(restored.size() == 1);
        CHECK(low_pipeline == split_pipeline);
        CHECK(low_pipeline->first == Vulkan::AnyLayoutKey(key));
        CHECK(low_pipeline->second.binary_key == key.Hash());
        CHECK(low_pipeline->second.words.empty());
        CHECK(low_pipeline->second.pointers ==
              std::vector<u64>{pointer_bank | 0x10, pointer_bank | 0x18, pointer_bank | 0x38});
        return;
    }
    REQUIRE(load.layouts == 2);
    CHECK(low_pipeline->second.binary_key == low_only.DriverBinaryKey());
    CHECK(split_pipeline->second.binary_key == split.DriverBinaryKey());
    CHECK(low_pipeline->second.binary_key != split_pipeline->second.binary_key);
    CHECK(low_pipeline->second.binary_key != key.Hash());
    CHECK(split_pipeline->second.binary_key != key.Hash());
    CHECK(low_pipeline->second.words == std::vector<u32>{3});
    CHECK(split_pipeline->second.words == std::vector<u32>{11});
    CHECK(low_pipeline->second.pointers == std::vector<u64>{pointer_bank | 0x10, pointer_bank | 0x18});
    CHECK(split_pipeline->second.pointers == std::vector<u64>{pointer_bank | 0x20, pointer_bank | 0x28});
}

TEST_CASE("One compute shader under two local layouts keeps two pipelines",
          "[video_core][native_local_memory]") {
    using Vulkan::ComputePipelineLookupKey;
    const Vulkan::ComputePipelineCacheKey key{
        .unique_hash = 0xd037c025142cc326ULL, .shared_memory_size = 0, .workgroup_size{64, 1, 1}};
    const ComputePipelineLookupKey high_only{key, 592, 592};
    const ComputePipelineLookupKey low_only{key, 592, 0};
    std::unordered_map<ComputePipelineLookupKey, int> cache;
    cache.emplace(high_only, 1);
    cache.emplace(low_only, 2);
    CHECK(cache.size() == 2);
    // Any number of alternations finds each layout's own translation.
    for (int i = 0; i < 10; ++i) {
        CHECK(Vulkan::FindComputePipeline(cache, high_only)->second == 1);
        CHECK(Vulkan::FindComputePipeline(cache, low_only)->second == 2);
    }
    CHECK(Vulkan::FindComputePipeline(cache, ComputePipelineLookupKey{key, 672, 656}) ==
          cache.end());

    // A record's pipeline is found under the layout it was translated for.
    CHECK(Vulkan::RecordLookupKey(key, Read(Record(16, 656))) ==
          ComputePipelineLookupKey{key, 672, 656});
    CHECK(Vulkan::RecordLookupKey(key, Read(Record(32, 0))) ==
          ComputePipelineLookupKey{key, 32, 0});
    // Only Maxwell compute translates local accesses from a record without the high allocation.
    CHECK(Vulkan::RecordLookupKey(key, Read(WithoutHighKey(Record(32, 0)))) ==
          ComputePipelineLookupKey{key, 32, 0});
}

TEST_CASE("Compute pipelines without local accesses serve every local layout",
          "[video_core][native_local_memory]") {
    using Vulkan::ComputePipelineLookupKey;
    const Vulkan::ComputePipelineCacheKey key{
        .unique_hash = 0x143a3470b3d02c14ULL, .shared_memory_size = 0, .workgroup_size{32, 1, 1}};
    LocalEnvironment env{0, 576};
    env.SetProgram(MaxwellExit);
    const auto translation = TranslateLocalProgram(env, {key, 576, 576});
    std::unordered_map<ComputePipelineLookupKey, int> cache;
    cache.emplace(translation.key, 1);
    // Xenoblade Chronicles 2 recorded it at 576 bytes and dispatches it at 592.
    CHECK(Vulkan::FindComputePipeline(cache, ComputePipelineLookupKey{key, 576, 576})->second == 1);
    CHECK(Vulkan::FindComputePipeline(cache, ComputePipelineLookupKey{key, 592, 592})->second == 1);
    // Another shader's key does not match.
    auto other{key};
    other.unique_hash ^= 1;
    CHECK(Vulkan::FindComputePipeline(cache, ComputePipelineLookupKey{other, 576, 576}) ==
          cache.end());
}

namespace {
/// The fields of a compute pipeline cache entry that publishing a rebuild reads and writes.
struct FakeEntry {
    std::unique_ptr<int> pipeline;
    bool depends_on_local_memory_layout{};
    Vulkan::ComputePipelineLookupKey dispatch_key{};
    u32 rebuilds{};
    bool used{};
};

struct FakeCache {
    std::unordered_map<Vulkan::ComputePipelineLookupKey, FakeEntry> map;
    std::vector<int> retired;

    /// What the dispatch path finds for a layout, the translation's id or 0
    int Dispatch(const Vulkan::ComputePipelineLookupKey& lookup) {
        const auto it{Vulkan::FindComputePipeline(map, lookup)};
        return it != map.end() && it->second.pipeline ? *it->second.pipeline : 0;
    }

    /// Publishes `rebuilt` over the entry dispatch `lookup` finds, as CurrentComputePipeline does,
    /// and returns whether that entry still serves the dispatch.
    bool Publish(const Vulkan::ComputePipelineLookupKey& lookup, FakeEntry&& rebuilt) {
        auto it{Vulkan::FindComputePipeline(map, lookup)};
        REQUIRE(it != map.end());
        it = Vulkan::PublishComputeRebuild(map, it, std::move(rebuilt),
                                           [&](std::unique_ptr<int>&& pipeline) {
                                               retired.push_back(*pipeline);
                                           });
        return Vulkan::ServesLookup(it->first, lookup);
    }
};

FakeEntry Translation(int id, bool depends_on_local_memory_layout,
                      const Vulkan::ComputePipelineLookupKey& dispatch_key) {
    return {.pipeline = std::make_unique<int>(id),
            .depends_on_local_memory_layout = depends_on_local_memory_layout,
            .dispatch_key = dispatch_key};
}
} // namespace

TEST_CASE("A rebuild with local accesses leaves the layout-free key for its own layout",
          "[video_core][native_local_memory]") {
    using Vulkan::ComputePipelineLookupKey;
    const Vulkan::ComputePipelineCacheKey key{
        .unique_hash = 0xd037c025142cc326ULL, .shared_memory_size = 0, .workgroup_size{64, 1, 1}};
    const ComputePipelineLookupKey a{key, 592, 592};
    const ComputePipelineLookupKey b{key, 608, 608};
    FakeCache cache;
    // The first translation did not depend on the layout.
    cache.map.emplace(Vulkan::AnyLayoutKey(key), Translation(1, false, a));
    REQUIRE(cache.Dispatch(a) == 1);
    REQUIRE(cache.Dispatch(b) == 1);

    SECTION("Immediate rebuild by the dispatch it was translated for") {
        CHECK(cache.Publish(a, Translation(2, true, a)));
        CHECK(cache.Dispatch(a) == 2);
        CHECK(cache.Dispatch(b) == 0); // B translates its own
        CHECK(cache.retired == std::vector<int>{1});
        CHECK(cache.map.at(a).rebuilds == 1);
    }
    SECTION("A queues the rebuild, B finds it built") {
        CHECK_FALSE(cache.Publish(b, Translation(2, true, a)));
        // B's dispatch looks up again: neither lookup path hands it A's translation.
        CHECK(cache.Dispatch(b) == 0);
        CHECK(cache.Dispatch(a) == 2);
        cache.map.emplace(b, Translation(3, true, b));
        CHECK(cache.Dispatch(b) == 3);
        CHECK(cache.Dispatch(a) == 2);
        CHECK(cache.retired == std::vector<int>{1});
    }
    SECTION("A rebuild without local accesses keeps serving every layout") {
        CHECK(cache.Publish(b, Translation(2, false, a)));
        CHECK(cache.Dispatch(a) == 2);
        CHECK(cache.Dispatch(b) == 2);
    }
    SECTION("A layout's own translation is replaced, not kept beside the rebuild") {
        cache.map.emplace(a, Translation(4, true, a));
        auto it{cache.map.find(Vulkan::AnyLayoutKey(key))};
        it = Vulkan::PublishComputeRebuild(cache.map, it, Translation(2, true, a),
                                           [&](std::unique_ptr<int>&& pipeline) {
                                               cache.retired.push_back(*pipeline);
                                           });
        CHECK(it->first == a);
        CHECK(cache.Dispatch(a) == 2);
        CHECK(cache.Dispatch(b) == 0);
        CHECK(cache.retired == std::vector<int>{1, 4});
    }
}
