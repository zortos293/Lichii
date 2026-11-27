// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dm.h"
#include "nvn_dirty.h"
#include "video_core/nvn_direct/nvn_direct.h"
#include "video_core/nvn_direct/nvn_direct_front.h"

enum { PAGE = 4096, NPAGES = 8, GUEST_BASE = 0x10000 };
static _Alignas(PAGE) uint8_t arena[NPAGES * PAGE];
static int armed[NPAGES], arms, disarms;
// Pages whose protection (refuse_arm) or release (refuse_disarm) is refused, as a failed
// VirtualProtect does: the rest of the range changes and the call returns 0. With lenient set,
// unprotected pages may be released (the tracker undoes a failed arming over its whole run).
static int refuse_arm[NPAGES], refuse_disarm[NPAGES], refusals, lenient;

void mtl_pkg_stats(uint64_t* uploads, uint64_t* reuploads);

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%d: %s\n", __LINE__, #expr); exit(1); } } while (0)

uint64_t nvn_direct_guest_of(uint64_t host) {
    return GUEST_BASE + host - (uintptr_t)arena;
}

uint64_t nvn_direct_host_of(uint64_t guest) {
    return (uintptr_t)arena + guest - GUEST_BASE;
}

int nvn_direct_track_pages(uint64_t address, uint64_t size, int on) {
    CHECK(address >= GUEST_BASE && address - GUEST_BASE + size <= sizeof arena);
    CHECK(size && address % PAGE == 0 && size % PAGE == 0);
    int done = 1;
    for (size_t page = (address - GUEST_BASE) / PAGE; size; page++, size -= PAGE) {
        if (on ? refuse_arm[page] : refuse_disarm[page]) {
            refusals++;
            done = 0;
            continue;
        }
        CHECK(armed[page] != on || (!on && lenient));
        armed[page] = on;
    }
    if (on) arms++; else disarms++;
    return done;
}

void dm_log(const char* file, int line, const char* function, const char* format, ...) {
    (void)file;
    (void)line;
    (void)function;
    (void)format;
}

void mtl_pkg_stats(uint64_t* uploads, uint64_t* reuploads) {
    *uploads = *reuploads = 0;
}

static void test_nested(void) {
    static const int outer, inner, short_alias;
    uint8_t* shared = arena + 2 * PAGE;
    nvn_dirty_add_range(&inner, shared, 2 * PAGE);
    const void* inner_span = nvn_dirty_span(shared, 2 * PAGE);
    nvn_dirty_add_range(&outer, arena, 5 * PAGE);
    const void* outer_span = nvn_dirty_span(arena, 5 * PAGE);
    CHECK(inner_span && outer_span && inner_span != outer_span);
    const uint64_t stamp = nvn_dirty_now();
    nvn_dirty_protect(arena, 5 * PAGE);
    CHECK(arms == 1 && armed[0] && armed[4]);
    nvn_direct_cpu_written(GUEST_BASE + 2 * PAGE + 4, 1);
    size_t lo, hi;
    CHECK(nvn_dirty_bounds_since(outer_span, arena, 5 * PAGE, stamp, &lo, &hi));
    CHECK(lo == 2 * PAGE && hi == 3 * PAGE);
    CHECK(nvn_dirty_since_span(inner_span, shared, 2 * PAGE, stamp));
    CHECK(!armed[2] && armed[1] && armed[3] && disarms == 1);
    CHECK(nvn_dirty_now() == stamp + 1);
    CHECK(nvn_dirty_tracked(shared + PAGE, 2 * PAGE));

    nvn_dirty_range log[2];
    CHECK(nvn_dirty_take(log, 2) == 1);
    CHECK(log[0].lo == (uintptr_t)shared && log[0].hi == (uintptr_t)shared + PAGE);
    nvn_dirty_add_range(&short_alias, shared + PAGE, 1);
    const uint64_t before_tail = nvn_dirty_now();
    dm_guest_write_prepare(arena + 4 * PAGE + 4, 1);
    CHECK(nvn_dirty_since_span(outer_span, arena, 5 * PAGE, before_tail));
    CHECK(!nvn_dirty_since_span(inner_span, shared, 2 * PAGE, before_tail));
    CHECK(!armed[4] && armed[3]);

    nvn_dirty_protect(shared, 2 * PAGE);
    nvn_dirty_remove_range(&outer);
    CHECK(!armed[0] && !armed[1] && armed[2] && armed[3]);
    nvn_dirty_remove_range(&inner);
    CHECK(!armed[2] && armed[3]);
    nvn_dirty_remove_range(&short_alias);
    CHECK(!armed[3]);
    CHECK(nvn_dirty_take(log, 2) == 1);
}

