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
    const void* pool;
    uintptr_t lo, hi, prefix_hi; // bytes in the arena, and the maximum hi through this span
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
// The generation of a page whose arming failed: writes to it may go unseen, so every texture over
// it counts as written until a later arming of it succeeds (upstream's GEN_UNPROT).
#define GEN_UNPROT UINT64_MAX
static int g_unprot_seen; // an arming failed once: arming looks for GEN_UNPROT pages (under g_mu)

static uintptr_t page_lo(uintptr_t a) { return a & ~(uintptr_t)(PAGE_SIZE - 1); }
static uintptr_t page_hi(uintptr_t e) { return page_lo(e - 1) + PAGE_SIZE; }

static int first_after(uintptr_t a) {
    int lo = 0, hi = g_nspans;
    while (lo < hi) {
        const int m = lo + (hi - lo) / 2;
        if (g_spans[m]->prefix_hi <= a) lo = m + 1;
        else hi = m;
    }
    return lo;
}

static span_t* find_locked(uintptr_t a, size_t len) {
    const int i = first_after(a + len - 1);
    return i < g_nspans && g_spans[i]->lo <= a ? g_spans[i] : NULL;
}
static span_t* find(uintptr_t a, size_t len) {
    pthread_rwlock_rdlock(&g_rw);
    span_t* s = find_locked(a, len);
    pthread_rwlock_unlock(&g_rw);
    return s;
}

static void update_prefix(int i) {
    uintptr_t hi = i ? g_spans[i - 1]->prefix_hi : 0;
    for (; i < g_nspans; i++) {
        span_t* s = g_spans[i];
        if (s->hi > hi) hi = s->hi;
        if (s->prefix_hi == hi) break;
        s->prefix_hi = hi;
    }
}

void nvn_dirty_add_range(const void* pool, const void* mem, size_t size) {
    const uintptr_t lo = (uintptr_t)mem, hi = lo + size;
    if (!size || nvn_direct_guest_of(lo) == lo) return; // not guest memory (a host segment)
    span_t* s = calloc(1, sizeof *s);
    const size_t pages = (size + PAGE_SIZE - 1 + (lo & (PAGE_SIZE - 1))) >> PAGE_SHIFT;
    if (s) { s->gen = calloc(pages, sizeof *s->gen); s->armed = calloc(pages, 1); s->host = calloc(pages, 1); }
    if (!s || !s->gen || !s->armed || !s->host) goto fail;
    s->pool = pool;
    s->lo = lo;
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
    // A page has one generation and protection state even if it belongs to several pools.
    const uintptr_t base = page_lo(lo), end = page_hi(hi);
    uintptr_t copied = base;
    for (int k = first_after(base); k < g_nspans && g_spans[k]->lo < end && copied < end; k++) {
        const span_t* other = g_spans[k];
        if (other->hi <= copied) continue;
        const uintptr_t ob = page_lo(other->lo), oe = page_hi(other->hi);
        const uintptr_t b = ob > copied ? ob : copied, e = oe < end ? oe : end;
        const uintptr_t dst = (b - base) >> PAGE_SHIFT, src = (b - ob) >> PAGE_SHIFT;
        const size_t n = (e - b) >> PAGE_SHIFT;
        for (size_t j = 0; j < n; j++) atomic_store(&s->gen[dst + j], atomic_load(&other->gen[src + j]));
        memcpy(s->armed + dst, other->armed + src, n);
        memcpy(s->host + dst, other->host + src, n);
        copied = e;
    }
    int i = g_nspans;
    while (i > 0 && g_spans[i - 1]->lo > s->lo) { g_spans[i] = g_spans[i - 1]; i--; }
    g_spans[i] = s;
    g_nspans++;
    update_prefix(i);
    pthread_rwlock_unlock(&g_rw);
    pthread_mutex_unlock(&g_mu);
    return;
fail:
    if (s) { free(s->gen); free(s->armed); free(s->host); free(s); }
    LOG("[RED] pool %p+%#zx: out of memory for its write tracking: CPU writes to its textures after their first upload are not seen", mem, size);
}

static void wlog_add(uintptr_t lo, uintptr_t hi);

