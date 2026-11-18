// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// CPU write tracking of memory pools in direct mode: upstream's nvn_dirty.h
// interface (its nvn_dirty.c, MIT, is not built: its own fault handler and mprotect calls would
// fight Lichii's fastmem protections) over Lichii's page types. Protecting pages arms them
// (track_pages: rasterizer-cached pages by guest address, which stay readable in direct mode:
// the replay reads them in place); a CPU write to an armed page takes Core::Memory's rasterizer
// write path, which reports it here (nvn_direct_cpu_written): the pages get a new generation and
// are disarmed. The pools are not mapped in the SMMU: with them there MKW stalled at ~127
// presents, one guest core busy (runs nd-bis-2..4). As upstream, a
// texture's upload takes nvn_dirty_now, protects its pages, then reads them; it is uploaded again
// once any page has a generation past that stamp.
//
// Host writes into pools: the completion thread's event values and counter reports go through
// the backing memory (nvn_direct_writable), which is never protected; the front end's command
// records, written in place into command memory, are on pages never armed
// (nvn_direct_host_written); host writes that change texture storage (readbacks of linear
// textures) disarm and stamp first (dm_guest_write_prepare).

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "nvn_direct.h"
#include "nvn_direct_front.h"
#include "nvn_dirty.h"

enum { PAGE_SHIFT = 12 };
#define PAGE_SIZE (1ull << PAGE_SHIFT)

// A registered pool. Kept (live 0) after it goes, until the teardown: images hold its pointer
// (nvn_dirty_span).
typedef struct {
    uintptr_t lo, hi; // host pointers into the arena
    _Atomic uint64_t* gen; // per page: generation of the last CPU write seen (0: none)
    uint8_t* armed;        // per page: Lichii tracks writes to it (under g_mu)
    uint8_t* host;         // per page: the front end writes it in place (nvn_direct_host_written): never armed
    _Atomic int live;
} span_t;

static span_t** g_spans; // live spans by lo (under g_mu for changes, g_rw for lookups)
static int g_nspans, g_capspans;
static span_t** g_gone; // spans of pools that went (under g_mu)
static int g_ngone, g_capgone;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t g_rw = PTHREAD_RWLOCK_INITIALIZER;
static _Atomic uint64_t g_now = 1;
static _Atomic uint64_t g_faults, g_preps;

static span_t* find_locked(uintptr_t a, size_t len) {
    int lo = 0, hi = g_nspans - 1, hit = -1;
    while (lo <= hi) {
        const int m = (lo + hi) / 2;
        if (g_spans[m]->lo <= a) { hit = m; lo = m + 1; } else hi = m - 1;
    }
    return hit >= 0 && a + len <= g_spans[hit]->hi ? g_spans[hit] : NULL;
}
static span_t* find(uintptr_t a, size_t len) {
    pthread_rwlock_rdlock(&g_rw);
    span_t* s = find_locked(a, len);
    pthread_rwlock_unlock(&g_rw);
    return s;
}

void nvn_dirty_add_range(const void* mem, size_t size) {
    const uintptr_t lo = (uintptr_t)mem, hi = lo + size;
    if (!size || nvn_direct_guest_of(lo) == lo) return; // not guest memory (a host segment)
    span_t* s = calloc(1, sizeof *s);
    const size_t pages = (size + PAGE_SIZE - 1 + (lo & (PAGE_SIZE - 1))) >> PAGE_SHIFT;
    if (s) { s->gen = calloc(pages, sizeof *s->gen); s->armed = calloc(pages, 1); s->host = calloc(pages, 1); }
    if (!s || !s->gen || !s->armed || !s->host) goto fail;
    s->lo = lo & ~(uintptr_t)(PAGE_SIZE - 1);
    s->hi = hi;
    atomic_store(&s->live, 1);
    pthread_mutex_lock(&g_mu);
    pthread_rwlock_wrlock(&g_rw);
    if (g_nspans == g_capspans) {
        const int cap = g_capspans ? 2 * g_capspans : 256;
        span_t** spans = realloc(g_spans, (size_t)cap * sizeof *g_spans);
        if (!spans) { pthread_rwlock_unlock(&g_rw); pthread_mutex_unlock(&g_mu); goto fail; }
        g_spans = spans;
        g_capspans = cap;
    }
    int i = g_nspans;
    while (i > 0 && g_spans[i - 1]->lo > s->lo) { g_spans[i] = g_spans[i - 1]; i--; }
    g_spans[i] = s;
    g_nspans++;
    pthread_rwlock_unlock(&g_rw);
    pthread_mutex_unlock(&g_mu);
    return;
fail:
    if (s) { free(s->gen); free(s->armed); free(s->host); free(s); }
    LOG("[RED] pool %p+%#zx: out of memory for its write tracking: CPU writes to its textures after their first upload are not seen", mem, size);
}

