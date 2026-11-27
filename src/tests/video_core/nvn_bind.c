// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The replay's binding walk (vka_bind.c vka_walk) for storage buffers whose address is read from
// a table in memory (VKA_KIND_SBUF_IND, VKA_KIND_SBUF_UBO_IND: Ocarina of Time's materials;
// VKA_KIND_UBO_PTR_IND: Duskbloods' compute), images whose handle is in a uniform buffer
// (VKA_KIND_IMG_UBO), images read from the image pool (VKA_KIND_IMG_POOL) and texel buffers
// whose handle is in a uniform buffer (VKA_KIND_TBUF_UBO, VKA_KIND_IMG_UBO: Orbitals).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vka_bind.h"
#include "nvn_direct.h"
#include "nvn_direct_front.h"

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%d: %s\n", __LINE__, #expr); exit(1); } } while (0)

enum { GUEST_BASE = 0x10000, ALIGN = 64 };
static _Alignas(64) uint8_t arena[4096];

uint64_t nvn_direct_guest_of(uint64_t host) { return GUEST_BASE + host - (uintptr_t)arena; }
uint64_t nvn_direct_host_of(uint64_t guest) { return (uintptr_t)arena + guest - GUEST_BASE; }

// Guest memory as the replay's cpu_read sees it: arena pointers or the game's addresses
static int rd(void* ctx, uint64_t addr, void* out, size_t n) {
    (void)ctx;
    if (addr >= GUEST_BASE && addr - GUEST_BASE + n <= sizeof arena) addr = nvn_direct_host_of(addr);
    if (addr < (uintptr_t)arena || addr - (uintptr_t)arena + n > sizeof arena) return 0;
    memcpy(out, (const void*)(uintptr_t)addr, n);
    return 1;
}

static int walk(const vka_bstate* s, int st, dmsh_vk_bind b, vka_res* r) {
    dmsh_vk_entry e = {.bind_count = 1};
    char why[200] = "";
    return vka_walk(s, st, &e, &b, ALIGN, 0, rd, NULL, r, 1, NULL, why, sizeof why);
}

int main(void) {
    static vka_bstate s;
    const uint64_t table = 0x100, uniforms = 0x200, buffer = GUEST_BASE + 0x1234, entry = 0x18;
    memcpy(arena + table + entry, &buffer, 8);                 // the table's entry: the buffer's game address
    const uint64_t table_guest = GUEST_BASE + table;
    memcpy(arena + uniforms + 0x40, &table_guest, 8);          // a uniform buffer holding the table's address
    s.ssbo[1][1] = (vka_range){(uintptr_t)arena + table, 0x40}; // the pixel stage's storage slot 1 points at the table
    s.ubo[1][2] = (vka_range){(uintptr_t)arena + uniforms, 0x100};
    const dmsh_vk_bind base = {.kind = VKA_KIND_SBUF_IND, .desc = 7 /* STORAGE_BUFFER */, .count = 1, .reg = 1, .soff = entry};
    vka_res r;

    // The address is the table entry's, bound from there (rounded down to the alignment) to the end of its chunk
    CHECK(walk(&s, 1, base, &r) == 1);
    CHECK(r.what == VKA_R_BUF && r.addr == (buffer & ~(uint64_t)(ALIGN - 1)) && r.size == 0);

    // The same table found through a uniform buffer word
    dmsh_vk_bind ubo = base;
    ubo.kind = VKA_KIND_SBUF_UBO_IND; ubo.reg = 2; ubo.off = 0x40;
    CHECK(walk(&s, 1, ubo, &r) == 1);
    CHECK(r.what == VKA_R_BUF && r.addr == (buffer & ~(uint64_t)(ALIGN - 1)) && r.size == 0);

    // A null entry, an unbound slot or an unreadable table binds nothing
    const uint64_t zero = 0;
    memcpy(arena + table + entry, &zero, 8);
    CHECK(walk(&s, 1, base, &r) == 1 && r.what == VKA_R_NULL);
    memcpy(arena + table + entry, &buffer, 8);
    CHECK(walk(&s, 0, base, &r) == 1 && r.what == VKA_R_NULL);
    s.ssbo[1][1].addr = 1;
    CHECK(walk(&s, 1, base, &r) == 1 && r.what == VKA_R_NULL);

    // The table's address in a compute uniform slot (NVN2 compute's c0 uniform table, read as UBO_PTR)
    s.ubo[5][3] = (vka_range){(uintptr_t)arena + table, 0x40};
    dmsh_vk_bind ubo_ptr = base;
    ubo_ptr.kind = VKA_KIND_UBO_PTR_IND; ubo_ptr.reg = 3;
    CHECK(walk(&s, 5, ubo_ptr, &r) == 1);
    CHECK(r.what == VKA_R_BUF && r.addr == (buffer & ~(uint64_t)(ALIGN - 1)) && r.size == 0);
    s.ubo[5][3].addr = 0;
    CHECK(walk(&s, 5, ubo_ptr, &r) == 1 && r.what == VKA_R_NULL);

    // An image handle in a uniform buffer word: its texture ID in the bound texture pool
    const uint32_t handle = 0x123 | 0x45u << 20;
    memcpy(arena + uniforms + 0x20, &handle, 4);
    s.tex_base = 0x1000;
    const dmsh_vk_bind img = {.kind = VKA_KIND_IMG_UBO, .desc = 3 /* STORAGE_IMAGE */, .count = 1, .reg = 2, .off = 0x20};
    CHECK(walk(&s, 1, img, &r) == 1);
    CHECK(r.what == VKA_R_IMG && r.tid == 0x1123);
    const uint32_t none = 0;
    memcpy(arena + uniforms + 0x20, &none, 4);
    CHECK(walk(&s, 1, img, &r) == 1 && r.what == VKA_R_NULL);

    // Texel buffers whose handle is in a uniform buffer word, uniform and storage: the buffer texture's ID
    memcpy(arena + uniforms + 0x20, &handle, 4);
    const dmsh_vk_bind tbuf = {.kind = VKA_KIND_TBUF_UBO, .desc = 4 /* UNIFORM_TEXEL_BUFFER */, .count = 1, .reg = 2, .off = 0x20};
    CHECK(walk(&s, 1, tbuf, &r) == 1);
    CHECK(r.what == VKA_R_TBUF && r.tid == 0x1123);
    dmsh_vk_bind image_buffer = img;
    image_buffer.desc = 5 /* STORAGE_TEXEL_BUFFER */;
    CHECK(walk(&s, 1, image_buffer, &r) == 1);
    CHECK(r.what == VKA_R_TBUF && r.tid == 0x1123);
    memcpy(arena + uniforms + 0x20, &none, 4);
    CHECK(walk(&s, 1, tbuf, &r) == 1 && r.what == VKA_R_NULL);

    // The image pool: no descriptor in set 0, the shader reads set 1
    const dmsh_vk_bind pool = {.kind = VKA_KIND_IMG_POOL, .desc = 3 /* STORAGE_IMAGE */, .set = DMSH_VK_SET_POOL, .binding = NVN_DIRECT_POOL_IMAGE_BINDING, .reg = -1};
    dmsh_vk_entry e = {.bind_count = 1};
    char why[200] = "";
    int uses_pool = 0;
    CHECK(vka_walk(&s, 5, &e, &pool, ALIGN, 0, rd, NULL, &r, 1, &uses_pool, why, sizeof why) == 0 && uses_pool);

    puts("nvn_bind_tests: ok");
    return 0;
}
