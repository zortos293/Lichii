// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/draw_template.h"

namespace {
using VideoCommon::DrawTemplateCheck;
using VideoCommon::DrawTemplateContext;
using Template = VideoCommon::DrawTemplate<u64, 5, 2>;
using Table = VideoCommon::DrawTemplateTable<Template>;

DrawTemplateContext Context(u64 owner) {
    return {
        .owner = owner,
        .memory = 0x1000,
        .image_table = 0x2000,
        .tic_address = 0x40000,
        .tsc_address = 0x50000,
        .tic_limit = 127,
        .tsc_limit = 63,
        .via_header_index = 0,
        .native_tiling = 1,
    };
}

Template* Find(Table& table, const DrawTemplateContext& context, const std::vector<u32>& handles) {
    return table.Find(context, handles, VideoCommon::HashDrawTemplate(context, handles));
}

Template& Insert(Table& table, const DrawTemplateContext& context,
                 const std::vector<u32>& handles) {
    return table.Insert(context, handles, VideoCommon::HashDrawTemplate(context, handles));
}

/// One draw as GraphicsPipeline::ConfigureImpl runs it: look up, check, note, then keep when the
/// template was not used. With verify, a found template is resolved in full and kept again.
/// Returns whether a valid template was found.
bool Draw(Table& table, VideoCommon::DrawTemplateRecorder& recorder,
          const DrawTemplateContext& context, std::span<const u32> handles, bool verify) {
    constexpr u64 host_serial = 3;
    constexpr u64 scale_epoch = 1;
    const u64 hash = VideoCommon::HashDrawTemplate(context, handles);
    Template* found = table.Find(context, handles, hash);
    if (found && found->Check(host_serial, scale_epoch, [](VideoCommon::ImageViewId) {
            return 0U;
        }) != DrawTemplateCheck::Hit) {
        Table::Erase(*found);
        found = nullptr;
    }
    const bool keep = recorder.Note(found != nullptr);
    if (keep && (!found || verify)) {
        Template& kept = table.Insert(context, handles, hash);
        kept.host_serial = host_serial;
        kept.scale_epoch = scale_epoch;
    }
    return found != nullptr;
}
} // namespace

TEST_CASE("Draw template: only the same pipeline, tables and handle words find a template",
          "[video_core]") {
    Table table{4};
    const DrawTemplateContext context = Context(table.NewOwner());
    const std::vector<u32> handles{0x10002, 0x30004};
    Insert(table, context, handles);
    REQUIRE(Find(table, context, handles) != nullptr);

    CHECK(Find(table, context, {0x10002, 0x30005}) == nullptr);
    CHECK(Find(table, context, {0x10002}) == nullptr);
    CHECK(Find(table, Context(table.NewOwner()), handles) == nullptr);
    for (auto change : {&DrawTemplateContext::tic_address, &DrawTemplateContext::tsc_address,
                        &DrawTemplateContext::memory, &DrawTemplateContext::image_table}) {
        DrawTemplateContext other = context;
        other.*change += 0x100;
        CHECK(Find(table, other, handles) == nullptr);
    }
    for (auto change :
         {&DrawTemplateContext::tic_limit, &DrawTemplateContext::tsc_limit,
          &DrawTemplateContext::via_header_index, &DrawTemplateContext::native_tiling}) {
        DrawTemplateContext other = context;
        other.*change ^= 1;
        CHECK(Find(table, other, handles) == nullptr);
    }
    // A hash collision alone never finds another key's template.
    const std::vector<u32> other_handles{0x10002, 0x30006};
    CHECK(table.Find(context, other_handles, VideoCommon::HashDrawTemplate(context, handles)) ==
          nullptr);
}

TEST_CASE("Draw template: a full set replaces its least recently used template", "[video_core]") {
    Table table{1};
    const DrawTemplateContext context = Context(table.NewOwner());
    for (u32 key = 0; key < Table::WAYS; ++key) {
        Insert(table, context, {key});
    }
    REQUIRE(Find(table, context, {0}) != nullptr);
    Insert(table, context, {Table::WAYS});
    CHECK(Find(table, context, {0}) != nullptr);
    CHECK(Find(table, context, {1}) == nullptr);
    CHECK(Find(table, context, {Table::WAYS}) != nullptr);

    // The same key keeps its slot.
    Template& kept = Insert(table, context, {2});
    CHECK(&Insert(table, context, {2}) == &kept);

    size_t found = 0;
    for (u32 key = 0; key < 64; ++key) {
        Insert(table, context, {100 + key});
    }
    for (u32 key = 0; key < 64; ++key) {
        found += Find(table, context, {100 + key}) != nullptr ? 1 : 0;
    }
    CHECK(found == table.Capacity());
}