// Arms (on) or disarms the pages [p, q] of s that are not already so, in runs (caller holds g_mu)
static void arm_pages(span_t* s, uintptr_t p, uintptr_t q, int on) {
    uintptr_t run = 0;
    int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        const int change = i <= q && s->armed[i] != on && !(on && s->host[i]);
        if (change) {
            s->armed[i] = (uint8_t)on;
            if (!open) { run = i; open = 1; }
            continue;
        }
        if (open) {
            nvn_direct_track_pages(nvn_direct_guest_of(s->lo + (run << PAGE_SHIFT)), (i - run) << PAGE_SHIFT, on);
            open = 0;
        }
    }
}

void nvn_dirty_remove_range(const void* mem) {
    pthread_mutex_lock(&g_mu);
    pthread_rwlock_wrlock(&g_rw);
    span_t* s = NULL;
    for (int i = 0; i < g_nspans; i++) {
        if (g_spans[i]->lo != ((uintptr_t)mem & ~(uintptr_t)(PAGE_SIZE - 1))) continue;
        s = g_spans[i];
        memmove(&g_spans[i], &g_spans[i + 1], (size_t)(g_nspans - i - 1) * sizeof *g_spans);
        g_nspans--;
        break;
    }
    pthread_rwlock_unlock(&g_rw);
    if (s) {
        arm_pages(s, 0, ((s->hi - s->lo - 1) >> PAGE_SHIFT), 0);
        atomic_store(&s->live, 0);
        if (g_ngone == g_capgone) {
            const int cap = g_capgone ? 2 * g_capgone : 256;
            span_t** gone = realloc(g_gone, (size_t)cap * sizeof *g_gone);
            if (gone) { g_gone = gone; g_capgone = cap; }
        }
        if (g_ngone < g_capgone) g_gone[g_ngone++] = s;
    }
    pthread_mutex_unlock(&g_mu);
}

// The teardown (nvn_direct_destroy, after the replay's images went): every span, live or gone.
void nvn_tracking_destroy(void) {
    for (int k = 0; k < 2; k++) {
        span_t** const spans = k ? g_gone : g_spans;
        for (int i = 0, n = k ? g_ngone : g_nspans; i < n; i++) {
            free(spans[i]->gen);
            free(spans[i]->armed);
            free(spans[i]->host);
            free(spans[i]);
        }
        free(spans);
    }
}

void nvn_direct_host_written(const void* mem, size_t size) {
    if (!size) return;
    pthread_mutex_lock(&g_mu);
    span_t* s = find((uintptr_t)mem, size);
    if (s) {
        const uintptr_t a = (uintptr_t)mem;
        const uintptr_t p = (a - s->lo) >> PAGE_SHIFT, q = (a + size - 1 - s->lo) >> PAGE_SHIFT;
        arm_pages(s, p, q, 0);
        for (uintptr_t i = p; i <= q; i++) s->host[i] = 1;
    }
    pthread_mutex_unlock(&g_mu);
}

int nvn_dirty_tracked(const void* addr, size_t len) { return len && find((uintptr_t)addr, len) != NULL; }
const void* nvn_dirty_span(const void* addr, size_t len) { return len ? find((uintptr_t)addr, len) : NULL; }
uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }

void nvn_dirty_protect(const void* addr, size_t len) {
    if (!len) return;
    pthread_mutex_lock(&g_mu);
    span_t* s = find((uintptr_t)addr, len);
    if (s) {
        const uintptr_t a = (uintptr_t)addr;
        arm_pages(s, (a - s->lo) >> PAGE_SHIFT, (a + len - 1 - s->lo) >> PAGE_SHIFT, 1);
    }
    pthread_mutex_unlock(&g_mu);
}

// The page ranges written since the replay last took them (nvn_dirty_take): it re-uploads the
// images over them instead of checking every live image. Past WLOG_N ranges the log is lost and
// the taker checks them all (under g_mu).
enum { WLOG_N = 4096 };
static nvn_dirty_range g_wlog[WLOG_N];
static int g_nwlog, g_wlog_lost;
static void wlog_add(uintptr_t lo, uintptr_t hi) {
    nvn_dirty_range* last = g_nwlog ? &g_wlog[g_nwlog - 1] : NULL;
    if (last && lo <= last->hi && hi >= last->lo) {
        if (lo < last->lo) last->lo = lo;
        if (hi > last->hi) last->hi = hi;
        return;
    }
    if (g_nwlog == WLOG_N) { g_wlog_lost = 1; return; }
    g_wlog[g_nwlog++] = (nvn_dirty_range){lo, hi};
}
int nvn_dirty_take(nvn_dirty_range* out, int max) {
    pthread_mutex_lock(&g_mu);
    const int n = g_wlog_lost || g_nwlog > max ? -1 : g_nwlog;
    if (n > 0) memcpy(out, g_wlog, (size_t)n * sizeof *out);
    g_nwlog = 0;
    g_wlog_lost = 0;
    pthread_mutex_unlock(&g_mu);
    return n;
}

