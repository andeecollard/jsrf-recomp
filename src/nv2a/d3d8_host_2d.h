#ifndef D3D8_HOST_2D_H
#define D3D8_HOST_2D_H
/* G51.1: THE FIRST HOST-DRAWN CLASS -- PRE-TRANSFORMED 2D, IN SHADOW.
 *
 * A draw whose vertex shader handle is an FVF with an XYZRHW position (the
 * HUD, text, fades and overlays; the executor sees them as transform
 * execution mode 6, D3D's pass-through program) is drawn a second time by the
 * host, from D3D state alone, with its own Metal pipeline, into a private
 * copy of the frame -- never into the executor's surfaces. The two results
 * are then compared pixel for pixel over the region the draw touched.
 *
 * WHAT THE HOST READS. Only what the D3D mirror (d3d8_mirror.c) carries in a
 * D3D8HostDrawCheck, plus guest memory at the addresses D3D named:
 *   - vertices from D3D's stream table through G41's array derivation
 *     (va_offset/va_format per NV2A slot), fetched at the draw's indices;
 *   - positions already in screen space: x, y scaled by the supersample
 *     factors D3D keeps at device +0x454/+0x458, z as given, clip w = 1/rhw;
 *   - textures from the device's m_Textures (Data/Format/Size), decoded by
 *     nv2a_texture_decode.c -- the same decoder the executor's G27 path uses;
 *   - combiners from d3d8_ff_combiners() (G43) for fixed-function draws, or
 *     the pixel shader definition words for a bound shader;
 *   - blend, alpha test, depth, stencil, dither and colour mask from the
 *     values D3D pushed through SetRenderState_Simple;
 *   - the scissor from D3D's viewport, supersample-scaled and cut to the
 *     render target, exactly as G39's viewport check derives it.
 *
 * WHAT IT IS COMPARED WITH. Each 2D draw gets a token BEFORE its commands as
 * well as the mirror's token after them. At the first, the executor's
 * surface is synchronised to guest RAM and copied: that is the destination
 * the draw starts from, and it seeds the shadow. At the second, the executor
 * has drawn it; its surface is synchronised again, and the host renders the
 * same draw over the seed. So every draw is judged alone, from the same
 * starting pixels, and an error in one cannot leak into the next. The pixel
 * comparison itself runs at the frame's FLIP_STALL.
 *
 * RECOMP_D3D8_HOST_2D=shadow arms it (and the mirror with it). `draw`, where
 * the host's output would REPLACE the executor's for this class, is reserved
 * and not implemented: it is read, named in the log, and run as shadow.
 *
 * Split in two so the part that decides WHAT to draw can be tested with no
 * device: d3d8_host_2d.c (pure C, xbox_nv2a) builds a D3D8Host2DDraw and runs
 * the shadow bookkeeping through a backend; d3d8_host_2d_metal.m (xbox_vsh,
 * Apple only) is the backend's renderer. main.c introduces the two. */
#include <stddef.h>
#include <stdint.h>
#include "d3d8_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One post-transform vertex, laid out as the Metal shader reads it. */
typedef struct {
    float p[4];        /* screen x, y (pixels, target space), z (0..1), clip w */
    float d0[4], d1[4];
    float t[4][4];
} D3D8H2DVertex;

typedef struct {
    uint32_t addr;         /* guest physical (Data & 0x03FFFFFF) */
    uint32_t d3d_format, d3d_size;
    uint32_t fmt;          /* NV2A colour format byte (NV2A_TEXFMT_*) */
    uint32_t width, height, pitch, levels;
    uint32_t linear;       /* pitch-linear image: coordinates are in texels */
    size_t   bytes;        /* whole mip chain */
    uint32_t wrap_u, wrap_v;   /* D3DTADDRESS: 1 wrap, 2 mirror, 3 clamp */
    uint32_t mag;              /* 1 point, 2 linear */
    uint32_t min_filter;       /* NV2A min field: MIN + 2*MIP, 1..6 */
    float    lod_bias;         /* quantised to 1/256 as the NV2A register holds it */
} D3D8H2DTexture;