static void test_same_start_and_host_pages(void) {
    static const int narrow, broad, late;
    uint8_t* base = arena + PAGE;
    nvn_dirty_add_range(&broad, base, 3 * PAGE);
    nvn_dirty_protect(base, 3 * PAGE);
    nvn_dirty_add_range(&narrow, base, PAGE);
    nvn_dirty_remove_range(&narrow);
    CHECK(nvn_dirty_tracked(base, 3 * PAGE));
    CHECK(armed[1] && armed[2] && armed[3]);
    nvn_dirty_add_range(&late, base + PAGE + 8, PAGE);
    nvn_direct_host_written(base + PAGE, 1);
    CHECK(armed[1] && !armed[2] && armed[3]);
    nvn_dirty_remove_range(&late);
    nvn_dirty_add_range(&late, base + PAGE + 8, PAGE);
    nvn_dirty_remove_range(&broad);
    CHECK(!armed[1] && !armed[2] && armed[3]);
    nvn_dirty_protect(base + PAGE + 8, PAGE);
    CHECK(!armed[2] && armed[3]);

    const uint64_t stamp = nvn_dirty_now();
    nvn_direct_cpu_written(GUEST_BASE + 3 * PAGE + 128, 1);
    CHECK(!armed[3]);
    CHECK(nvn_dirty_since(base + PAGE + 8, PAGE, stamp));
    nvn_dirty_remove_range(&late);
    nvn_dirty_add_range(&broad, base, 3 * PAGE);
    nvn_dirty_protect(base, 3 * PAGE);
    CHECK(armed[1] && armed[2] && armed[3]);
    nvn_dirty_remove_range(&broad);
    CHECK(!armed[1] && !armed[2] && !armed[3]);
}

// A protection that fails leaves its pages unarmed in every span over them, and the textures over
// them dirty at each check until a later protection succeeds.
static void test_failed_protection(void) {
    static const int pool, alias;
    uint8_t* base = arena + 5 * PAGE;
    nvn_dirty_add_range(&pool, base, 3 * PAGE);
    nvn_dirty_add_range(&alias, base + PAGE, PAGE);
    const void* alias_span = nvn_dirty_span(base + PAGE, PAGE);
    nvn_dirty_range log[2];
    nvn_dirty_take(log, 2);

    const uint64_t stamp = nvn_dirty_now();
    const int arms_before = arms, disarms_before = disarms;
    refuse_arm[5] = refuse_arm[6] = refuse_arm[7] = 1;
    lenient = 1;
    nvn_dirty_protect(base, 3 * PAGE);
    CHECK(refusals == 3 && arms == arms_before + 1 && disarms == disarms_before + 1);
    CHECK(!armed[5] && !armed[6] && !armed[7]);
    CHECK(nvn_dirty_now() > stamp);
    CHECK(nvn_dirty_take(log, 2) == 1);
    CHECK(log[0].lo == (uintptr_t)base && log[0].hi == (uintptr_t)base + 3 * PAGE);
    const uint64_t after = nvn_dirty_now();
    CHECK(nvn_dirty_since(base, 3 * PAGE, after));
    CHECK(nvn_dirty_since_span(alias_span, base + PAGE, PAGE, after));
    refuse_arm[5] = refuse_arm[6] = refuse_arm[7] = 0;

    // The alias span did not take the failed run as armed: protecting it arms its page.
    nvn_dirty_remove_range(&pool);
    CHECK(arms == arms_before + 1 && disarms == disarms_before + 1);
    nvn_dirty_protect(base + PAGE, PAGE);
    CHECK(arms == arms_before + 2 && armed[6]);
    CHECK(nvn_dirty_since_span(alias_span, base + PAGE, PAGE, after));
    CHECK(nvn_dirty_take(log, 2) == 1);
    CHECK(log[0].lo == (uintptr_t)base + PAGE && log[0].hi == (uintptr_t)base + 2 * PAGE);

    // Tracked from there.
    const uint64_t tracked = nvn_dirty_now();
    CHECK(!nvn_dirty_since_span(alias_span, base + PAGE, PAGE, tracked));
    nvn_direct_cpu_written(GUEST_BASE + 6 * PAGE + 8, 1);
    CHECK(!armed[6] && nvn_dirty_since_span(alias_span, base + PAGE, PAGE, tracked));
    nvn_dirty_remove_range(&alias);
    lenient = 0;
}

