// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The front end's host-lent command memory segments (nvn_hostseg.c): a lent segment is reclaimed
// only when the game gives out again the guest bytes of the recording it hangs off.

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvn_hostseg.h"

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%d: %s\n", __LINE__, #expr); exit(1); } } while (0)

uint64_t dm_nvn_frame_count(void) { return 0; }
void dm_log(const char* file, int line, const char* func, const char* fmt, ...) {
    (void)file; (void)line; (void)func; (void)fmt;
}

static _Alignas(8) uint8_t guest[256];   // the game's command memory
static cmd_t o;

// AddCommandMemory's bookkeeping of a grant of n bytes at g (hle_nvn.c), without the JUMP it writes
static void grant(uint8_t* g, size_t n) {
    nvn_hostseg_release((uintptr_t)g, (uintptr_t)g + n);
    if (o.recording == 1) nvn_hostseg_adopt(&o, (uintptr_t)g, (uintptr_t)g + 8);
    o.cur = o.seg = g; o.end = g + n;
}
// A record that does not fit: the segment the front end lends for it (cmd_alloc_keep), and the record written there
static uint8_t* lend(size_t record) {
    const size_t n = nvn_hostseg_size(&o, 0, record);
    uint8_t* m = nvn_hostseg_take(&o, n);
    CHECK(m != NULL);
    o.cur = o.seg = m; o.end = m + n;
    if (o.recording == 2) { o.begin = m; o.recording = 1; }
    o.cur += record;
    return m;
}

int main(void) {
    // A 64-byte grant: a 48-byte record and an END fill it to 56
    grant(guest, 64);
    o.recording = 1; o.begin = guest; o.cur = guest + 56; o.recording = 0;
    // The next recording's first record does not fit in the 8 bytes left: it begins in a lent segment, and its callback grants the next 64 bytes, right after
    o.recording = 2;
    uint8_t* h = lend(48);
    grant(guest + 64, 64);
    CHECK(nvn_hostseg_busy(h) == 1);   // (its range was built from the old write pointer: [guest, guest + 72) took in the new grant, which freed it)
    // It ends at once with an END at the start of the new grant, and the game packs the next recording right behind that
    o.cur = guest + 72; o.recording = 0;
    nvn_hostseg_release((uintptr_t)guest + 72, (uintptr_t)guest + 128);
    CHECK(nvn_hostseg_busy(h) == 1);
    // The game gives out the grant again once the recording is dead: the segment is reclaimed
    nvn_hostseg_release((uintptr_t)guest + 64, (uintptr_t)guest + 128);
    CHECK(nvn_hostseg_busy(h) == 0);

    // A recording that runs out in the middle of guest memory hangs off its own bytes there, up to the JUMP
    grant(guest + 128, 64);
    o.recording = 1; o.begin = guest + 128; o.cur = guest + 184;
    uint8_t* h2 = lend(64);
    CHECK(h2 != NULL);
    nvn_hostseg_release((uintptr_t)guest + 200, (uintptr_t)guest + 256);   // bytes after the JUMP: not its own
    CHECK(nvn_hostseg_busy(h2) == 1);
    nvn_hostseg_release((uintptr_t)guest + 128, (uintptr_t)guest + 192);
    CHECK(nvn_hostseg_busy(h2) == 0);

    nvn_hostseg_destroy();
    puts("nvn_hostseg_tests: ok");
    return 0;
}