#define D3D8H2D_MAX_VERTS (3u * 16384u)

typedef struct {
    uint32_t serial;
    uint32_t fvf, prim, count, draw_kind;
    /* Target: D3D's render target. Only 16-bit R5G6B5 is drawn. */
    uint32_t rt_addr, rt_pitch, rt_w, rt_h, rt_fmt;
    /* Scissor, inclusive; bbox = what the triangles can reach inside it. */
    int32_t  sc_x0, sc_y0, sc_x1, sc_y1;
    int32_t  bb_x0, bb_y0, bb_x1, bb_y1;      /* empty when bb_x1 < bb_x0 */
    float    ss_x, ss_y;
    unsigned nverts;                           /* triangles * 3 */
    D3D8H2DVertex *verts;                      /* caller's array, D3D8H2D_MAX_VERTS */
    /* Textures by stage; bit u of tmask = stage u is sampled. */
    uint32_t tmask;
    D3D8H2DTexture tex[4];
    /* Register combiners as NV2A words (the host's shader interprets them). */
    uint32_t pixel_shader;                     /* 0 fixed-function (G43), else PS definition */
    uint32_t cc, control;
    uint32_t ci[8], ai[8], co[8], ao[8], k0[8], k1[8];
    uint32_t final_cw0, final_cw1, add_specular;
    /* Fragment state, NV2A method values as D3D pushed them. */
    uint32_t alpha_test, alpha_func, alpha_ref;
    uint32_t blend, blend_src, blend_dst, blend_eq, blend_color;
    uint32_t dither, color_mask;
    /* Depth, as D3D pushed it: test (0x30C), func (0x354, 0x200..0x207),
     * write mask (0x35C). The executor writes depth only when the test is on
     * (hw_depth_state_for), and so does the host. zs_* name D3D's depth
     * surface (device +0x2074), whose contents the shadow seeds from. */
    uint32_t depth_test, depth_func, depth_write, stencil_test;
    uint32_t zs_addr, zs_pitch;
    float    z_min, z_max;                     /* over the emitted vertices */
    uint32_t control_perturbed;                /* RECOMP_D3D8_HOST_2D_CONTROL applied */
} D3D8Host2DDraw;

/* Is this draw pre-transformed 2D? The vertex shader handle (device +0x384)
 * is an FVF (bit 0 clear) whose position field (0x00E) is XYZRHW (0x004). */
static inline int d3d8_host_2d_is_fvf_xyzrhw(uint32_t handle)
{
    return !(handle & 1u) && (handle & 0x00Eu) == 0x004u;
}
int d3d8_host_2d_is_2d(const D3D8HostDrawCheck *c);

/* Build the host's description of a 2D draw from the mirror's snapshot and
 * guest memory (`ram` is guest physical 0; `ram_size` bounds every read).
 * Returns NULL on success or a short refusal reason (a string literal; the
 * caller counts by pointer). `out->verts` must point at D3D8H2D_MAX_VERTS
 * vertices. `control` perturbs the result for the positive control: the
 * geometry moves two pixels right and the diffuse colour's red is inverted. */
const char *d3d8_host_2d_build(const D3D8HostDrawCheck *c, const uint8_t *ram, size_t ram_size,
                               int control, D3D8Host2DDraw *out);

/* ---- the renderer (d3d8_host_2d_metal.m) ----
 * Draw `d` over `pixels`, a w x h crop of an R5G6B5 target whose top-left is
 * (x0, y0) in target space, with rows `pitch_px` pixels apart. On entry
 * `pixels` holds the destination the draw starts from; on return, the result.
 * `depth`, when not NULL, is the same crop of the depth surface as
 * Depth32Float values (guest z / 16777215), rows w apart: it seeds a real
 * depth attachment and receives what the draw left there. A draw with the
 * depth test on and no `depth` fails. Synchronous. Returns 0 on success, or
 * -1 (pixels untouched) on failure; d3d8_host_2d_metal_last_error() names it. */
