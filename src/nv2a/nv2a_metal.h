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
/* Drop the retained surface WITHOUT reading it back, for a guest clear that is
 * about to overwrite every byte of it. Returns non-zero if it actually dropped
 * something -- the caller must invalidate normally when it returns 0, because
 * the surface being cleared was not the one being held. */
int nv2a_metal_discard(const uint8_t *color, const uint8_t *depth);
/* A full-surface clear performed by the GPU, so it costs a load action instead
 * of a drain, a whole-surface read-back and a re-upload. Both return 0 -- and
 * the caller then performs its byte-exact CPU clear -- for anything partial,
 * for the software tail (whose colour alpha carries depth), or when the
 * surface is not the retained one. See the long note at the implementation for
 * why the two halves are all-or-nothing. */
int nv2a_metal_clear_color(uint8_t *target, size_t target_size, uint32_t pitch,
                           uint32_t width, uint32_t height,
                           uint32_t param, uint32_t value);
int nv2a_metal_clear_depth_stencil(uint8_t *target, size_t target_size,
        uint32_t pitch, uint32_t width, uint32_t height,
        uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
        uint32_t components, uint32_t value);
/* Read-only: what the backend currently retains, for RECOMP_SURFACE_AUDIT.
 * owed is -1 nothing retained, 0 retained and clean, 1 retained and dirty. */
void nv2a_metal_retained(const uint8_t **color, const uint8_t **depth, int *owed);

/* Is the dithered-blend shader path active? RECOMP_METAL_SHADER_BLEND=0 off. */
int nv2a_metal_shader_blend_on(void);
/* The fragment-tail selector, exported so it can be gated without a device.
 * nv2a_metal_shader_blend_for() is pure: given a mode and a draw's blend and
 * dither flags it answers whether that draw takes fs_hw_blend, which is the
 * tail that reads colour(0) under raster_order_group(0). The invariant the fix
 * rests on is that the DEFAULT mode answers 1 for every draw. */
/* The guest's vertex program, on the GPU. RECOMP_METAL_VSH.
 *
 * nv2a_metal_vsh_ready() is asked BEFORE the executor decides whether to run
 * the CPU interpreter: it returns 1 only if this exact program has, or can be
 * given, an MTLFunction. A 0 is a clean CPU draw. The executor then passes the
 * program's INPUTS in the array it would otherwise fill with outputs.
 * nv2a_metal_vsh_clear() must be called for any draw that did not go through
 * ready(), so a stale program cannot be applied to somebody else's vertices. */
int nv2a_metal_vsh_ready(const uint32_t (*words)[4], int length,
                         uint16_t inputs_read);
void nv2a_metal_vsh_constants(const float (*c)[4]);
void nv2a_metal_vsh_clear(void);
int nv2a_metal_shader_blend_mode(void);
int nv2a_metal_shader_blend_for(int mode, int blend, int dither);
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
