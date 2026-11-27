// From Banana Native (github.com/hugorosario/BananaNative, Android port of github.com/seedds/nxnative by seedds), MIT: see LICENSE in src/video_core/nvn_direct/bananative.
// Lichii: host-lent command memory segments, out of hle_nvn.c (its commands take them, AddCommandMemory reclaims and adopts them) so tests build them on their own
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "nvn_obj.h"

size_t nvn_hostseg_size(const cmd_t *o, int empty_pool, size_t want);
uint8_t *nvn_hostseg_take(const cmd_t *o, size_t n);
void nvn_hostseg_release(uintptr_t lo, uintptr_t hi);
void nvn_hostseg_adopt(const cmd_t *o, uintptr_t lo, uintptr_t hi);
int nvn_hostseg_busy(const void *mem);   // 1: the segment at mem is lent, 0: free to be lent again, -1: no segment starts there
void nvn_hostseg_destroy(void);          // the teardown (nvn_hle_destroy): every segment freed
