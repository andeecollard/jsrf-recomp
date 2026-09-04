#ifndef NV2A_TEXTURE_COPY_H
#define NV2A_TEXTURE_COPY_H
#include <stddef.h>
#include <stdint.h>

/* Bounded CPU fragment path: PROJECT2D, one-level linear RGB565 or DXT1,
 * copy or measured texture-times-diffuse combiner. Only the measured alpha,
 * source-alpha blend and fixed Z24 LEQUAL state is supported. RGB565 dithering
 * uses ordered quantisation, not yet verified against NV2A hardware. */
typedef struct {
    uint32_t texture_handle, texture_offset, width, height, pitch, linear, dither;
    uint32_t modulate;
    /* No texture bound: the fragment is the diffuse colour alone. */
    uint32_t untextured;
    uint32_t dxt1, repeat, alpha_test, alpha_ref, blend, cull_face, front_cw;
    uint32_t depth_test, depth_write, depth_handle, depth_offset, depth_pitch;
    uint32_t target_handle, target_offset, target_pitch, target_bpp;
    uint32_t clip_x, clip_y, clip_w, clip_h;
} NV2ATextureCopy;

size_t nv2a_texture_copy_texture_bytes(const NV2ATextureCopy *state);

const char *nv2a_texture_copy_prepare(const uint32_t methods[2048], NV2ATextureCopy *state);
int nv2a_dma_resolve(const uint8_t *ramin, size_t size, uint32_t ramht,
                     uint32_t handle, uint32_t *base, uint32_t *limit);
/* Buffers start at the resolved surface offsets. Sizes include row padding.
 * Returns zero for invalid geometry/bounds; no guest memory is accessed here. */
int nv2a_texture_copy_triangle(const NV2ATextureCopy *state,
    const uint8_t *texture, size_t texture_size, uint8_t *target, size_t target_size,
    const float a[16][4], const float b[16][4], const float c[16][4]);
int nv2a_texture_copy_triangle_depth(const NV2ATextureCopy *state,
    const uint8_t *texture, size_t texture_size, uint8_t *target, size_t target_size,
    uint8_t *depth, size_t depth_size,
    const float a[16][4], const float b[16][4], const float c[16][4]);
#endif
