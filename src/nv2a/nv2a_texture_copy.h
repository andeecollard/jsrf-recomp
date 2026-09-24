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
    /* SZ_X8R8G8B8 (0x07) is SZ_A8R8G8B8 with the top byte undefined, so it
     * shares the rgba8 path and only forces alpha opaque. SZ_X1R5G5B5 (0x03)
     * is swizzled 16-bit, which is neither the linear image-rectangle path
     * nor any of the block formats. Both were refused outright until
     * 21 Sep 2026; see the [TEXFMT] census.
     *
     * SZ_A4R4G4B4 (0x04) is the last member of the set the shipped assets can
     * produce, and it is swizzled 16-bit too -- so it rides sz16 for layout
     * and needs argb4 only to pick its unpack. It is the one 16-bit format
     * here that carries REAL alpha, which is why it cannot simply reuse the
     * 555 path. */
    uint32_t xrgb8, sz16, argb4;
    /* Pitch-linear 32-bit image rectangles: 1 = LU_IMAGE_A8R8G8B8 (0x12),
     * 2 = LU_IMAGE_X8R8G8B8 (0x1E, alpha forced opaque). Linear like 0x11 --
     * width/height from the image-rectangle register, coordinates in texels,
     * one level -- but BGRA at four bytes a texel. Refused until 24 Sep 2026;
     * the graffiti editor's canvas is one. */
    uint32_t lin32;
    float lod_bias;
    uint32_t combiner_count, color_icw[8], alpha_icw[8];
    uint32_t color_ocw[8], alpha_ocw[8], add_specular;
    /* Per-stage constants, NV097_SET_COMBINER_FACTOR0/1 (0x0A60/0x0A80).
     * D3DCOLOR, i.e. A8R8G8B8. They were refused outright until 21 Sep 2026;
     * the graffiti shader programs them to the pure channel masks 0x0000FF00,
     * 0x00FF0000 and 0x000000FF, which with a dot product is channel
     * extraction -- so they are not incidental, they ARE the effect. */
    uint32_t const0[8], const1[8];
    uint32_t texture_mask;
    uint32_t bump_approx;   /* a unit's BUMPENVMAP mode was drawn as plain 2D (RECOMP_TEXMODE_APPROX) */
    /* BUMPENVMAP, IMPLEMENTED (RECOMP_TEXMODE_BUMP, default on). Per unit,
     * indexed by the unit itself (bump[0] is always 0 -- the mode is not
     * legal on stage 0). xemu pgraph/glsl/psh.c is the reference:
     *
     *   (du,dv) = (sign3(Tin.b), sign3(Tin.g))   Tin = bump_input[u]'s texel
     *   (du',dv') = (M00 du + M10 dv, M01 du + M11 dv)
     *   T[u] = texture_u(coord_u.xy + (du',dv'))   -- NO projective divide
     *   mode 7 also: T[u] *= bump_scale * Tin.r + bump_offset
     *
     * sign3(x) reads the channel as a two's-complement byte over 127. The
     * matrix is in D3D's sense (D3DTSS_BUMPENVMATij); the method words at
     * NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 64*u arrive as M00, M01, M11, M10
     * -- the XDK's own D3DTSS numbering (22, 23, 24, 25) and xemu's register
     * swizzle agree -- and are put back in order at the gate. The offset is
     * added in the unit's own coordinate space, so for the image-rectangle
     * formats it is in texels, as xemu's normalize-after-add does. */
    uint32_t bump[4];         /* 0, 6 BUMPENVMAP or 7 BUMPENVMAP_LUMINANCE */
    uint32_t bump_input[4];   /* the unit whose texel supplies du, dv (and l) */
    float bump_mat[4][4];     /* M00, M01, M10, M11 */
    float bump_scale[4], bump_offset[4];
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
    /* NV097_SET_WINDOW_CLIP_HORIZONTAL/VERTICAL, inclusive pixel bounds,
     * already intersected with the surface clip above. The rasteriser --
     * Metal's scissor, or the software loop's bounds -- draws inside it and
     * nowhere else. Until 21 Sep 2026 any window clip smaller than the
     * surface refused the whole draw ("partial window clip"), and the
     * character-select screen draws its character view through exactly
     * such a rectangle: 495,110 draws refused in one 90 s session, two
     * black panels on screen. */
    uint32_t wc_x0, wc_y0, wc_x1, wc_y1;   /* all zero: no window clip (see
                                            * nv2a_texture_copy_window) */
    /* The guest's own depth range and what it wants done outside it.
     * NV097_SET_CLIP_MIN/MAX (0x394/0x398) are IEEE floats in the same
     * 0..16777215 units as oPos.z; ZMIN_MAX_CONTROL (0x1D78) selects
     * discard (CULL) or saturate (CLAMP) outside them. Measured in JSRF:
     * 0 .. 16777215 with CULL. */
    float z_clip_min, z_clip_max;
    uint32_t z_cull;
    /* THE FINAL COMBINER AND FOG (G53, Rokkaku-dai Heights, 24 Sep 2026).
     *
     * Until then the gate accepted only the two final-combiner programs
     * D3D writes with fog OFF -- CW0 0xC (D = R0) or 0xE (D = V1+R0 sum),
     * CW1 0x1C80 -- and refused every other one, which DROPPED THE DRAW: a
     * fogged level (Rokkaku) lost its buildings, streets and characters to
     * 926,734 refusals, the sky and the HUD being the only unfogged draws.
     * D3D's fog program (d3d8_ff_final_combiner) is CW0 0x130C0300 or
     * 0x130E0300: A = FOG.a, B = R0 (or V1+R0), C = FOG.rgb, D = 0, i.e.
     * lerp(fog colour, colour, fog factor).
     *
     * final_general 0: the two fog-off programs, drawn exactly as before
     * (R0, plus V1 when add_specular). 1: final_cw0/1 are evaluated in full
     * by nv2a_final_combine (and its MSL twin): rgb = D + A*B + (1-A)*C,
     * alpha = G, with EF_PROD (15) = E*F and V1R0_SUM (14) = V1 + R0
     * (each optionally complemented, the sum optionally clamped -- CW1 bits
     * 5, 6 and 7), FOG (3) = (fog colour, fog factor), C0/C1 (1, 2) =
     * SPECULAR_FOG_FACTOR0/1 (0x1E20/0x1E24). The fog factor comes from the
     * interpolated fog coordinate by nv2a_fog_factor. */
    uint32_t final_general, final_cw0, final_cw1;
    uint32_t fog_enable, fog_mode, fog_color;   /* 0x02A4, 0x029C, 0x02A8 (ABGR: red is the low byte) */
    float fog_p0, fog_p1;                       /* FOG_PARAMS 0x09C0, 0x09C4 */
    uint32_t spec_fog_c0, spec_fog_c1;          /* SPECULAR_FOG_FACTOR0/1, A8R8G8B8 */
    /* G54: points. NV097_SET_POINT_SIZE (0x43C) is in 1/8 pixel (xemu:
     * glPointSize(size / 8)); SET_POINT_SMOOTH_ENABLE (0x31C) is the point
     * sprite switch, whose texture coordinates run 0..1 over the sprite;
     * SET_POINT_PARAMS_ENABLE (0x318) is distance attenuation, not modelled. */
    float point_size;
    uint32_t point_sprite, point_params;
} NV2ATextureCopy;

