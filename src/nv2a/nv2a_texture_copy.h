#ifndef NV2A_TEXTURE_COPY_H
#define NV2A_TEXTURE_COPY_H
#include <stddef.h>
#include <stdint.h>

/* Deliberately bounded CPU fragment path: PROJECT2D, linear RGB565,
 * clamp-to-edge, one mip, texture RGB / interpolated diffuse alpha.
 * Preparation rejects other combiner and depth/blend states. */
typedef struct {
    uint32_t texture_handle, texture_offset, width, height, pitch, linear, dither;
    uint32_t target_handle, target_offset, target_pitch, target_bpp;
    uint32_t clip_x, clip_y, clip_w, clip_h;
} NV2ATextureCopy;

const char *nv2a_texture_copy_prepare(const uint32_t methods[2048], NV2ATextureCopy *state);
int nv2a_dma_resolve(const uint8_t *ramin, size_t size, uint32_t ramht,
                     uint32_t handle, uint32_t *base, uint32_t *limit);
/* Buffers start at the resolved surface offsets. Sizes include row padding.
 * Returns zero for invalid geometry/bounds; no guest memory is accessed here. */
int nv2a_texture_copy_triangle(const NV2ATextureCopy *state,
    const uint8_t *texture, size_t texture_size, uint8_t *target, size_t target_size,
    const float a[16][4], const float b[16][4], const float c[16][4]);
#endif
