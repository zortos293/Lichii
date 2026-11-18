// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The host functions Banana Native's NVN front end and Vulkan replay call (upstream's host in
// dm/host implements them for a game it runs natively), as Lichii has them: there is no native
// module table, cheat engine, frame generation or custom driver; the guest's memory is Lichii's.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nvn_count.h"
#include "nvn_direct_front.h"

uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
uint64_t g_t0_ns;
__attribute__((constructor)) static void start_time(void) { g_t0_ns = now_ns(); }
uint64_t guest_ns(void) { return now_ns() - g_t0_ns; }

// The front end's slot table names (upstream's names.txt, nvn_thunks.S): Lichii's hook calls the
// implementations itself, so the thunks and their entry stubs are never run.
const char* nvn_names[NVN_NFUNC] = {
#include "nvn_names.inc"
};
void* nvn_thunk[NVN_NFUNC];
void nvn_unimpl_entry(void) { abort(); }
void nvn_prof_entry(void) { abort(); }
void nvn_big_entry(void) { abort(); }

typedef struct module_t module_t;
module_t* g_mod;
int g_nmod;
module_t* module_of(const void* p) { (void)p; return NULL; }
const char* dm_module_dir(void) { return "."; }
void dm_backtrace(uint64_t fp) { (void)fp; }
int dm_detclock_enabled(void) { return 0; }
void dm_guest_frame_tick(void) {}
void dm_cheats_present(void) {}
void crash_set_wait_dump(void (*fn)(void)) { (void)fn; }
void dm_log(const char* file, int line, const char* func, const char* fmt, ...); // dm.h
void dm_host_shutdown(const char* why) {
    dm_log(__FILE__, __LINE__, __func__, "[dm] FATAL the replay stops the emulator: %s", why ? why : "");
    abort();
}
int front_screen_max_fps(void) { return 0; }
void dm_vi_layer_crop(int c[4]) { c[0] = c[1] = c[2] = c[3] = 0; }
void* vka_driver_open(void) { return NULL; }
const char* vka_driver_tag(void) { return NULL; }
void nxhost_framegen_set(int on, int multiplier, int target_rate, int flow_pct) {
    (void)on; (void)multiplier; (void)target_rate; (void)flow_pct;
}
struct vka_img;
int vka_fg_frame(uint64_t serial, const int* crop, double period, uint64_t fence, struct vka_img** gen,
                 double* hold, double* out_period) {
    (void)serial; (void)crop; (void)period; (void)fence; (void)gen; (void)hold;
    if (out_period) *out_period = 0;
    return 0;
}

// Guest memory. The front end and the replay reach it through host pointers into Lichii's
// fastmem arena (the dispatch adds the arena's base to the game's addresses). Pages the guest has
// not mapped are inaccessible there: the readers that check for them (texture uploads from pool
// storage) read through the backing memory (nvn_direct_readable), which a concurrent unmap leaves
// readable. Write tracking is Lichii's (nvn_tracking.c): the replay's own fault and pre-write
// hooks are not installed.
void dm_fault_hook_register(int (*fn)(void* addr, int is_write, void* uc)) { (void)fn; }
void dm_guest_write_prep_register(void (*fn)(void* addr, size_t len)) { (void)fn; }
uint64_t dm_guest_unmapped(uintptr_t a, size_t n) {
    uint64_t c = 0;
    for (uintptr_t p = a & ~(uintptr_t)0xFFF; n && p < a + n; p += 0x1000) c += !nvn_direct_readable(p);
    return c;
}
uint64_t dm_guest_read_mapped(void* dst, uintptr_t a, size_t n) {
    uint64_t c = 0;
    uint8_t* d = dst;
    for (uintptr_t p = a; p < a + n;) {
        uintptr_t e = (p & ~(uintptr_t)0xFFF) + 0x1000;
        if (e > a + n) e = a + n;
        const void* s = nvn_direct_readable(p);
        if (s) memcpy(d + (p - a), s, e - p);
        else { memset(d + (p - a), 0, e - p); c++; }
        p = e;
    }
    return c;
}
// The placement swap's guest block (vka_mem.c): pools are not placed in Lichii.
void svc_guest_range(uint8_t** base, size_t* size) { *base = NULL; *size = 0; }
