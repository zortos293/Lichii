// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// What the front end (bananative/dm/gfx/nvn/hle_nvn.c) and Lichii's own NVN functions
// (nvn_window.c) take from the dispatch (nvn_dispatch.c). C only.

#pragma once

#include <stddef.h>
#include <stdint.h>

/// The guest's return address of the NVN call this thread runs (its LR). Upstream records the
/// host's __builtin_return_address(0) as the game's call site.
extern _Thread_local uint64_t nvn_direct_guest_lr;
static inline uint64_t nvn_caller(void) { return nvn_direct_guest_lr; }

/// The game's address of a host pointer into the fastmem arena (anything else: unchanged). The
/// replay keys its pool memory by the game's addresses, so GPU addresses the game wrote into its
/// own data (shader-loaded pointers, c0's storage table) find it as the front end's do.
uint64_t nvn_direct_guest_of(uint64_t host);
/// The host pointer of a game address (anything at or past the guest address space: unchanged).
uint64_t nvn_direct_host_of(uint64_t guest);
/// nvn_direct_memory's map_view, unmap_view and track_pages (nvn_direct.h).
void* nvn_direct_map_view(uint64_t address, uint64_t size, uint64_t* unmapped);
void nvn_direct_unmap_view(void* view, uint64_t size);
void nvn_direct_track_pages(uint64_t address, uint64_t size, int armed);
/// The file holding the backing memory of the guest page at address (nvn_direct_memory's
/// backing_fd) and the page's offset in it (*offset); -1 when there is no file or the guest has
/// not mapped the page.
int nvn_direct_backing_file(uint64_t address, uint64_t* offset);
/// Maps the guest memory at host (an arena pointer) at to, for size bytes (both page aligned),
/// replacing what was mapped there: the same pages, read-write, as a virtual pool's range maps
/// its physical pool's. Pages with no guest memory behind them are made inaccessible. Returns
/// those pages' bytes.
uint64_t nvn_direct_alias_pages(void* to, uint64_t host, uint64_t size);
/// Where the host writes a value into guest memory at host (an arena pointer or the game's
/// address): its backing memory, which write tracking never protects. Anything else (a host
/// segment): unchanged. The value must not cross a page.
void* nvn_direct_writable(uint64_t host);
/// Where the replay reads guest memory at host (an arena pointer): its backing memory, which a
/// guest unmap never takes away, NULL when the guest has not mapped the page (the arena's page is
/// then inaccessible). In a virtual pool's range: unchanged, NULL where nothing is mapped.
/// Anything else (a host segment): unchanged. The read must not cross a page.
const void* nvn_direct_readable(uint64_t host);
/// The front end writes [mem, mem + size) of a pool in place from now on (command memory): its
/// pages are never write-tracked (a host write to a tracked page would fault outside the JIT).
void nvn_direct_host_written(const void* mem, size_t size);
/// The presenting queue's last fence, for the present this thread makes next (nvn_window.c).
extern _Thread_local uint64_t nvn_direct_present_queue_fence;
/// The ticket of the present this thread just made (nvn_window.c: mtl_present) and its window's
/// present interval, for nvn_direct_present_take.
void nvn_direct_present_note(uint64_t ticket, uint32_t interval);
