#ifndef NV2A_METAL_H
#define NV2A_METAL_H
#include "nv2a_texture_copy.h"
/* Returns triangle count, or -1 without changing the target on unsupported
 * state/device failure. Called serially by the existing draw worker. */
int nv2a_metal_draw(const NV2ATextureCopy *state, const uint8_t *texture,
    size_t texture_size, uint8_t *target, size_t target_size,
    uint8_t *depth, size_t depth_size,
    const float (*vertices)[16][4], unsigned count, unsigned primitive);
/* Complete queued draws and copy the retained render surface to guest RAM. */
int nv2a_metal_sync(void);
/* Synchronize, then require the next draw to upload CPU-modified pixels. */
void nv2a_metal_invalidate(uint8_t *target);
/* Drop the retained surface without reading it back. Only valid when the
 * caller is about to overwrite every byte of it -- see the definition. */
void nv2a_metal_discard(void);
/* Stable diagnostic label for the most recent -1 result. */
const char *nv2a_metal_last_reject(void);
/* Compact cumulative counters for live performance validation. */
void nv2a_metal_report(void);
/* Command-buffer accounting (RECOMP_METAL_CB_STATS=1). The frame count is
 * bumped by the pushbuffer executor at the guest's own frame boundary,
 * NV097_FLIP_STALL, because this file has no notion of a frame. */
void nv2a_metal_cb_report(void);
/* Capture one frame's draws and replay them through both submission paths.
 * Called at the guest's own frame boundary. See RECOMP_METAL_FRAME_BENCH. */
void nv2a_metal_frame_bench_flip(void);
/* Ring staging self-test. pin_mode: 0 none (positive control), 1 per command
 * buffer, 2 once per batch from a slab bitmask. Returns 0 if it could not run;
 * *corrupt_out is the number of reservations the GPU read after they had been
 * overwritten. See the comment on nv2a_metal_ring_selftest. */
int nv2a_metal_ring_selftest(unsigned slabs, int pin_mode, unsigned iters,
                             unsigned per_batch, unsigned *corrupt_out);
extern unsigned long long g_mtl_frames;
#endif