// Sets the arm bit of the pages [lo, hi) in every span over them (caller holds g_mu); unprot gives
// them GEN_UNPROT, restamp gives those that had it a new generation.
static void set_armed(uintptr_t lo, uintptr_t hi, int armed, int unprot, int restamp, uint64_t* g) {
    for (int i = first_after(lo); i < g_nspans && g_spans[i]->lo < hi; i++) {
        span_t* s = g_spans[i];
        if (s->hi <= lo) continue;
        const uintptr_t base = page_lo(s->lo), end = page_hi(s->hi);
        const uintptr_t b = base > lo ? base : lo, e = end < hi ? end : hi;
        for (uintptr_t j = (b - base) >> PAGE_SHIFT; j < (e - base) >> PAGE_SHIFT; j++) {
            s->armed[j] = (uint8_t)armed;
            if (unprot) {
                atomic_store(&s->gen[j], GEN_UNPROT);
            } else if (restamp && atomic_load(&s->gen[j]) == GEN_UNPROT) {
                if (!*g) *g = atomic_load(&g_now) + 1;
                atomic_store(&s->gen[j], *g);
            }
        }
    }
}

// Records whether arming (on) or disarming the pages [lo, hi) worked (caller holds g_mu). A page
// counts as armed while it may be protected, so the next write or host write
// (dm_guest_write_prepare) releases it. A failed arming is undone and a failed disarming tried
// again, page by page where the whole run cannot be released (Windows may have changed part of
// it, as upstream's nvn_dirty.c): pages left protected stay armed. The pages of a failed arming get
// GEN_UNPROT and are logged as written, so the textures over them are uploaded again at each
// check, whose upload tries to arm them again. Once that works, pages still GEN_UNPROT count as
// written now (one more upload) and are tracked from there.
static void track_result(uintptr_t lo, uintptr_t hi, int on, int done) {
    uint64_t g = 0;
    if (done) {
        set_armed(lo, hi, on, 0, on && g_unprot_seen, &g);
    } else {
        if (on) g_unprot_seen = 1;
        if (on && nvn_direct_track_pages(nvn_direct_guest_of(lo), hi - lo, 0)) {
            set_armed(lo, hi, 0, 1, 0, &g);
        } else {
            for (uintptr_t a = lo; a < hi; a += PAGE_SIZE) {
                const int released = nvn_direct_track_pages(nvn_direct_guest_of(a), PAGE_SIZE, 0);
                set_armed(a, a + PAGE_SIZE, !released, on, 0, &g);
            }
        }
        if (on) g = atomic_load(&g_now) + 1;
    }
    if (g) {
        wlog_add(lo, hi);
        atomic_store(&g_now, g);
    }
}

// Arms or disarms pages in [lo, hi) of s in runs, each run's result recorded in every span over it
// (caller holds g_mu).
static void arm_pages(span_t* s, uintptr_t lo, uintptr_t hi, int on) {
    const uintptr_t base = page_lo(s->lo);
    const uintptr_t p = (lo - base) >> PAGE_SHIFT, q = (hi - base) >> PAGE_SHIFT;
    uintptr_t run = 0;
    int open = 0;
    for (uintptr_t i = p; i <= q; i++) {
        const int change = i < q && s->armed[i] != on && !(on && s->host[i]);
        if (change) {
            if (!open) { run = i; open = 1; }
            continue;
        }
        if (open) {
            const uintptr_t run_lo = base + (run << PAGE_SHIFT), run_hi = base + (i << PAGE_SHIFT);
            track_result(run_lo, run_hi, on, nvn_direct_track_pages(nvn_direct_guest_of(run_lo), run_hi - run_lo, on));
            open = 0;
        }
    }
}

// Change the protection only on the first span over a page; every span over it gets the result.
static void arm_shared(int first, uintptr_t lo, uintptr_t hi, int on) {
    uintptr_t done = lo;
    for (int i = first; i < g_nspans && g_spans[i]->lo < hi; i++) {
        span_t* s = g_spans[i];
        if (s->hi <= lo) continue;
        const uintptr_t base = page_lo(s->lo), end = page_hi(s->hi);
        uintptr_t b = base > lo ? base : lo;
        if (b < done) b = done;
        const uintptr_t e = end < hi ? end : hi;
        if (b < e) {
            arm_pages(s, b, e, on);
            done = e;
        }
    }
}

