#ifndef NV2A_TEXTURE_COPY_H
#define NV2A_TEXTURE_COPY_H
#include <stddef.h>
#include <stdint.h>

/* Bounded CPU fragment path: PROJECT2D, linear RGB565, DXT1 or DXT3,
 * copy or measured texture-times-diffuse combiner. Only the measured alpha,
 * source-alpha blend and fixed Z24 LEQUAL state is supported. RGB565 dithering
 * uses ordered quantisation, not yet verified against NV2A hardware. */
typedef struct NV2ATextureCopy {
    uint32_t texture_handle, texture_offset, width, height, pitch, linear, dither;
    uint32_t modulate;
    uint32_t rgba8, levels, min_filter;
    float lod_bias;
    uint32_t combiner_count, color_icw[8], alpha_icw[8];
    uint32_t color_ocw[8], alpha_ocw[8], add_specular;
    uint32_t texture_mask;
    const struct NV2ATextureCopy *extra_stages;
    const uint8_t *extra_texture[3];
    size_t extra_size[3];
    /* No texture bound: the fragment is the diffuse colour alone. */
    uint32_t untextured;
    uint32_t dxt1, dxt3, repeat, alpha_test, alpha_ref, blend, blend_src, blend_dst;
    uint32_t cull_face, front_cw;
    uint32_t depth_test, depth_write, depth_func, depth_handle, depth_offset, depth_pitch;
    uint32_t stencil_test, stencil_write, stencil_mask, stencil_ref, stencil_func_mask;
    uint32_t stencil_func, stencil_fail, stencil_zfail, stencil_zpass;
    uint32_t target_handle, target_offset, target_pitch, target_bpp;
    uint32_t clip_x, clip_y, clip_w, clip_h;
    /* The guest's own depth range and what it wants done outside it.
     * NV097_SET_CLIP_MIN/MAX (0x394/0x398) are IEEE floats in the same
     * 0..16777215 units as oPos.z; ZMIN_MAX_CONTROL (0x1D78) selects
     * discard (CULL) or saturate (CLAMP) outside them. Measured in JSRF:
     * 0 .. 16777215 with CULL. */
    float z_clip_min, z_clip_max;
    uint32_t z_cull;
} NV2ATextureCopy;

const char *nv2a_texture_copy_prepare_image(const uint32_t methods[2048], unsigned unit, NV2ATextureCopy *state);

size_t nv2a_texture_copy_texture_bytes(const NV2ATextureCopy *state);

/* Unpack one mip level into tightly packed RGBA8 for a hardware sampler.
 * Returns zero, writing nothing, if the level is absent or either buffer is
 * too small. out_size must hold level_width * level_height * 4 bytes. */
int nv2a_texture_copy_decode_level(const NV2ATextureCopy *state,
    const uint8_t *data, size_t size, unsigned level,
    uint8_t *out, size_t out_size, unsigned *out_w, unsigned *out_h);

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
/* True for the NV2A blend factors every sink implements. The accept test
 * in prepare_texture_copy consults this, so a factor cannot be accepted
 * that a sink would then silently substitute for. */
int nv2a_texture_copy_blend_factor_supported(uint32_t factor);

/* Screen-space winding is inverted for a triangle with an ODD number of
 * vertices behind the camera. The guest perspective-divided these positions
 * itself, and dividing by a negative w negates x and y, so the sign of the
 * screen area is the opposite of the true orientation. Pass the three w
 * values; returns non-zero when the facing test must be flipped. */
int nv2a_texture_copy_winding_flipped(float wa, float wb, float wc);

/* Facing and culling, in ONE place, for the same reason blend_factor_supported
 * is in one place: the three sinks must not drift apart.
 *
 * Both expressions were written out identically in nv2a_texture_copy.c, in
 * nv2a_d3d11.c and in nv2a_metal.m. Straddle culling is an open question here
 * -- inverted winding is known to discard half of the triangles crossing the
 * camera plane, and the obvious one-line sign fix is recorded as WRONG -- so
 * whoever eventually gets it right would have had to find all three copies and
 * change them the same way. Two out of three is a bug that renders correctly
 * on one host.
 *
 * Header-inline rather than a call, because this is per-triangle and the
 * software rasteriser runs it millions of times a frame. */
static inline int nv2a_texture_copy_front_facing(
        const NV2ATextureCopy *s, float area, float wa, float wb, float wc)
{
    return ((area > 0) ^ nv2a_texture_copy_winding_flipped(wa, wb, wc))
           == (s->front_cw != 0);
}

/* NV097_SET_CULL_FACE: 0x408 culls everything, 0x404 front, 0x405 back. */
static inline int nv2a_texture_copy_culled(const NV2ATextureCopy *s, int front)
{
    return s->cull_face == 0x408
        || (s->cull_face == 0x404 && front)
        || (s->cull_face == 0x405 && !front);
}

#endif
