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
 *     factors D3D keeps at device +0x454/+0x458 and moved by D3D's screen-space
 *     offset D3D8H2D_SCREEN_OFFSET, z as given, clip w = 1/rhw;
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
 * RECOMP_D3D8_HOST_2D=shadow arms it (and the mirror with it).
 *
 * RECOMP_D3D8_HOST_2D=draw REPLACES the executor for this class. The mirror
 * writes the whole description as a token BEFORE the draw's commands; at it
 * the host builds the draw under exactly the shadow's eligibility rules and,
 * if it can, encodes it into the surface the executor has bound
 * (nv2a_metal_external_draw), then tells the executor to skip the draw's
 * batches (nv2a_pb_exec_host_skip) until the check token behind them. Any
 * draw the host refuses, or whose target the executor has not bound, is left
 * to the executor untouched. RECOMP_D3D8_HOST_2D_CONTROL=1 perturbs the host's
 * draws in this mode too, so what reaches the screen visibly is the host's.
 *
 * Split in two so the part that decides WHAT to draw can be tested with no
 * device: d3d8_host_2d.c (pure C, xbox_nv2a) builds a D3D8Host2DDraw and runs
 * the shadow bookkeeping through a backend; d3d8_host_2d_metal.m (xbox_vsh,
 * Apple only) is the backend's renderer. main.c introduces the two. */
#include <math.h>
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
/* D3D's pass-through program for XYZRHW adds c1.xy = (0.53125, 0.53125) to
 * every screen position (it loads c0 = (1, 1, 16777215, 1) and c1 with it;
 * docs/jsrf/handovers/CLAUDE_HANDOVER.txt section 11.2 records the upload).
 * 0.53125 is the NV2A's pixel-centre bias: 1/2 plus one 1/32 subpixel step.
 * The executor runs that program, so its positions carry the offset; without
 * it the host's picture sat half a pixel left of the executor's at every edge
 * (tutorial run 3, "Presented by SEGA"). The shadow prints the executor's c0/c1
 * and counts any draw where they are not these values. */
#define D3D8H2D_SCREEN_OFFSET 0.53125f
/* THEN THE EXECUTOR SNAPS: prepare_vertices (nv2a_pb_exec.c) and the GPU
 * program epilogue (nv2a_vsh_msl.c) both truncate screen x and y toward zero
 * to 1/16 pixel for |v| < 2^20, the NV2A's 4-bit subpixel grid. So a D3D
 * coordinate k lands on k + 0.5, not k + 0.53125: a full-screen quad from 0
 * covers column 0 and row 0 (their centres lie on its top-left edge), and the
 * logo's edges fall 1/32 px left of where the unsnapped offset put them.
 * Tutorial run 4 without it: 1,119 depth pixels (row 0 + column 0) on every
 * full-screen quad and r2 g3 b2 on the logo. */
static inline float d3d8_host_2d_snap(float v)
{
    return (v < 1048576.0f && v > -1048576.0f) ? truncf(v * 16.0f) / 16.0f : v;
}

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
    /* Triangles not drawn because a vertex's clip w = 1/rhw is not finite
     * (rhw 0: a zeroed slot in a partly filled dynamic buffer). The executor
     * loses them too: its CPU path's vertex_valid() rejects the triangle and
     * its GPU path clips it away. Tutorial run 5's one mismatch was such a
     * triangle, drawn by the host at w = 1 as a sliver from the corner. */
    uint32_t tris_dropped_w;
    uint32_t idx_min, idx_max;                 /* over the indices drawn */
    uint32_t cls;                              /* d3d8_host_2d_class */
    uint32_t cull_face, front_cw;              /* as applied: 0 none, 0x404 front, 0x405 back */
    uint32_t tris_dropped_q;                   /* textured unit q <= 0: the executor drops those too */
} D3D8Host2DDraw;

/* Is this draw pre-transformed 2D? The vertex shader handle (device +0x384)
 * is an FVF (bit 0 clear) whose position field (0x00E) is XYZRHW (0x004). */
static inline int d3d8_host_2d_is_fvf_xyzrhw(uint32_t handle)
{
    return !(handle & 1u) && (handle & 0x00Eu) == 0x004u;
}
int d3d8_host_2d_is_2d(const D3D8HostDrawCheck *c);
/* G51.3: an FVF whose position is XYZ or XYZ plus blend weights (0x002,
 * 0x006..0x00E): the fixed-function transform, the executor's mode 4. */
static inline int d3d8_host_2d_is_fvf_ff(uint32_t handle)
{
    return !(handle & 1u) && (handle & 0x00Eu) != 0x004u && (handle & 0x00Eu) != 0u;
}
/* 1 pre-transformed 2D, 2 fixed-function 3D, 0 neither (programmable, or no draw). */
int d3d8_host_2d_class(const D3D8HostDrawCheck *c);
/* The executor's fixed-function vertex unit (nv2a_ff_vertex), handed in. */
typedef const char *(*D3D8H2DFFVertexFn)(const uint32_t m[2048], const float in[16][4], float out[16][4]);

/* Build the host's description of a 2D draw from the mirror's snapshot and
 * guest memory (`ram` is guest physical 0; `ram_size` bounds every read).
 * Returns NULL on success or a short refusal reason (a string literal; the
 * caller counts by pointer). `out->verts` must point at D3D8H2D_MAX_VERTS
 * vertices. `control` perturbs the result for the positive control: the
 * geometry moves two pixels right and the diffuse colour's red is inverted. */