/* The fog factor for fog coordinate `d`, per NV097_SET_FOG_MODE (xemu
 * pgraph/glsl/vsh.c, the reference this file already names; D3D's params per
 * d3d8_ff_fog make LINEAR (end - d)/(end - start) and EXP/EXP2 e^-(d*density)
 * and e^-(d*density)^2):
 *   0x2601 LINEAR      p0 + d*p1 - 1           (d non-finite: 0)
 *   0x800  EXP         p0 + 2^(16*d*p1) - 1.5  (d non-finite: 0)
 *   0x801  EXP2        p0 + 2^(-32*d*d*p1*p1) - 1.5
 *   0x804, 0x802, 0x803  the _ABS forms: the same on |d|
 * clamped to [0,1]. Evaluated PER PIXEL from the interpolated coordinate: the
 * mode and params are rasteriser registers (NV_PGRAPH_CONTROL_3, FOGPARAM0/1)
 * and only the coordinate comes from the vertex unit. xemu evaluates per
 * vertex; the two agree exactly for LINEAR and differ inside large triangles
 * for EXP/EXP2 ([FOG] counts which modes a run uses). The _ABS forms take |d|
 * here; xemu takes |factor|, which no reading of the name supports. Unknown
 * modes: 1 (no fog). */
float nv2a_fog_factor(uint32_t mode, float p0, float p1, float d);
/* NULL if the final combiner program (CW0, CW1) is one nv2a_final_combine
 * models, else the reason it is not. */
const char *nv2a_final_combiner_supported(uint32_t cw0, uint32_t cw1);
/* The final combiner on a register file whose 0..13 the stages left (R0 in
 * 12, V0/V1 in 4/5, T0..T3 in 8..11). Writes 1, 2, 3, 14 and 15 itself.
 * `fog` is the fog factor; out is clamped RGBA. The reference the Metal
 * shader's final_comb() is tested against. */
void nv2a_final_combine(const NV2ATextureCopy *s, float regs[16][4], float fog, float out[4]);
/* Counts per reason, printed with the combiner census: every refused final
 * combiner is a DROPPED draw, and says so. */
void nv2a_fog_census(void);

/* The rectangle the rasteriser may touch: the window clip when the state
 * carries one, the surface clip when it does not. A state built by hand --
 * the unit tests, a replay -- leaves the four fields zero, and a 1x1 clip at
 * the origin is not something this title asks for, so all-zero means "the
 * whole surface" rather than "one pixel". Inclusive bounds. */
static inline void nv2a_texture_copy_window(const NV2ATextureCopy *s,
                                            uint32_t *x0, uint32_t *y0,
                                            uint32_t *x1, uint32_t *y1)
{
    if (!s->wc_x0 && !s->wc_y0 && !s->wc_x1 && !s->wc_y1) {
        *x0 = s->clip_x; *y0 = s->clip_y;
        *x1 = s->clip_x + s->clip_w - 1; *y1 = s->clip_y + s->clip_h - 1;
    } else { *x0 = s->wc_x0; *y0 = s->wc_y0; *x1 = s->wc_x1; *y1 = s->wc_y1; }
}

const char *nv2a_texture_copy_prepare_image(const uint32_t methods[2048], unsigned unit, NV2ATextureCopy *state);

size_t nv2a_texture_copy_texture_bytes(const NV2ATextureCopy *state);

/* Unpack one mip level into tightly packed RGBA8 for a hardware sampler.
 * Returns zero, writing nothing, if the level is absent or either buffer is
 * too small. out_size must hold level_width * level_height * 4 bytes. */
int nv2a_texture_copy_decode_level(const NV2ATextureCopy *state,
    const uint8_t *data, size_t size, unsigned level,
    uint8_t *out, size_t out_size, unsigned *out_w, unsigned *out_h);

const char *nv2a_texture_copy_prepare(const uint32_t methods[2048], NV2ATextureCopy *state);

/* Which texture formats the gate above refused, and how often. The reason
 * string names the gate, not the input, so a run cannot otherwise say whether
 * the missing pixels are one unsupported format or twenty. */
void nv2a_texture_copy_census(void);
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