// A CPU write Lichii saw on guest [address, address + size): new generation, pages disarmed, in
// every span the range touches (spans change only under g_mu, which is held).
static void written(uintptr_t a, size_t len) {
    if (!len) return;
    const uintptr_t e = a + len;
    pthread_mutex_lock(&g_mu);
    int k = 0, lo = 0, hi = g_nspans - 1;
    while (lo <= hi) {
        const int m = (lo + hi) / 2;
        if (g_spans[m]->lo <= a) { k = m; lo = m + 1; } else hi = m - 1;
    }
    uint64_t g = 0;
    for (; k < g_nspans && g_spans[k]->lo < e; k++) {
        span_t* s = g_spans[k];
        if (s->hi <= a) continue;
        if (!g) g = atomic_fetch_add(&g_now, 1) + 1;
        const uintptr_t b = a > s->lo ? a : s->lo, end = e < s->hi ? e : s->hi;
        const uintptr_t p = (b - s->lo) >> PAGE_SHIFT, q = (end - 1 - s->lo) >> PAGE_SHIFT;
        for (uintptr_t i = p; i <= q; i++) atomic_store(&s->gen[i], g);
        arm_pages(s, p, q, 0);
        wlog_add(s->lo + (p << PAGE_SHIFT), s->lo + ((q + 1) << PAGE_SHIFT));
    }
    pthread_mutex_unlock(&g_mu);
}
void mtl_pkg_stats(uint64_t* uploads, uint64_t* reuploads); // vka_tex.c
void nvn_direct_cpu_written(uint64_t address, uint64_t size) {
    const uint64_t n = atomic_fetch_add(&g_faults, 1) + 1;
    written((uintptr_t)nvn_direct_host_of(address), size);
    if (n <= 4 || !(n & (n - 1))) {
        uint64_t uploads = 0, reuploads = 0;
        mtl_pkg_stats(&uploads, &reuploads);
        LOG("[vk] tracked pool memory: %llu CPU writes seen (the last at %#llx+%#llx); textures uploaded from storage %llu, again after a write %llu",
            (unsigned long long)n, (unsigned long long)address, (unsigned long long)size, (unsigned long long)uploads, (unsigned long long)reuploads);
    }
}
void dm_guest_write_prepare(void* addr, size_t len) {
    atomic_fetch_add(&g_preps, 1);
    written((uintptr_t)addr, len);
}

int nvn_dirty_since_span(const void* span, const void* addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    const span_t* s = span;
    const uintptr_t a = (uintptr_t)addr;
    if (!s || !atomic_load(&s->live) || a < s->lo || a + len > s->hi) s = find(a, len);
    if (!s) return 1; // untracked: uploaded again rather than shown stale
    const uintptr_t p = (a - s->lo) >> PAGE_SHIFT, q = (a + len - 1 - s->lo) >> PAGE_SHIFT;
    for (uintptr_t i = p; i <= q; i++) if (atomic_load(&s->gen[i]) > stamp) return 1;
    return 0;
}
int nvn_dirty_since(const void* addr, size_t len, uint64_t stamp) { return nvn_dirty_since_span(NULL, addr, len, stamp); }

int nvn_dirty_bounds_since(const void* span, const void* addr, size_t len, uint64_t stamp, size_t* lo, size_t* hi) {
    *lo = 0;
    *hi = len;
    if (!len) return 0;
    const span_t* s = span;
    const uintptr_t a = (uintptr_t)addr;
    if (!s || !atomic_load(&s->live) || a < s->lo || a + len > s->hi) s = find(a, len);
    if (!s) return 1;
    uintptr_t f = UINTPTR_MAX, l = 0;
    const uintptr_t p = (a - s->lo) >> PAGE_SHIFT, q = (a + len - 1 - s->lo) >> PAGE_SHIFT;
    for (uintptr_t i = p; i <= q; i++) {
        if (atomic_load(&s->gen[i]) > stamp) { if (f == UINTPTR_MAX) f = i; l = i; }
    }
    if (f == UINTPTR_MAX) return 0;
    const uintptr_t b0 = s->lo + (f << PAGE_SHIFT), b1 = s->lo + ((l + 1) << PAGE_SHIFT);
    *lo = b0 > a ? b0 - a : 0;
    *hi = b1 - a < len ? b1 - a : len;
    return 1;
}

// Upstream's placement swap held the tracker over a range it remapped; imports remap nothing.
void nvn_dirty_hold(const void* addr, size_t len) { (void)addr; (void)len; }
void nvn_dirty_unhold(void) {}
void nvn_dirty_stats(uint64_t* faults, uint64_t* preps) { *faults = atomic_load(&g_faults); *preps = atomic_load(&g_preps); }
void nvn_dirty_mp_stats(uint64_t* calls, uint64_t* pages, uint64_t* ns) { *calls = *pages = *ns = 0; }
void nvn_dirty_debug(const void* span, const void* addr, size_t len, uint64_t stamp, char* out, size_t n) {
    (void)span; (void)addr; (void)len; (void)stamp;
    if (n) out[0] = 0;
}
