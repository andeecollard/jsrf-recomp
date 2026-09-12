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
/* Complete queued draws and copy retained render surfaces to guest RAM. */
int nv2a_d3d11_sync(void);
/* Synchronize only surfaces overlapping a guest-memory range. */
int nv2a_d3d11_sync_range(uint8_t *target, size_t bytes);
/* Synchronize, then require the next draw to upload CPU-modified pixels. */
void nv2a_d3d11_invalidate(uint8_t *target);
void nv2a_d3d11_invalidate_range(uint8_t *target, size_t bytes);
/* Execute a full, all-RGB clear on every cached colour image for this guest
 * target. Returns nonzero only when guest RAM may remain stale because the GPU
 * now owns the cleared contents. Partial-channel clears stay on the CPU. */
int nv2a_d3d11_clear_color(uint8_t *target, size_t target_size,
    uint32_t pitch, uint32_t width, uint32_t height,
    uint32_t components, uint32_t value);
/* Reserved for a future verified resident Z24S8 clear. Currently returns zero
 * so the caller retains the byte-exact CPU fallback. */
int nv2a_d3d11_clear_depth_stencil(uint8_t *target, size_t target_size,
    uint32_t pitch, uint32_t width, uint32_t height,
    uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
    uint32_t components, uint32_t value);
/* Stable diagnostic label for the most recent -1 result. */
const char *nv2a_d3d11_last_reject(void);
/* Compact cumulative counters for live performance validation. */
void nv2a_d3d11_report(void);
/* What each retained surface holds right now, for the flip trace. */
void nv2a_d3d11_surface_report(void);
/* RECOMP_D3D11_EVENTS=<n>: true for the first n clear/draw/flush/flip events,
 * so the ORDER of them can be read rather than inferred from counters. */
int nv2a_d3d11_event_trace(void);
uint32_t nv2a_d3d11_guest_offset(const void *host);
#endif