int d3d8_host_2d_metal_render(const D3D8Host2DDraw *d, const uint8_t *ram, size_t ram_size,
                              uint16_t *pixels, unsigned pitch_px, float *depth,
                              unsigned x0, unsigned y0, unsigned w, unsigned h);
const char *d3d8_host_2d_metal_last_error(void);

/* ---- the shadow bookkeeping (d3d8_host_2d.c) ---- */
typedef struct {
    /* Required. */
    int (*render)(const D3D8Host2DDraw *d, const uint8_t *ram, size_t ram_size,
                  uint16_t *pixels, unsigned pitch_px, float *depth,
                  unsigned x0, unsigned y0, unsigned w, unsigned h);
    /* Make guest RAM hold the executor's pixels for [p, p+bytes). */
    int (*sync_range)(uint8_t *p, size_t bytes);
    uint8_t *ram;
    size_t   ram_size;
    /* Optional: why the last render failed. */
    const char *(*last_error)(void);
    /* Optional: the executor's depth for the depth surface at `depth` (a host
     * pointer into `ram`), top-left w x h as guest z / 16777215. Returns 0 if
     * it holds none; the shadow then reads the surface from guest RAM, which
     * is what the executor itself would upload for it. nv2a_metal_depth_peek. */
    int (*depth_peek)(const uint8_t *depth, unsigned w, unsigned h, float *out);
} D3D8Host2DBackend;

/* 0 off, 1 shadow (and `draw`, which runs as shadow for now). Reads
 * RECOMP_D3D8_HOST_2D once. */
int  d3d8_host_2d_mode(void);
void d3d8_host_2d_set_backend(const D3D8Host2DBackend *b);
/* The token before a 2D draw's commands: snapshot the target it starts from. */
void d3d8_host_2d_pre(uint32_t serial, uint32_t rt_data, uint32_t rt_format, uint32_t rt_size,
                      uint32_t zs_data, uint32_t zs_size);
/* The mirror's token after it: build, capture the executor's result, render. */
void d3d8_host_2d_post(const D3D8HostDrawCheck *c, void (*exec_source)(D3D8ExecDrawTextures *));
/* NV097_FLIP_STALL: compare the frame's draws, report, dump. */
void d3d8_host_2d_flip(void);
void d3d8_host_2d_report(const char *why);
typedef struct {
    unsigned long long draws, built, rendered, compared, exact, within, mismatching, px, px_mismatch, dumped;
    unsigned long long depth_draws, depth_px, depth_px_mismatch, depth_draws_mismatching;
    unsigned long long z_from_texture, z_from_ram, proof_pass, proof_reject, proof_depends;
} D3D8H2DStats;
void d3d8_host_2d_get_stats(D3D8H2DStats *out);

/* The comparison, on its own, for the unit test: a w x h crop of the
 * starting pixels, the executor's result and the host's, in R5G6B5. */
typedef struct {
    unsigned long long pixels, exec_changed, host_changed, mismatch;
    unsigned max_err[3];       /* per channel, in 565 steps (R, G, B) */
} D3D8H2DDiff;
void d3d8_host_2d_diff(const uint16_t *pre, const uint16_t *exec, const uint16_t *host,
                       unsigned w, unsigned h, unsigned tol, D3D8H2DDiff *out);
/* Depth, like for like: both sides are Depth32Float holding guest z /
 * 16777215; each is quantised back to the guest's 24 bits (as the executor's
 * own readback does) and a pixel mismatches when they differ by more than
 * `tol` of those steps. Returns the mismatching count; *max_steps the worst. */
unsigned long long d3d8_host_2d_depth_diff(const float *exec, const float *host, size_t n,
                                           unsigned tol, unsigned *max_steps);
/* Can the depth test reject any fragment of a draw whose vertex z spans
 * [zmin, zmax] against stored depth spanning [smin, smax]? 1 every fragment
 * passes, -1 every fragment fails, 0 it depends. Fragment depth is the
 * screen-linear interpolation of vertex z, so it stays inside [zmin, zmax]. */
int d3d8_host_2d_depth_proof(uint32_t func, float zmin, float zmax, float smin, float smax);

#ifdef __cplusplus
}
#endif
#endif
