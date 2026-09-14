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
/* Stable diagnostic label for the most recent -1 result. */
const char *nv2a_metal_last_reject(void);
/* Compact cumulative counters for live performance validation. */
void nv2a_metal_report(void);
/* Command-buffer accounting (RECOMP_METAL_CB_STATS=1). The frame count is
 * bumped by the pushbuffer executor at the guest's own frame boundary,
 * NV097_FLIP_STALL, because this file has no notion of a frame. */
void nv2a_metal_cb_report(void);
extern unsigned long long g_mtl_frames;
#endif