// An arming that protects part of its run and whose undo releases part of it: the pages left
// protected stay armed, so a host write's preparation releases them, and all stay dirty until an
// arming succeeds.
static void test_partial_protection(void) {
    static const int pool;
    nvn_dirty_add_range(&pool, arena, 3 * PAGE);
    nvn_dirty_range log[2];
    nvn_dirty_take(log, 2);

    const uint64_t stamp = nvn_dirty_now();
    refuse_arm[1] = 1;
    refuse_disarm[0] = 1;
    lenient = 1;
    nvn_dirty_protect(arena, 3 * PAGE);
    CHECK(armed[0] && !armed[1] && !armed[2]);
    CHECK(nvn_dirty_since(arena, PAGE, stamp) && nvn_dirty_since(arena + 2 * PAGE, PAGE, stamp));
    CHECK(nvn_dirty_take(log, 2) == 1);
    CHECK(log[0].lo == (uintptr_t)arena && log[0].hi == (uintptr_t)arena + 3 * PAGE);

    // A host write tries to release the page left protected until it works.
    dm_guest_write_prepare(arena + 8, 1);
    CHECK(armed[0]);
    refuse_disarm[0] = 0;
    dm_guest_write_prepare(arena + 8, 1);
    CHECK(!armed[0]);

    // The next arming protects the whole run; the pages count as written once more.
    refuse_arm[1] = 0;
    const uint64_t retry = nvn_dirty_now();
    nvn_dirty_protect(arena, 3 * PAGE);
    CHECK(armed[0] && armed[1] && armed[2]);
    CHECK(nvn_dirty_since(arena + PAGE, PAGE, retry));
    const uint64_t tracked = nvn_dirty_now();
    CHECK(!nvn_dirty_since(arena, 3 * PAGE, tracked));

    // A disarming that releases part of its run keeps the rest armed; the next arming skips it.
    refuse_disarm[1] = 1;
    dm_guest_write_prepare(arena + 8, 3 * PAGE - 16);
    CHECK(!armed[0] && armed[1] && !armed[2]);
    const int arms_before = arms;
    nvn_dirty_protect(arena, 3 * PAGE);
    CHECK(armed[0] && armed[1] && armed[2] && arms == arms_before + 2);
    refuse_disarm[1] = 0;
    nvn_dirty_remove_range(&pool);
    CHECK(!armed[0] && !armed[1] && !armed[2]);
    lenient = 0;
}

int main(void) {
    test_nested();
    test_same_start_and_host_pages();
    test_failed_protection();
    test_partial_protection();
    nvn_tracking_destroy();
}