void nvn_dirty_remove_range(const void* pool) {
    pthread_mutex_lock(&g_mu);
    pthread_rwlock_wrlock(&g_rw);
    span_t* s = NULL;
    for (int i = 0; i < g_nspans; i++) {
        if (g_spans[i]->pool != pool) continue;
        s = g_spans[i];
        memmove(&g_spans[i], &g_spans[i + 1], (size_t)(g_nspans - i - 1) * sizeof *g_spans);
        g_nspans--;
        update_prefix(i);
        atomic_store(&s->live, 0);
        break;
    }
    pthread_rwlock_unlock(&g_rw);
    if (s) {
        const uintptr_t lo = page_lo(s->lo), hi = page_hi(s->hi);
        uintptr_t b = lo;
        for (int i = first_after(lo); i < g_nspans && g_spans[i]->lo < hi && b < hi; i++) {
            const span_t* other = g_spans[i];
            if (other->hi <= b) continue;
            const uintptr_t ob = page_lo(other->lo);
            if (b < ob) arm_pages(s, b, ob, 0);
            b = page_hi(other->hi);
        }
        if (b < hi) arm_pages(s, b, hi, 0);
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
    pthread_mutex_lock(&g_mu);
    pthread_rwlock_wrlock(&g_rw);
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
    g_spans = g_gone = NULL;
    g_nspans = g_ngone = g_capspans = g_capgone = 0;
    pthread_rwlock_unlock(&g_rw);
    pthread_mutex_unlock(&g_mu);
}

void nvn_direct_host_written(const void* mem, size_t size) {
    if (!size) return;
    const uintptr_t lo = page_lo((uintptr_t)mem), hi = page_hi((uintptr_t)mem + size);
    pthread_mutex_lock(&g_mu);
    const int first = first_after(lo);
    for (int i = first; i < g_nspans && g_spans[i]->lo < hi; i++) {
        span_t* s = g_spans[i];
        if (s->hi <= lo) continue;
        const uintptr_t base = page_lo(s->lo), end = page_hi(s->hi);
        const uintptr_t b = base > lo ? base : lo, e = end < hi ? end : hi;
        memset(s->host + ((b - base) >> PAGE_SHIFT), 1, (e - b) >> PAGE_SHIFT);
    }
    arm_shared(first, lo, hi, 0);
    pthread_mutex_unlock(&g_mu);
}

int nvn_dirty_tracked(const void* addr, size_t len) { return len && find((uintptr_t)addr, len) != NULL; }
const void* nvn_dirty_span(const void* addr, size_t len) { return len ? find((uintptr_t)addr, len) : NULL; }
uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }

void nvn_dirty_protect(const void* addr, size_t len) {
    if (!len) return;
    const uintptr_t a = (uintptr_t)addr, lo = page_lo(a), hi = page_hi(a + len);
    pthread_mutex_lock(&g_mu);
    if (find_locked(a, len)) arm_shared(first_after(lo), lo, hi, 1);
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

// A CPU write gives every span covering a touched page the same generation, then disarms the page.
static void written(uintptr_t a, size_t len) {
    if (!len) return;
    const uintptr_t lo = page_lo(a), hi = page_hi(a + len);
    pthread_mutex_lock(&g_mu);
    const int first = first_after(lo);
    uint64_t g = 0;
    uintptr_t logged = lo;
    for (int i = first; i < g_nspans && g_spans[i]->lo < hi; i++) {
        span_t* s = g_spans[i];
        if (s->hi <= lo) continue;
        if (!g) g = atomic_load(&g_now) + 1;
        const uintptr_t base = page_lo(s->lo), end = page_hi(s->hi);
        const uintptr_t b = base > lo ? base : lo, e = end < hi ? end : hi;
        const uintptr_t p = (b - base) >> PAGE_SHIFT, q = (e - base) >> PAGE_SHIFT;
        for (uintptr_t j = p; j < q; j++) atomic_store(&s->gen[j], g);
        if (e > logged) {
            wlog_add(b > logged ? b : logged, e);
            logged = e;
        }
    }
    if (g) {
        arm_shared(first, lo, hi, 0);
        atomic_store(&g_now, g);
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
    const uintptr_t base = page_lo(s->lo);
    const uintptr_t p = (a - base) >> PAGE_SHIFT, q = (a + len - 1 - base) >> PAGE_SHIFT;
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
    const uintptr_t base = page_lo(s->lo);
    const uintptr_t p = (a - base) >> PAGE_SHIFT, q = (a + len - 1 - base) >> PAGE_SHIFT;
    for (uintptr_t i = p; i <= q; i++) {
        if (atomic_load(&s->gen[i]) > stamp) { if (f == UINTPTR_MAX) f = i; l = i; }
    }
    if (f == UINTPTR_MAX) return 0;
    const uintptr_t b0 = base + (f << PAGE_SHIFT), b1 = base + ((l + 1) << PAGE_SHIFT);
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