TEST_CASE("Draw template: a change the key does not cover rejects the template", "[video_core]") {
    std::array<u8, sizeof(Tegra::Texture::TICEntry)> tic_memory{};
    std::array<u8, sizeof(Tegra::Texture::TSCEntry)> tsc_memory{};
    tic_memory[4] = 0x5a;
    tsc_memory[8] = 0xa5;
    std::array<u32, 4> generations{0, 3, 0, 0};
    const auto generation = [&](VideoCommon::ImageViewId id) { return generations[id.index]; };

    Template kept;
    kept.host_serial = 7;
    kept.scale_epoch = 2;
    VideoCommon::DrawTemplateTexture& texture = kept.textures.emplace_back();
    texture.tic_host = tic_memory.data();
    texture.tsc_host = tsc_memory.data();
    std::memcpy(&texture.tic, tic_memory.data(), sizeof(texture.tic));
    std::memcpy(&texture.tsc, tsc_memory.data(), sizeof(texture.tsc));
    texture.view = VideoCommon::ImageViewId{1};
    texture.view_generation = 3;
    REQUIRE(kept.Check(7, 2, generation) == DrawTemplateCheck::Hit);

    CHECK(kept.Check(8, 2, generation) == DrawTemplateCheck::HostMemory);
    CHECK(kept.Check(7, 3, generation) == DrawTemplateCheck::Scale);

    // The view was deleted and its slot holds another one.
    ++generations[1];
    CHECK(kept.Check(7, 2, generation) == DrawTemplateCheck::ViewDeleted);
    --generations[1];

    ++tic_memory[4];
    CHECK(kept.Check(7, 2, generation) == DrawTemplateCheck::Descriptors);
    --tic_memory[4];
    ++tsc_memory[31];
    CHECK(kept.Check(7, 2, generation) == DrawTemplateCheck::Descriptors);
    --tsc_memory[31];
    CHECK(kept.Check(7, 2, generation) == DrawTemplateCheck::Hit);
}

TEST_CASE("Draw template: a rejected template is gone for the next lookup", "[video_core]") {
    Table table{4};
    const DrawTemplateContext context = Context(table.NewOwner());
    const std::vector<u32> handles{0x10002};
    Template* const kept = &Insert(table, context, handles);
    REQUIRE(Find(table, context, handles) == kept);
    Table::Erase(*kept);
    CHECK(Find(table, context, handles) == nullptr);
    CHECK(&Insert(table, context, handles) == kept);
    CHECK(Find(table, context, handles) == kept);
}

TEST_CASE("Draw template: every draw of a repeating frame finds its template from the third frame",
          "[video_core]") {
    constexpr u32 draws_per_frame = 200;
    for (const bool verify : {false, true}) {
        Table table{2048};
        VideoCommon::DrawTemplateRecorder recorder;
        const u64 owner = table.NewOwner();
        std::vector<u32> found_per_frame;
        for (u32 frame = 0; frame < 3; ++frame) {
            u32 found = 0;
            for (u32 draw = 0; draw < draws_per_frame; ++draw) {
                // The context and handle words are read again for every draw, as each frame does.
                const DrawTemplateContext context = Context(owner);
                const std::array<u32, 2> handles{0x100000 | draw, 0x200000 | (draw * 7)};
                found += Draw(table, recorder, context, handles, verify) ? 1 : 0;
            }
            found_per_frame.push_back(found);
        }
        INFO("verify " << verify);
        CHECK(found_per_frame[0] == 0);
        // The first 16 misses in a row and one in 16 after them are kept.
        CHECK(found_per_frame[1] == 16 + (draws_per_frame - 16) / 16);
        CHECK(found_per_frame[2] == draws_per_frame);
    }
}
