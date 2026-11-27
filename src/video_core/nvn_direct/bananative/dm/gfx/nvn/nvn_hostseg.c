// From Banana Native (github.com/hugorosario/BananaNative, Android port of github.com/seedds/nxnative by seedds), MIT: see LICENSE in src/video_core/nvn_direct/bananative.
// Lichii: out of hle_nvn.c (nvn_hostseg.h)
#include "nvn_hostseg.h"
#include "../../host/dm.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

uint64_t dm_nvn_frame_count(void);
// Host-supplied segment when the game's own command memory runs out: when the game's out-of-memory callback cannot allocate from its command memory pool, it calls AddCommandMemory with an empty pool (p = NULL, n = 0)
// (opening the advanced stats page on the level 88 sorceress save: many list items in one frame, and the original implementation segfaulted here reading NULL+8). Our records are larger than real-device NVN commands, so the budget the game reserved for real-device usage is not necessarily enough,
// so this is not an error state of the game but something we must catch: provide a segment of host memory to keep recording. The segment is replayed in place at submit and reclaimed after being kept for 8 frames (submit is in the same frame as recording or the next one).
// (Do not use the whole game chunk as the range: SMBW sub-allocates many recordings out of one big chunk, and a later grant inside that chunk freed segments still in use.)
// Reclaim follows the game's command memory chunks, not a frame count: the game caches recorded command handles and CALLs them every frame until it re-records them itself (SMBW's object setup handles);
// once a handle was recorded into a host segment, the old "reclaim 8 frames after lending" rewrote the segment while the game still CALLed it (with overflow forced on the Mac, the replay read garbage commands: RGBA16F used as a depth format for a pipeline).
// A segment hangs off one recording through the JUMP at the end of its guest part: that recording occupies [BEGIN, end of JUMP) in guest memory, and the game only gives those bytes out again once they are dead
// (when it re-records that material in place; packing the next recording right behind by used never touches them, since used includes the JUMP). So a segment remembers that guest range and is reclaimed when the game gives out memory
// overlapping it; segments overflowing from a host segment keep the same range.
// The segment pool grows on demand (SMBW opening course: many small command buffers each overflow once per frame, and the old fixed 64 segments × 1 MB ran out within 8 frames); abort only beyond HOSTSEG_CAP in total.
// A segment is HOSTSEG_SZ, with two exceptions (nvn_hostseg_size): an empty pool with a length of at most HOSTSEG_SMALL outside a recording gets that much rounded up to HOSTSEG_SMALL
// (Super Mario Odyssey hands out many such 512-byte grants before recording; at 256 KB each, and with no recording to reclaim them by, they reached the cap entering the Wooded
// Kingdom), and a record larger than HOSTSEG_SZ gets a segment that holds it. A free segment is taken again only for a request of its own size; the cap counts every segment's bytes.
// An empty-pool grant outside a recording means the game's own allocator for command memory came back empty. On hardware a null pool cannot be written to, so a game only gets here
// when the host's records outgrew what it measured for them (Odyssey's "DisplayList" heap: docs/reference/memory.md, "Model heaps sized by the host's command record sizes").
// Those segments have no guest range to be reclaimed by, so they are not reclaimed; the cap's abort names the state, and the fix belongs in the record sizes
#define HOSTSEG_SZ (256u << 10)
#define HOSTSEG_SMALL (4u << 10)
#define HOSTSEG_CAP (1u << 30)
typedef struct { uint8_t *mem; size_t n; uint64_t frame; int busy; uintptr_t org_lo, org_hi; } hostseg_t;   // org: the guest range of the recording it hangs off (0 = unknown, never reclaimed)
static hostseg_t *g_hostseg; static int g_nhostseg, g_caphostseg; static uint64_t g_hostseg_bytes;
static pthread_mutex_t g_hostseg_mu = PTHREAD_MUTEX_INITIALIZER;
size_t nvn_hostseg_size(const cmd_t *o, int empty_pool, size_t want) {
    if (empty_pool) return want && want <= HOSTSEG_SMALL && !o->recording ? HOSTSEG_SMALL : HOSTSEG_SZ;
    return want + 16 > HOSTSEG_SZ ? (want + 16 + 0xfff) & ~(size_t)0xfff : HOSTSEG_SZ;   // 16: room for the next JUMP (want holds OP_RESERVE already, this is slack for the alignment)
}
// Take a segment of n bytes: origin = the current recording's guest bytes [BEGIN, end of JUMP); if the write pointer is already in a host segment (overflowing again), keep that segment's origin
uint8_t *nvn_hostseg_take(const cmd_t *o, size_t n) {
    static unsigned total; uint64_t f = dm_nvn_frame_count(); uint8_t *m = NULL;
    uintptr_t lo = 0, hi = 0, cur = o ? (uintptr_t)o->cur : 0;
    pthread_mutex_lock(&g_hostseg_mu);
    // Lichii: only a recording that has a record (state 1) has guest bytes or a lent segment to hang off: one begun with no command yet (state 2) writes no JUMP where
    // it was (cmd_new_segment), so its segment's range stays unknown until its callback's memory gives it one (nvn_hostseg_adopt). Built from the write pointer, it
    // took in the bytes after the last recording's END and the grant next to them, which the callback then gave out: the segment was reclaimed while its handle was live
    int in_seg = o && o->recording != 1;   // (state 2 or none: no range)
    for (int i = 0; i < g_nhostseg && cur && !in_seg; i++) if (cur >= (uintptr_t)g_hostseg[i].mem && o->end == g_hostseg[i].mem + g_hostseg[i].n) { lo = g_hostseg[i].org_lo; hi = g_hostseg[i].org_hi; in_seg = 1; break; }
    // (Lichii: the recording's bytes in this guest segment only: from its first record when that is in it, else from the segment's start, where it went on from a
    // segment before it. A recording that began in a lent segment had its begin there, a host address: the range from it to cur took in every guest address below,
    // and the next AddCommandMemory reclaimed the segment while its handle was live: Orbitals' replay then walked a reused segment and crashed)
    if (!in_seg && o && o->cur) {
        const uintptr_t seg = o->seg && (uintptr_t)o->seg <= cur ? (uintptr_t)o->seg : cur;
        lo = o->begin && (uintptr_t)o->begin >= seg && (uintptr_t)o->begin <= cur ? (uintptr_t)o->begin : seg; hi = cur + 16;
    }
    for (int i = 0; i < g_nhostseg && !m; i++)
        if (!g_hostseg[i].busy && g_hostseg[i].n == n) { g_hostseg[i] = (hostseg_t){ g_hostseg[i].mem, n, f, 1, lo, hi }; m = g_hostseg[i].mem; }
    if (!m && g_hostseg_bytes + n <= HOSTSEG_CAP && (m = malloc(n))) {
        if (g_nhostseg == g_caphostseg) { g_caphostseg = g_caphostseg ? g_caphostseg * 2 : 64; g_hostseg = realloc(g_hostseg, sizeof *g_hostseg * (size_t)g_caphostseg); }
        g_hostseg[g_nhostseg++] = (hostseg_t){ m, n, f, 1, lo, hi }; g_hostseg_bytes += n;
    }
    unsigned t = ++total; int ns = g_nhostseg; uint64_t bytes = g_hostseg_bytes;
    pthread_mutex_unlock(&g_hostseg_mu);
    if (!m) die("NVN command memory: the game's pool is empty and host-lent segments reached %u MB without being reclaimed by the game's blocks", HOSTSEG_CAP >> 20);
    // Lichii: logged when the pool grows to twice the segments of the last line, not every 1024 lends: a game whose recordings each start in a lent segment (the callback adds
    // their memory after the call that needed it: Orbitals) lends hundreds a second from a pool that stays the same
    static _Atomic int logged_ns; int lns = atomic_load(&logged_ns);
    if (ns >= 2 * lns && atomic_compare_exchange_strong(&logged_ns, &lns, ns))   // (the first lend too: 0 logged)
        LOG("[nvn] the game's command memory ran short, the host lends a segment of %zu KB (%u times so far, frame %llu, segment pool %d segments, %llu KB)", n >> 10, t, (unsigned long long)f, ns, (unsigned long long)(bytes >> 10));
    return m;
}
// The game gives out [lo, hi): segments whose recording's guest bytes overlap it are dead, reclaim them
void nvn_hostseg_release(uintptr_t lo, uintptr_t hi) {
    pthread_mutex_lock(&g_hostseg_mu);
    for (int i = 0; i < g_nhostseg; i++) if (g_hostseg[i].busy && g_hostseg[i].org_hi > lo && g_hostseg[i].org_lo < hi) g_hostseg[i].busy = 0;
    pthread_mutex_unlock(&g_hostseg_mu);
}
// Lichii: the recording in progress goes on in [lo, hi) of the game's memory, which its out-of-memory callback just added (nvn_direct_guest_call_owe): a segment it was lent before
// any of its records were in the game's memory (the callback runs after the call that needed it) knew no guest range, so it is reclaimed with this one
void nvn_hostseg_adopt(const cmd_t *o, uintptr_t lo, uintptr_t hi) {
    pthread_mutex_lock(&g_hostseg_mu);
    for (int i = 0; i < g_nhostseg && o->cur; i++)
        if (o->cur >= g_hostseg[i].mem && o->end == g_hostseg[i].mem + g_hostseg[i].n) { if (!g_hostseg[i].org_lo && !g_hostseg[i].org_hi) { g_hostseg[i].org_lo = lo; g_hostseg[i].org_hi = hi; } break; }
    pthread_mutex_unlock(&g_hostseg_mu);
}
int nvn_hostseg_busy(const void *mem) {
    int r = -1; pthread_mutex_lock(&g_hostseg_mu);
    for (int i = 0; i < g_nhostseg; i++) if (g_hostseg[i].mem == mem) { r = g_hostseg[i].busy; break; }
    pthread_mutex_unlock(&g_hostseg_mu); return r;
}
void nvn_hostseg_destroy(void) {
    for (int i = 0; i < g_nhostseg; i++) free(g_hostseg[i].mem);
    free(g_hostseg); g_hostseg = NULL; g_nhostseg = g_caphostseg = 0; g_hostseg_bytes = 0;
}