const char *d3d8_host_2d_build(const D3D8HostDrawCheck *c, const uint8_t *ram, size_t ram_size,
                               int control, D3D8Host2DDraw *out);
/* The same, with the draw's indices supplied (`idx`, c->count of them) rather
 * than read from guest memory at c->idx_ptr now. d3d8_host_2d_build(...) is
 * this with idx = NULL. */
const char *d3d8_host_2d_build_ex(const D3D8HostDrawCheck *c, const uint8_t *ram, size_t ram_size,
                                  int control, const uint16_t *idx, D3D8Host2DDraw *out);
/* G51.3: the same for either class. A fixed-function draw needs its register
 * file `ffm` (d3d8_host_ff_registers) and the evaluator `ffv`; each vertex is
 * assembled from the D3D arrays and transformed, lit and texgen'd by it. */
const char *d3d8_host_draw_build(const D3D8HostDrawCheck *c, const uint8_t *ram, size_t ram_size,
                                 int control, const uint16_t *idx, const uint32_t *ffm,
                                 D3D8H2DFFVertexFn ffv, D3D8Host2DDraw *out);

/* ---- the index ring: indices captured at the draw call ----
 * One producer (the thread calling D3D, through the mirror), one consumer
 * (the ring consumer at the post token). The producer reserves n contiguous
 * entries, fills them, and publishes; the consumer copies them out and then
 * checks that the producer has not come round onto them meanwhile. */
#define D3D8H2D_IDX_RING (1u << 20)
#define D3D8H2D_IDX_PER_DRAW 16384u
uint16_t *d3d8_host_2d_idx_reserve(uint32_t n, uint64_t *pos);
void      d3d8_host_2d_idx_publish(uint64_t pos, uint32_t n);
/* 1 and `out` filled, or 0 if the entries were never published or have been overwritten. */
int       d3d8_host_2d_idx_copy(uint64_t pos, uint32_t n, uint16_t *out);
/* FNV-1a over every enabled vertex array's bytes for indices imin..imax. */
uint64_t  d3d8_host_2d_vertex_hash(const uint8_t *ram, size_t ram_size, const D3D8HostDrawCheck *c,
                                   uint32_t imin, uint32_t imax);

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
    /* Draw mode. external_draw draws `d` into the executor's bound target
     * (1), or does nothing (0); exec_skip turns the executor's rasteriser off
     * and on; exec_skipped counts the batches it has skipped. */
    int (*external_draw)(const D3D8Host2DDraw *d, const uint8_t *ram, size_t ram_size);
    /* G51.3: the executor's fixed-function vertex unit, for the FF shadow. */
    D3D8H2DFFVertexFn ff_vertex;
    void (*exec_skip)(int on);
    unsigned long long (*exec_skipped)(void);
} D3D8Host2DBackend;

/* 0 off, 1 shadow, 2 draw. Reads RECOMP_D3D8_HOST_2D once. */
int  d3d8_host_2d_mode(void);
/* G51.3: 0 off, 1 shadow. RECOMP_D3D8_HOST_FF=shadow: fixed-function 3D
 * draws are drawn by the host into the same kind of scratch crop and
 * compared with the executor exactly as the 2D shadow does.
 *
 * SAMPLED BY FLIP. Shadowing a draw drains the executor twice, and a 3D
 * frame has ~550 fixed-function draws: shadowing every one ran the tutorial
 * at 1 fps (run g51-ffshadow). So only every RECOMP_D3D8_HOST_FF_STRIDE-th
 * flip (default 60; 1 = every flip) is shadowed -- all of its FF draws --
 * and the flips between cost nothing but a token per draw. */
int  d3d8_host_ff_mode(void);
/* Does the shadow take this draw (pre and post tokens)? */
int  d3d8_host_shadow_wants(const D3D8HostDrawCheck *c);
int  d3d8_host_shadow_wants_handle(uint32_t vs_handle);
/* Draw mode: the token before a 2D draw's commands, and the check behind them. */
void d3d8_host_2d_replace(const D3D8HostDrawCheck *c);
void d3d8_host_2d_after(const D3D8HostDrawCheck *c);
/* The renderer's draw-mode half: `d` into the executor's bound surface. */
int  d3d8_host_2d_metal_external(const D3D8Host2DDraw *d, const uint8_t *ram, size_t ram_size);
void d3d8_host_2d_set_backend(const D3D8Host2DBackend *b);
/* The token before a 2D draw's commands: snapshot the target it starts from. */
void d3d8_host_2d_pre(uint32_t serial, uint32_t vs_handle, uint32_t rt_data, uint32_t rt_format, uint32_t rt_size,
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
    unsigned long long idx_from_snapshot, idx_changed, vtx_changed, exec_outside_host_box;
    unsigned long long replace_tokens, replaced, replace_refused, replace_unbound, exec_batches_skipped,
                       replaced_without_skip;
    /* G51.3, fixed-function 3D in shadow: the same per-draw verdicts. */
    unsigned long long ff_draws, ff_built, ff_compared, ff_exact, ff_within, ff_mismatching, ff_px, ff_px_mismatch;
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
