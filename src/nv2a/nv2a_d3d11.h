#ifndef NV2A_D3D11_H
#define NV2A_D3D11_H
#include "nv2a_texture_copy.h"
/* The Windows twin of nv2a_metal.h, with the same contract, so the call site
 * in nv2a_pb_exec.c differs between the hosts only in which name it spells.
 *
 * Returns triangle count, or -1 without changing the target on unsupported
 * state/device failure. Called serially by the push-buffer executor. */
int nv2a_d3d11_draw(const NV2ATextureCopy *state, const uint8_t *texture,
    size_t texture_size, uint8_t *target, size_t target_size,
    uint8_t *depth, size_t depth_size,
    const float (*vertices)[16][4], unsigned count, unsigned primitive);
/* Complete queued draws and copy the retained render surface to guest RAM. */
int nv2a_d3d11_sync(void);
/* Synchronize, then require the next draw to upload CPU-modified pixels. */
void nv2a_d3d11_invalidate(uint8_t *target);
/* Stable diagnostic label for the most recent -1 result. */
const char *nv2a_d3d11_last_reject(void);
/* Compact cumulative counters for live performance validation. */
void nv2a_d3d11_report(void);
/* What each retained surface holds right now, for the flip trace. */
void nv2a_d3d11_surface_report(void);
#endif
