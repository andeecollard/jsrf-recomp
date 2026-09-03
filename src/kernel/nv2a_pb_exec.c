/**
 * Execute the parts of the title's pushbuffer that produce visible pixels.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; without
 * something consuming them the framebuffer stays whatever it was, which is how
 * a fully booted title renders a black screen. This walks the same command
 * stream nv2a_pb_scan.c surveys and carries out the subset that decides what is
 * on screen: which surface is being drawn into, and clearing it.
 *
 * Geometry uses either pre-transformed attributes or the title's uploaded
 * NV2A vertex program. Shader positions and diffuse outputs feed the CPU
 * rasteriser directly. The measured linear RGB565 texture-copy / colour
 * program is supported; other configured fragment states are rejected.
 * Depth, blending and general colour programs remain unsupported.
 *
 * Everything this does not handle is counted and ranked by
 * nv2a_pb_exec_report(), so what remains is a list rather than a guess.
 *
 * Enabled with RECOMP_PB_EXEC. RECOMP_RASTER_TEST draws one known triangle
 * after every clear, which separates "the pixel path is broken" from "the title
 * has not given us any vertices". RECOMP_FB_DUMP=<prefix> writes the surface to
 * <prefix>NNN.bmp, so the result can be looked at without a display.
 */
#include "nv2a_vsh.h"
#include "nv2a_texture_copy.h"
#include "nv2a_regs.h"
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>     /* ptrdiff_t; MSVC gets it via another header */

/* strtok_s is MSVC's name for what POSIX calls strtok_r. Same signature and
 * same semantics, so one alias covers it rather than restructuring the two
 * call sites. */
#if !defined(_MSC_VER)
#define strtok_s(str, delim, ctx) strtok_r((str), (delim), (ctx))
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void *xbox_GpuMemoryRange(uint32_t address, size_t bytes);
extern const uint8_t *xbox_Nv2aRegisterMemory(void);
extern int xbox_HeapDescribe(uint32_t xbox_va, char *buf, size_t size);
extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowStart(void);

/* One vertex attribute stream, as the title describes it. Attribute 0 is
 * position; the rest are colours, texture coordinates and so on. */
typedef struct {
    uint32_t offset;      /* guest address of element 0 */
    uint32_t type;        /* NV097 data type nibble */
    uint32_t size;        /* components per element */
    uint32_t stride;      /* bytes between elements */
} VertexAttr;

#define NV_VERTEX_ATTRS 16
#define NV_MAX_INDICES  4096
/* Inline vertex data arrives as one dword per push, so a batch needs room for
 * the whole primitive: 16384 dwords is 2048 vertices at a typical eight-dword
 * layout, and the index array caps the batch at 4096 either way. */
#define NV_MAX_INLINE_WORDS 16384

static struct {
    VertexAttr attr[NV_VERTEX_ATTRS];
    uint32_t   prim;                    /* SET_BEGIN_END parameter, 0 = ended */
    uint16_t   idx[NV_MAX_INDICES];
    uint32_t   idx_count;
    uint32_t   inline_words[NV_MAX_INLINE_WORDS];
    uint32_t   inline_count;            /* dwords pushed this batch, 0 = none */
    float      vp_offset[4], vp_scale[4];
    int        vp_seen;                 /* the title programmed a viewport */
    uint32_t   draws, verts, nonzero_draws;
    float      min_x, max_x, min_y, max_y;
    uint32_t color_offset, pitch, format;
    uint32_t clip_x, clip_w, clip_y, clip_h;
    uint32_t clear_color;
    uint32_t clears, unhandled_total;
    uint32_t tris_drawn, tris_skipped_offscreen, batches_untransformed;
} s_gpu;

/* Unhandled methods, ranked. The interesting output is not that something was
 * skipped but which things dominate, because that is the order to implement
 * them in. */
#define PB_EXEC_MAX_UNHANDLED 2048
typedef struct { uint32_t method, count; } PbUnhandled;
static PbUnhandled s_unhandled[PB_EXEC_MAX_UNHANDLED];
static int s_unhandled_count;

/* Raw uploads remain available for diagnosis. Execution uses persistent
 * program/constant banks and the hardware LOAD/START cursors below. */
static struct {
    uint32_t words[NV2A_VS_MAX_INSTRUCTIONS][4];
    uint8_t loaded[NV2A_VS_MAX_INSTRUCTIONS];
    float constants[NV2A_VS_MAX_CONSTANTS][4];
    float current[16][4];
    uint32_t load, start, constant_load, mode;
    int dirty;
    NV2AVshProgram decoded;
    uint32_t batches, rejected;
} s_vsh;
/* RECOMP_VSH_TRACE measures delivery separately from the D3D11 sink's
 * unhandled histogram. Compare actual selected words, not just START/length. */
static struct {
    int enabled;
    uint64_t program_words[8], changed, dropped, constants, constant_dropped;
    uint64_t loads, starts, modes, current4f, current4ub, decodes;
    uint8_t current_written[16];
    uint32_t hashes[32], unique;
    uint32_t start_values, load_values;
    uint64_t load_high;
    struct { uint32_t method, param; } recent[2048];
    uint64_t recent_count;
} s_vsh_trace;
static float s_positions[NV_MAX_INDICES][4];
static uint32_t s_colors[NV_MAX_INDICES];
/* Retain all shader outputs for texture interpolation and draw captures. */
static float s_outputs[NV_MAX_INDICES][16][4];
static uint32_t s_methods[0x2000 / 4];
static uint8_t s_method_seen[0x2000 / 4];
static int s_capture_selected;
static struct {
    NV2ATextureCopy state;
    const uint8_t *texture;
    uint8_t *target;
    size_t texture_bytes, target_bytes;
    uint32_t texture_address, target_address, batches, rejected;
    int active;
} s_copy;

static const char *prepare_texture_copy(void)
{
    s_copy.active = 0;
    /* Preserve the original diagnostic flat-colour path when no fragment
     * state has been supplied. Once configured, unsupported states reject. */
    if (!s_method_seen[0x1e60/4] && !s_method_seen[0x1b0c/4]) return NULL;
    const char *error = nv2a_texture_copy_prepare(s_methods, &s_copy.state);
    if (error) return error;
    const uint8_t *regs = xbox_Nv2aRegisterMemory();
    if (!regs) return "NV2A mapping unavailable";
    uint32_t ramht, base, limit;
    memcpy(&ramht, regs + 0x2210, 4);
    NV2ATextureCopy *c = &s_copy.state;
    s_copy.texture_bytes = (size_t)c->pitch*c->height;
    s_copy.target_bytes = (size_t)c->target_pitch*(c->clip_y+c->clip_h);
    if (!nv2a_dma_resolve(regs+0x700000, 0x100000, ramht, c->texture_handle, &base, &limit)
            || (uint64_t)c->texture_offset+s_copy.texture_bytes > (uint64_t)limit+1
            || (uint64_t)base+c->texture_offset > UINT32_MAX) return "texture DMA range";
    s_copy.texture_address = base+c->texture_offset;
    s_copy.texture = xbox_GpuMemoryRange(s_copy.texture_address, s_copy.texture_bytes);
    if (!nv2a_dma_resolve(regs+0x700000, 0x100000, ramht, c->target_handle, &base, &limit)
            || (uint64_t)c->target_offset+s_copy.target_bytes > (uint64_t)limit+1
            || (uint64_t)base+c->target_offset > UINT32_MAX) return "target DMA range";
    s_copy.target_address = base+c->target_offset;
    s_copy.target = xbox_GpuMemoryRange(s_copy.target_address, s_copy.target_bytes);
    if (!s_copy.texture || !s_copy.target) return "surface outside mapped RAM";
    if ((uint64_t)s_copy.texture_address+s_copy.texture_bytes > s_copy.target_address
            && (uint64_t)s_copy.target_address+s_copy.target_bytes > s_copy.texture_address)
        return "overlapping texture and target";
    s_copy.active = 1;
    ++s_copy.batches;
    return NULL;
}



#define PB_PROG_SLOTS 1024
static struct { uint32_t method, param; } s_prog_log[PB_PROG_SLOTS];
static int s_prog_logged;

static void note_program_write(uint32_t method, uint32_t param)
{
    if (s_prog_logged < PB_PROG_SLOTS) {
        s_prog_log[s_prog_logged].method = method;
        s_prog_log[s_prog_logged].param  = param;
        s_prog_logged++;
    }
}

void nv2a_pb_exec_dump_program(void)
{
    int i;
    if (!s_prog_logged)
        return;
    fprintf(stderr, "[GPU] vertex program and constants, first %d writes:\n",
            s_prog_logged);
    for (i = 0; i < s_prog_logged; i++) {
        uint32_t m = s_prog_log[i].method, v = s_prog_log[i].param;
        float f;
        memcpy(&f, &v, sizeof(f));
        if (m >= NV097_SET_TRANSFORM_CONSTANT)
            fprintf(stderr, "  [GPU]   0x%04X = 0x%08X  (% .6f)\n", m, v, f);
        else
            fprintf(stderr, "  [GPU]   0x%04X = 0x%08X\n", m, v);
    }
    fflush(stderr);
}

static void note_unhandled(uint32_t method)
{
    int i;

    s_gpu.unhandled_total++;
    for (i = 0; i < s_unhandled_count; i++) {
        if (s_unhandled[i].method == method) {
            s_unhandled[i].count++;
            return;
        }
    }
    if (s_unhandled_count < PB_EXEC_MAX_UNHANDLED) {
        s_unhandled[s_unhandled_count].method = method;
        s_unhandled[s_unhandled_count].count = 1;
        s_unhandled_count++;
    }
}

/* Where each attribute sits inside one inline vertex, in dwords.
 *
 * INLINE_ARRAY carries the vertices in the command stream instead of pointing
 * at a buffer, so there is no offset or stride to read: the layout is implied
 * by which attributes are enabled and what format each one declares, packed in
 * attribute order. Recomputed per batch, because a title changes the format
 * between batches and a stale layout silently misreads every vertex.
 *
 * s_inline_stride of 0 means the layout could not be derived -- an attribute
 * declared a format this decoder does not know how to size -- and the batch is
 * then left alone rather than guessed at. */
static uint32_t s_inline_off[NV_VERTEX_ATTRS];
static uint32_t s_inline_stride;

#define INLINE_ATTR_ABSENT 0xFFFFFFFFu

static void inline_layout(void)
{
    uint32_t a, n = 0;

    s_inline_stride = 0;
    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        const VertexAttr *at = &s_gpu.attr[a];
        s_inline_off[a] = INLINE_ATTR_ABSENT;
        if (!at->size)
            continue;
        s_inline_off[a] = n;
        if (at->type == 2)                  /* float per component */
            n += at->size;
        else if (at->type == 4)             /* four normalised bytes, one dword */
            n += 1;
        else
            return;                         /* unknown packing: refuse the batch */
    }
    s_inline_stride = n;
}

/* Read attribute `a` of inline vertex `index`. Same contract as fetch_attr. */
static int fetch_inline(uint32_t a, uint32_t index, float out[4])
{
    const uint32_t *w;
    uint32_t i;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!s_inline_stride || a >= NV_VERTEX_ATTRS
            || s_inline_off[a] == INLINE_ATTR_ABSENT
            || (index + 1) * s_inline_stride > s_gpu.inline_count)
        return 0;

    w = &s_gpu.inline_words[index * s_inline_stride + s_inline_off[a]];
    if (s_gpu.attr[a].type == 2) {
        for (i = 0; i < s_gpu.attr[a].size && i < 4; i++)
            memcpy(&out[i], &w[i], sizeof(float));
        return 1;
    }
    for (i = 0; i < 4; i++)                 /* type 4 */
        out[i] = (float)((w[0] >> (i * 8)) & 0xFFu) / 255.0f;
    return 1;
}

/* Read attribute `a` of vertex `index` as floats. Anything this does not
 * decode returns 0, so a caller sees a degenerate vertex rather than reading
 * past the array -- and the batch is refused, which is how UB_D3D was found:
 * every draw JSRF submits declares its diffuse colour as
 *
 *     attribute 3 type=0 size=4 stride=32 offset=0x01BA2010
 *
 * and type 0 is UB_D3D, which was not decoded. That one gap refused 70,228 of
 * 70,228 rejected batches. UB_D3D is a packed D3DCOLOR: the bytes are stored
 * B, G, R, A, which is why it cannot share the UB_OGL case below it. */
static int fetch_attr(const VertexAttr *a, uint32_t index, float out[4])
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t i;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!a->offset || !a->size || !a->stride)
        return 0;
    p = mem + a->offset + (size_t)index * a->stride;

    switch (a->type) {
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
        /* Stored B, G, R, A. Only a 4-component D3DCOLOR is defined this way;
         * a shorter one would be a different declaration and is refused. */
        if (a->size != 4)
            return 0;
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = ((const float *)p)[i];
        return 1;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    default:
        /* S1, CMP and the rest are undecoded. The rejection report names the
         * type, so the next one that matters identifies itself rather than
         * being guessed at. */
        return 0;
    }
}

static uint32_t surface_bpp(void)
{
    switch (s_gpu.format & 15) {
    case 1: case 2: case 3: return 2;
    case 4: case 5: case 6: case 7: case 8: return 4;
    default: return s_gpu.clip_w ? s_gpu.pitch / s_gpu.clip_w : 0;
    }
}


/* Write the current surface out as a 24-bit BMP.
 *
 * A framebuffer window needs someone watching it. A file does not, which makes
 * this the only way to check what a title actually rendered on a machine you
 * are not sitting at -- and the only way to put a picture in a bug report.
 *
 * Programmable batches use guest shader outputs in NV2A screen space.
 * The measured RGB565 copy uses the portable texture/colour path.
 * Fixed-function batches retain the pre-transformed-position heuristic.
 */
static void dump_surface_bmp(void)
{
    const char *prefix = getenv("RECOMP_FB_DUMP");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    static int seq;
    char path[512];
    uint32_t w = s_gpu.clip_w, h = s_gpu.clip_h, y, x;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    FILE *f;

    if (!prefix || !w || !h || (bpp != 2 && bpp != 4) || !s_gpu.color_offset)
        return;

    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%03d.bmp", prefix, seq++);
    f = fopen(path, "wb");
    if (!f)
        return;

    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    /* BMP rows run bottom-up. */
    for (y = h; y-- > 0; ) {
        const uint8_t *row = mem + s_gpu.color_offset
                           + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
        for (x = 0; x < w; x++) {
            uint8_t bgr[3];
            if (bpp == 4) {
                uint32_t v = ((const uint32_t *)row)[s_gpu.clip_x + x];
                bgr[0] = (uint8_t)(v);
                bgr[1] = (uint8_t)(v >> 8);
                bgr[2] = (uint8_t)(v >> 16);
            } else {
                uint16_t v = ((const uint16_t *)row)[s_gpu.clip_x + x];
                bgr[0] = (uint8_t)(( v        & 0x1F) << 3);
                bgr[1] = (uint8_t)(((v >>  5) & 0x3F) << 2);
                bgr[2] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            }
            fwrite(bgr, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    if (seq == 1)
        fprintf(stderr, "  [GPU] framebuffer dump: %s (%ux%u from 0x%08X %ubpp)\n",
                path, w, h, s_gpu.color_offset, bpp);
}

/* Defined below, next to the rest of the rasteriser; the clear path uses it
 * for RECOMP_RASTER_TEST. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb);

static void clear_surface(uint32_t param)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    uint32_t y, x;

    if (!(param & (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G | NV097_CLEAR_SURFACE_B | NV097_CLEAR_SURFACE_A)))
        return;                            /* depth/stencil only */
    if (!s_gpu.color_offset || !s_gpu.pitch || !s_gpu.clip_h || bpp == 0)
        return;

    for (y = 0; y < s_gpu.clip_h; y++) {
        uint8_t *row = mem + s_gpu.color_offset
                     + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
        if (bpp == 4) {
            uint32_t *p = (uint32_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = s_gpu.clear_color;
        } else if (bpp == 2) {
            /* The clear value is always given as A8R8G8B8; a 16-bit surface
             * takes the same colour reduced to 5:6:5. */
            uint16_t v = (uint16_t)(((s_gpu.clear_color >> 8) & 0xF800)
                                  | ((s_gpu.clear_color >> 5) & 0x07E0)
                                  | ((s_gpu.clear_color >> 3) & 0x001F));
            uint16_t *p = (uint16_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = v;
        }
    }
    s_gpu.clears++;
    /* Progress markers, interleaved with everything else in the log. The
     * summary says drawing stopped; only a marker next to the surrounding
     * activity says what the title was doing when it stopped. */
    if ((s_gpu.clears % 100) == 0)
        fprintf(stderr, "  [GPU] clear #%u\n", s_gpu.clears);
    /* Distinct clear colours actually used. "Cleared to black" and "the clear
     * never ran" look identical in the framebuffer, and only one of them is a
     * bug -- so record what was asked for, not just how often. */
    {
        static uint32_t seen[8];
        static int n;
        int i;
        for (i = 0; i < n; i++)
            if (seen[i] == s_gpu.clear_color) break;
        if (i == n && n < 8) {
            seen[n++] = s_gpu.clear_color;
            fprintf(stderr, "  [GPU] clear colour 0x%08X -> surface 0x%08X"
                            " (%ubpp)\n",
                    s_gpu.clear_color, s_gpu.color_offset, surface_bpp());
        }
    }

    /* Prove the pixel path end to end, independent of whether the title has
     * given us any geometry yet.
     *
     * "Nothing on screen" has three very different causes -- the surface
     * address or pitch is wrong, the rasteriser is broken, or the title's
     * vertex buffers are empty -- and they are indistinguishable from a black
     * window. RECOMP_RASTER_TEST draws one known triangle into the surface
     * just cleared, so a visible triangle rules out the first two and leaves
     * only the third. On Wreckless it is the third: attribute 0 decodes
     * correctly (float, size 2, stride 16) and the buffer it points at stays
     * zero.
     *
     * ponytail: bring-up aid, not a feature. It costs one branch per clear. */
    if (getenv("RECOMP_RASTER_TEST")) {
        static int announced;
        /* Every clear, not once: the title clears each frame and double-buffers,
         * so a triangle drawn a single time is erased before anyone sees it. */
        if (s_gpu.clip_w && s_gpu.clip_h) {
            float a[2], b[2], c[2];
            a[0] = s_gpu.clip_w * 0.5f; a[1] = s_gpu.clip_h * 0.15f;
            b[0] = s_gpu.clip_w * 0.85f; b[1] = s_gpu.clip_h * 0.85f;
            c[0] = s_gpu.clip_w * 0.15f; c[1] = s_gpu.clip_h * 0.85f;
            raster_triangle(a, b, c, 0xFFFF00FFu);   /* magenta: never a clear colour */
            if (announced++ == 0)
            fprintf(stderr, "  [GPU] raster self-test: triangle (%.0f,%.0f)"
                            " (%.0f,%.0f) (%.0f,%.0f) into 0x%08X %ubpp\n",
                    a[0], a[1], b[0], b[1], c[0], c[1],
                    s_gpu.color_offset, surface_bpp());
        }
    }

    /* Show the surface actually being drawn into. A title that double-buffers
     * renders into the back buffer, so following AvSetDisplayMode's address
     * would show the one nothing is writing. */
    xbox_FramebufferWindowSet(s_gpu.color_offset, s_gpu.pitch);

    /* And open the window, rather than waiting for AvSetDisplayMode to do it.
     *
     * That was the only caller, so a title which draws before setting a display
     * mode -- or never sets one at all -- got no window however much it
     * rendered. The Xbox Dashboard clears a 1280x960 surface at 0x00088000 on
     * its first frame and had not called AvSetDisplayMode by then, so
     * RECOMP_FB_WINDOW=1 was set, the executor knew the address and the pitch,
     * and nothing appeared.
     *
     * Here is the better trigger anyway: this runs when a surface address is
     * known to be real, because a clear just used it. Idempotent and gated on
     * RECOMP_FB_WINDOW, so the cost is one interlocked compare per clear. */
    xbox_FramebufferWindowStart();
}


/* ── Rasteriser ──────────────────────────────────────────────────────────
 *
 * Fills triangles straight into the guest framebuffer, the same memory
 * clear_surface() writes and the framebuffer window already shows. That is the
 * whole reason it is done on the CPU rather than through D3D: nothing new has
 * to be plumbed for the result to be visible.
 *
 * ponytail: flat-shaded, no depth buffer, no texturing, no perspective
 * correction, and only batches whose attribute 0 is already in screen space.
 * A title running a vertex program hands over object-space positions that mean
 * nothing without executing the program, so those batches are counted and
 * skipped rather than drawn somewhere wrong. Upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c once vertex programs are
 * translated; this exists to get the first geometry on screen for every title,
 * which in practice is UI, HUD and 2D overlays -- all pre-transformed.
 */

static void put_pixel(uint8_t *mem, uint32_t bpp, int x, int y, uint32_t argb)
{
    uint8_t *row;

    if (x < (int)s_gpu.clip_x || x >= (int)(s_gpu.clip_x + s_gpu.clip_w))
        return;
    if (y < (int)s_gpu.clip_y || y >= (int)(s_gpu.clip_y + s_gpu.clip_h))
        return;
    row = mem + s_gpu.color_offset + (size_t)y * s_gpu.pitch;
    if (bpp == 4) {
        ((uint32_t *)row)[x] = argb;
    } else if (bpp == 2) {
        ((uint16_t *)row)[x] = (uint16_t)(((argb >> 8) & 0xF800)
                                        | ((argb >> 5) & 0x07E0)
                                        | ((argb >> 3) & 0x001F));
    }
}

/* Half-space fill. Barycentric edge functions rather than scanline slopes:
 * the same test decides both windings, so a title that emits clockwise
 * triangles does not silently render nothing. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    float area;
    int minx, maxx, miny, maxy, x, y;

    if (bpp != 4 && bpp != 2)
        return;

    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f)
        return;                            /* degenerate */

    if (!isfinite(area) || !isfinite(a[0]) || !isfinite(a[1])
            || !isfinite(b[0]) || !isfinite(b[1]) || !isfinite(c[0]) || !isfinite(c[1])) return;
    float left = (float)s_gpu.clip_x, right = (float)(s_gpu.clip_x + s_gpu.clip_w);
    float top = (float)s_gpu.clip_y, bottom = (float)(s_gpu.clip_y + s_gpu.clip_h);
    minx = (int)floorf(fmaxf(left, fminf(right, fminf(a[0], fminf(b[0], c[0])))));
    maxx = (int)ceilf(fmaxf(left, fminf(right, fmaxf(a[0], fmaxf(b[0], c[0])))));
    miny = (int)floorf(fmaxf(top, fminf(bottom, fminf(a[1], fminf(b[1], c[1])))));
    maxy = (int)ceilf(fmaxf(top, fminf(bottom, fmaxf(a[1], fmaxf(b[1], c[1])))));
    if (minx >= maxx || miny >= maxy) {
        s_gpu.tris_skipped_offscreen++;
        return;
    }

    for (y = miny; y < maxy; y++) {
        for (x = minx; x < maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))
                put_pixel(mem, bpp, x, y, argb);
        }
    }
    s_gpu.tris_drawn++;
}

/* One vertex source for both paths: a batch that pushed inline data reads from
 * it, anything else reads the arrays the title pointed at. */
static int fetch_vertex(uint32_t a, uint32_t index, float out[4])
{
    if (s_gpu.inline_count)
        return fetch_inline(a, index, out);
    return fetch_attr(&s_gpu.attr[a], index, out);
}

static uint32_t pack_color(const float c[4])
{
    uint32_t n[4];
    for (int k = 0; k < 4; ++k)
        n[k] = (uint32_t)(fminf(fmaxf(c[k], 0.0f), 1.0f) * 255.0f);
    return (n[3] << 24) | (n[0] << 16) | (n[1] << 8) | n[2];
}

/* Why the last batch was refused.
 *
 * prepare_vertices has five distinct ways to fail and the counter only said
 * how often, so "38,472 rejected" named no cause and could not be acted on.
 * These are the causes, counted separately, with the first offending detail
 * kept for each -- which attribute could not be fetched, which output
 * components the program left unwritten. Reason strings are literals and are
 * compared by pointer, so the table stays a fixed size. */
static struct { const char *reason; uint32_t n; uint32_t detail; }
    s_vsh_reject[8];
static const char *s_vsh_reason;
static uint32_t s_vsh_reason_detail;

#define VSH_REJECT(text, value) \
    do { s_vsh_reason = (text); s_vsh_reason_detail = (uint32_t)(value); \
         return 0; } while (0)

static void note_vsh_reject(void)
{
    const char *reason = s_vsh_reason ? s_vsh_reason : "unrecorded";
    for (size_t i = 0; i < sizeof s_vsh_reject / sizeof s_vsh_reject[0]; ++i) {
        if (!s_vsh_reject[i].reason) {
            s_vsh_reject[i].reason = reason;
            s_vsh_reject[i].detail = s_vsh_reason_detail;
        }
        if (s_vsh_reject[i].reason == reason) { s_vsh_reject[i].n++; return; }
    }
}

static uint32_t vsh_sample_batch(void)
{
    static long batch = -1;

    if (batch < 0) {
        const char *env = getenv("RECOMP_VSH_SAMPLE");
        batch = env ? strtol(env, NULL, 0) : 20000;
        if (batch < 4) batch = 4;
    }
    return (uint32_t)batch;
}

static void trace_selected_program(void)
{
    if (!s_vsh_trace.enabled) return;
    ++s_vsh_trace.decodes;
    uint32_t hash = 2166136261u;
    for (int i = 0; i < s_vsh.decoded.length; ++i)
        for (int k = 0; k < 4; ++k)
            hash = (hash ^ s_vsh.words[s_vsh.start+i][k]) * 16777619u;
    for (uint32_t i = 0; i < s_vsh_trace.unique; ++i)
        if (s_vsh_trace.hashes[i] == hash) return;
    if (s_vsh_trace.unique == 32) return;
    s_vsh_trace.hashes[s_vsh_trace.unique++] = hash;
    fprintf(stderr, "[VSH-TRACE] selected hash=%08X draw=%u start=%u slots=%d reads=%04X valid=%d final=%d\n",
            hash, s_gpu.draws, s_vsh.start, s_vsh.decoded.length,
            s_vsh.decoded.inputs_read, s_vsh.decoded.valid, s_vsh.decoded.has_final);
    for (int i = 0; i < s_vsh.decoded.length; ++i) {
        const uint32_t *w = s_vsh.words[s_vsh.start+i];
        fprintf(stderr, "[VSH-TRACE] slot=%u %08X %08X %08X %08X\n",
                s_vsh.start+i, w[0], w[1], w[2], w[3]);
    }
}

static void trace_vertex_inputs(const float inputs[16][4])
{
    if (!s_vsh_trace.enabled) return;
    if (s_vsh.batches == vsh_sample_batch()) {
        uint64_t first = s_vsh_trace.recent_count > 2048 ? s_vsh_trace.recent_count-2048 : 0;
        for (uint64_t i = first; i < s_vsh_trace.recent_count; ++i)
            fprintf(stderr, "[VSH-METHOD] %04X %08X\n",
                    s_vsh_trace.recent[i%2048].method, s_vsh_trace.recent[i%2048].param);
    }
    for (unsigned a = 0; a < 16; ++a) {
        if (!(s_vsh.decoded.inputs_read & (1u << a))) continue;
        fprintf(stderr, "[VSH-TRACE] draw=%u input=%u array=%u current-written=%X value=(%.9g %.9g %.9g %.9g)\n",
                s_gpu.draws, a, s_gpu.attr[a].size, s_vsh_trace.current_written[a],
                inputs[a][0], inputs[a][1], inputs[a][2], inputs[a][3]);
    }
    uint8_t seen[NV2A_VS_MAX_CONSTANTS] = {0};
    for (int i = 0; i < s_vsh.decoded.length; ++i) {
        const NV2AVshInstruction *ins = &s_vsh.decoded.insns[i];
        unsigned used = nv2a_vsh_mac_sources(ins->mac_op) | (ins->ilu_op ? 4 : 0);
        for (int k = 0; k < 3; ++k) {
            const NV2AVshSrcOperand *src = &ins->mac_src[k];
            if (!(used & (1u << k)) || src->reg_type != NV2A_VSH_REG_CONST
                    || src->reg_index >= NV2A_VS_MAX_CONSTANTS || seen[src->reg_index]) continue;
            seen[src->reg_index] = 1;
            const float *c = s_vsh.constants[src->reg_index];
            fprintf(stderr, "[VSH-TRACE] draw=%u constant=%d relative=%d value=(%.9g %.9g %.9g %.9g)\n",
                    s_gpu.draws, src->reg_index, src->rel_addr, c[0], c[1], c[2], c[3]);
        }
    }
}

static int prepare_vertices(void)
{
    int programmable = s_vsh.mode == 2;
    s_vsh_reason = NULL;
    s_vsh_reason_detail = 0;
    if (s_vsh.mode != 0 && !programmable)
        VSH_REJECT("fixed-function transform mode is not implemented", s_vsh.mode);
    if (programmable && s_vsh.dirty) {
        uint32_t n = 0;
        while (s_vsh.start < NV2A_VS_MAX_INSTRUCTIONS
                && n < NV2A_VS_MAX_INSTRUCTIONS - s_vsh.start
                && s_vsh.loaded[s_vsh.start + n] == 15) {
            if (s_vsh.words[s_vsh.start + n++][3] & 1) break;
        }
        nv2a_vsh_parse(n ? s_vsh.words[s_vsh.start] : NULL, (int)n, &s_vsh.decoded);
        s_vsh.dirty = 0;
        trace_selected_program();
    }
    for (uint32_t i = 0; i < s_gpu.idx_count; ++i) {
        if (programmable) {
            float inputs[16][4];
            NV2AVshResult result;
            memcpy(inputs, s_vsh.current, sizeof(inputs));
            for (uint32_t a = 0; a < 16; ++a) {
                if (!(s_vsh.decoded.inputs_read & (1u << a))) continue;
                if (s_gpu.attr[a].size && !fetch_vertex(a, s_gpu.idx[i], inputs[a])) {
                    /* Name the record, once. "attribute 3 failed" is one step
                     * from useless; the format byte is the answer, because
                     * fetch_attr only decodes float and UB_OGL and a D3D8
                     * title declares its diffuse colour as UB_D3D. */
                    static uint32_t told;
                    if (!(told & (1u << a))) {
                        told |= 1u << a;
                        fprintf(stderr, "  [VSH] attribute %u unfetchable: "
                                "type=%u size=%u stride=%u offset=0x%08X "
                                "inline=%u\n", a, s_gpu.attr[a].type,
                                s_gpu.attr[a].size, s_gpu.attr[a].stride,
                                s_gpu.attr[a].offset, s_gpu.inline_count);
                        fflush(stderr);
                    }
                    VSH_REJECT("vertex attribute could not be fetched", a);
                }
            }
            if (i == 0 && (s_vsh.batches < 4 || s_vsh.batches == vsh_sample_batch()))
                trace_vertex_inputs(inputs);
            if (!nv2a_vsh_execute(&s_vsh.decoded, inputs, s_vsh.constants, &result))
                VSH_REJECT("shader execution failed", s_vsh.decoded.length);
            if ((result.written[0] & 12) != 12)
                VSH_REJECT("program left oPos.zw unwritten", result.written[0]);
            memcpy(s_outputs[i], result.output, sizeof(s_outputs[i]));
            memcpy(s_positions[i], result.output[0], sizeof(s_positions[i]));
            s_colors[i] = pack_color(result.output[NV2A_VSH_OUT_D0]);
            /* NV2A programs include the viewport transform and perspective
             * division. Only screen subpixel quantisation remains here. */
            for (int k = 0; k < 2; ++k) {
                if (!isfinite(s_positions[i][k]))
                    VSH_REJECT("oPos is not finite", k);
                if (fabsf(s_positions[i][k]) < 0x1p20f)
                    s_positions[i][k] = truncf(s_positions[i][k] * 16.0f) / 16.0f;
                s_outputs[i][0][k] = s_positions[i][k];
            }
            /* Sample a late batch as well as the first few. The early ones
             * are the measured full-screen blit and always look the same;
             * whether the title's own geometry transforms differently is a
             * question about batch 20,000, not batch 1. RECOMP_VSH_SAMPLE
             * moves the sample point. */
            if (s_vsh.batches == vsh_sample_batch() && i < 3) {
                /* The constants matter as much as the attributes. A program
                 * that produces an infinite w is dividing by something, and
                 * the transform rows are where that something comes from. */
                fprintf(stderr, "  [VSH] late batch c0..c7:");
                for (uint32_t k = 0; k < 8; ++k)
                    fprintf(stderr, " [%g %g %g %g]", s_vsh.constants[k][0],
                            s_vsh.constants[k][1], s_vsh.constants[k][2],
                            s_vsh.constants[k][3]);
                fprintf(stderr, "\n  [VSH] late batch attrs:");
                for (uint32_t k = 0; k < 16; ++k)
                    if (s_gpu.attr[k].size)
                        fprintf(stderr, " a%u(t%u s%u st%u @%08X)", k,
                                s_gpu.attr[k].type, s_gpu.attr[k].size,
                                s_gpu.attr[k].stride, s_gpu.attr[k].offset);
                fprintf(stderr, " reads=%04X inline=%u idx=%u\n",
                        s_vsh.decoded.inputs_read, s_gpu.inline_count,
                        s_gpu.idx[i]);
                /* The values actually handed to the program, and the raw
                 * bytes behind them. A constant oPos has exactly two causes
                 * -- the inputs are zero, or the transform is -- and these
                 * two lines tell them apart. */
                fprintf(stderr, "  [VSH] late batch in0=[%g %g %g %g]"
                                " in3=[%g %g %g %g] in9=[%g %g %g %g]\n",
                        inputs[0][0], inputs[0][1], inputs[0][2], inputs[0][3],
                        inputs[3][0], inputs[3][1], inputs[3][2], inputs[3][3],
                        inputs[9][0], inputs[9][1], inputs[9][2], inputs[9][3]);
                {
                    /* Vertex array offsets are relative to a DMA context, the
                     * way texture and surface offsets are -- and unlike those,
                     * fetch_attr treats them as absolute guest addresses. If
                     * either vertex DMA object has a non-zero base, that
                     * assumption is wrong and every array read lands in the
                     * wrong place. Resolve them the same way the copy path
                     * does and say what they are. */
                    const uint8_t *regs = xbox_Nv2aRegisterMemory();
                    uint32_t ramht = 0, base, limit;
                    if (regs) memcpy(&ramht, regs + 0x2210, 4);
                    for (int which = 0; which < 2; ++which) {
                        uint32_t m = which ? NV097_SET_CONTEXT_DMA_VERTEX_B
                                           : NV097_SET_CONTEXT_DMA_VERTEX_A;
                        uint32_t handle = s_methods[m / 4];
                        int seen = s_method_seen[m / 4];
                        int ok = regs && seen && nv2a_dma_resolve(regs + 0x700000,
                                0x100000, ramht, handle, &base, &limit);
                        fprintf(stderr, "  [VSH] vertex DMA %c: seen=%d "
                                        "handle=0x%08X resolved=%d base=0x%08X "
                                        "limit=0x%08X\n", which ? 'B' : 'A',
                                seen, handle, ok, ok ? base : 0,
                                ok ? limit : 0);
                    }
                }
                {
                    char owner[192];
                    xbox_HeapDescribe(s_gpu.attr[0].offset, owner, sizeof owner);
                    fprintf(stderr, "  [VSH] array owner: %s\n", owner);
                }
                {
                    const uint8_t *raw = (const uint8_t *)xbox_GetMemoryOffset()
                            + s_gpu.attr[0].offset
                            + (size_t)s_gpu.idx[i] * s_gpu.attr[0].stride;
                    fprintf(stderr, "  [VSH] late batch bytes:");
                    for (uint32_t k = 0; k < 32; ++k)
                        fprintf(stderr, " %02X", raw[k]);
                    fprintf(stderr, "\n");
                }
            }
            if ((s_vsh.batches < 4 || s_vsh.batches == vsh_sample_batch()) && i < 3)
                fprintf(stderr, "  [VSH] start=%u slots=%d vertex=%u oPos=(%.6g %.6g %.6g %.6g) color=%08X\n",
                        s_vsh.start, s_vsh.decoded.length, i,
                        result.output[0][0], result.output[0][1], result.output[0][2], result.output[0][3], s_colors[i]);
        } else {
            float color[4];
            if (!fetch_vertex(0, s_gpu.idx[i], s_positions[i]))
                VSH_REJECT("position attribute could not be fetched", 0);
            int has_color = fetch_vertex(3, s_gpu.idx[i], color);
            s_colors[i] = has_color ? pack_color(color) : 0xFFFFFFFFu;
            memset(s_outputs[i], 0, sizeof(s_outputs[i]));
            memcpy(s_outputs[i][0], s_positions[i], sizeof(s_positions[i]));
            for (int k=0;k<4;++k) s_outputs[i][NV2A_VSH_OUT_D0][k] = has_color ? color[k] : 1;
            fetch_vertex(9, s_gpu.idx[i], s_outputs[i][NV2A_VSH_OUT_T0]);
        }
    }
    if (programmable) s_vsh.batches++;
    return 1;
}

/* Capture state at a draw boundary, not during a periodic interrupt report.
 * The first draw and two later samples distinguish initial setup from steady
 * state. Raw method values are included even when rendering does not support
 * them yet. The snapshot format is deliberately independent of C structs. */
static void capture_bytes(const char *extension, const void *data, size_t size)
{
    const char *prefix = getenv("RECOMP_DRAW_CAPTURE");
    if (!prefix || !data || !s_capture_selected) return;
    char path[768];
    snprintf(path, sizeof(path), "%s%06u.%s", prefix, s_gpu.draws, extension);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    if (fwrite(data, 1, size, f) != size) perror(path);
    if (fclose(f)) perror(path);
}

static void capture_draw(const char *error)
{
    const char *prefix = getenv("RECOMP_DRAW_CAPTURE");
    s_capture_selected = prefix && (s_gpu.draws == 1 || s_gpu.draws == 128 || s_gpu.draws == 2048
            || (error && s_copy.rejected < 2));
    if (!s_capture_selected) return;
    char path[768];
    snprintf(path, sizeof(path), "%s%06u.json", prefix, s_gpu.draws);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "{\n\"version\":1,\"draw\":%u,\"primitive\":%u,\"registers\":{", s_gpu.draws, s_gpu.prim);
    int comma = 0;
    for (unsigned i = 0; i < 0x2000/4; ++i) if (s_method_seen[i]) {
        fprintf(f, "%s\n\"%04x\":%u", comma ? "," : "", i*4, s_methods[i]); comma = 1;
    }
    fprintf(f, "},\n\"vertices\":[");
    for (unsigned i = 0; i < s_gpu.idx_count; ++i) {
        fprintf(f, "%s[", i ? "," : "");
        for (int o = 0; o < 16; ++o) {
            fprintf(f, "%s[", o ? "," : "");
            for (int k = 0; k < 4; ++k) {
                if (k) fputc(',', f);
                if (isfinite(s_outputs[i][o][k])) fprintf(f, "%.9g", s_outputs[i][o][k]);
                else fputs("null", f);
            }
            fputc(']', f);
        }
        fputc(']', f);
    }
    const uint8_t *regs = xbox_Nv2aRegisterMemory();
    uint32_t ramht = 0;
    if (regs) memcpy(&ramht, regs + 0x2210, 4);
    fprintf(f, "],\"ramht\":%u,\"copy_supported\":%s,\"texture_address\":%u,\"target_address\":%u}\n",
            ramht, s_copy.active ? "true" : "false", s_copy.texture_address, s_copy.target_address);
    if (regs) capture_bytes("ramin", regs+0x700000, 0x100000);
    if (s_copy.active) {
        capture_bytes("texture", s_copy.texture, s_copy.texture_bytes);
        capture_bytes("before", s_copy.target, s_copy.target_bytes);
    }
    if (fclose(f)) perror(path);
    else fprintf(stderr, "[DRAW] captured %s\n", path);
}

/* Is attribute 0 already in screen space? Measured, not assumed: every vertex
 * of the batch has to land inside the surface. Object-space positions are
 * small numbers around the origin and fail this immediately, which is what
 * keeps an untransformed batch from being smeared across the top-left corner.
 */
static int batch_is_screen_space(void)
{
    float p[4];
    uint32_t i;

    if (!s_gpu.clip_w || !s_gpu.clip_h)
        return 0;
    for (i = 0; i < s_gpu.idx_count; i++) {
        if (!fetch_vertex(0, s_gpu.idx[i], p))
            return 0;
        if (p[0] < (float)s_gpu.clip_x - 1.0f
         || p[0] > (float)(s_gpu.clip_x + s_gpu.clip_w) + 1.0f
         || p[1] < (float)s_gpu.clip_y - 1.0f
         || p[1] > (float)(s_gpu.clip_y + s_gpu.clip_h) + 1.0f)
            return 0;
    }
    return 1;
}

/* NV097 primitive types that are triangles under some winding. */
#define NV_PRIM_TRIANGLES      NV097_SET_BEGIN_END_OP_TRIANGLES
#define NV_PRIM_TRIANGLE_STRIP NV097_SET_BEGIN_END_OP_TRIANGLE_STRIP
#define NV_PRIM_TRIANGLE_FAN   NV097_SET_BEGIN_END_OP_TRIANGLE_FAN
#define NV_PRIM_QUADS          NV097_SET_BEGIN_END_OP_QUADS
#define NV_PRIM_QUAD_STRIP     NV097_SET_BEGIN_END_OP_QUAD_STRIP

static void raster_indices(uint32_t a, uint32_t b, uint32_t c)
{
    if (!s_copy.active) {
        raster_triangle(s_positions[a], s_positions[b], s_positions[c], s_colors[a]);
        return;
    }
    if (nv2a_texture_copy_triangle(&s_copy.state, s_copy.texture, s_copy.texture_bytes,
            s_copy.target, s_copy.target_bytes, s_outputs[a], s_outputs[b], s_outputs[c]))
        ++s_gpu.tris_drawn;
    else {
        if (++s_copy.rejected <= 4) fprintf(stderr, "[TEXTURE] rejected triangle geometry / bounds\n");
    }
}

static void raster_batch(void)
{
    uint32_t i;
    uint32_t drawn_before = s_gpu.tris_drawn;

    if (s_gpu.idx_count < 3)
        return;
    if (!prepare_vertices()) {
        s_vsh.rejected++;
        note_vsh_reject();
        if (s_vsh.rejected <= 4)
            fprintf(stderr, "  [VSH] rejected batch mode=%u start=%u valid=%d final=%d: "
                            "%s (%u)\n",
                    s_vsh.mode, s_vsh.start, s_vsh.decoded.valid, s_vsh.decoded.has_final,
                    s_vsh_reason ? s_vsh_reason : "unrecorded", s_vsh_reason_detail);
        return;
    }
    const char *copy_error = prepare_texture_copy();
    capture_draw(copy_error);
    if (copy_error) {
        if (++s_copy.rejected <= 8) fprintf(stderr, "[TEXTURE] rejected draw %u: %s\n", s_gpu.draws, copy_error);
        return;
    }
    if (s_vsh.mode == 0 && !batch_is_screen_space()) {
        s_gpu.batches_untransformed++;
        /* RECOMP_PB_EXEC_NOCLIPTEST: rasterise anyway, to tell "the vertices
         * were decoded but sit outside the clip rect" apart from "the vertices
         * are not usable at all". Diagnostic only -- the test exists because
         * untransformed vertices would otherwise paint nonsense. */
        {
            static int allow = -1;
            if (allow < 0) allow = getenv("RECOMP_PB_EXEC_NOCLIPTEST") ? 1 : 0;
            if (!allow)
                return;
        }
    }

    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < s_gpu.idx_count; i += 3)
            raster_indices(i, i+1, i+2);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < s_gpu.idx_count; i++)
            raster_indices(i, i+1, i+2);
        break;
    case NV_PRIM_TRIANGLE_FAN:
        for (i = 1; i + 1 < s_gpu.idx_count; i++)
            raster_indices(0, i, i+1);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < s_gpu.idx_count; i += 4) {
            raster_indices(i, i+1, i+2);
            raster_indices(i, i+2, i+3);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < s_gpu.idx_count; i += 2) {
            raster_indices(i, i+1, i+3);
            raster_indices(i, i+3, i+2);
        }
        break;
    default:
        break;                             /* points and lines: not yet */
    }
    if (s_copy.active) capture_bytes("after", s_copy.target, s_copy.target_bytes);
    /* Report-time snapshots may interrupt the clear or raster loops. Capture
     * a few completed batches when inspecting the actual rendered result. */
    if (s_gpu.tris_drawn != drawn_before) {
        static unsigned captured;
        if (captured < 3 && getenv("RECOMP_FB_DUMP_DRAW")) {
            captured++;
            dump_surface_bmp();
        }
    }

    if (s_gpu.tris_drawn && (s_gpu.tris_drawn % 500) == 0)
        fprintf(stderr, "  [GPU] %u triangles rasterised\n", s_gpu.tris_drawn);
}

/* What a batch actually contains. Before anything can be rasterised, the
 * question is what space attribute 0 arrives in: a title running a vertex
 * program hands over object-space positions that mean nothing without running
 * it, while pre-transformed screen-space coordinates can be drawn directly. */
static void draw_primitive(void)
{
    float v[4];
    uint32_t i;

    if (!s_gpu.prim || !s_gpu.idx_count)
        return;
    s_gpu.draws++;
    if ((s_gpu.draws % 200) == 0)
        fprintf(stderr, "  [GPU] draw #%u\n", s_gpu.draws);
    s_gpu.verts += s_gpu.idx_count;

    /* How many batches carry coordinates at all, and what range they span.
     * A pipeline that decodes perfectly and draws nothing is indistinguishable
     * from one that never ran, unless the vertices themselves are measured. */
    {
        float p[4];
        if (fetch_vertex(0, s_gpu.idx[0], p)) {
            if (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f) {
                s_gpu.nonzero_draws++;
                if (p[0] < s_gpu.min_x) s_gpu.min_x = p[0];
                if (p[0] > s_gpu.max_x) s_gpu.max_x = p[0];
                if (p[1] < s_gpu.min_y) s_gpu.min_y = p[1];
                if (p[1] > s_gpu.max_y) s_gpu.max_y = p[1];
            }
        }
    }

    raster_batch();

    if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
        static int shown;
        if (shown++ < 6) {
            fprintf(stderr, "  [GPU] prim %u, %u indices, pos attr:"
                            " off 0x%08X type %u size %u stride %u\n",
                    s_gpu.prim, s_gpu.idx_count, s_gpu.attr[0].offset,
                    s_gpu.attr[0].type, s_gpu.attr[0].size, s_gpu.attr[0].stride);
            {
                /* Every attribute the batch has, not just position. If the
                 * other streams carry data and position does not, the problem
                 * is one buffer rather than the whole vertex path. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t a, k;
                for (a = 0; a < NV_VERTEX_ATTRS; a++) {
                    const VertexAttr *at = &s_gpu.attr[a];
                    uint32_t nz = 0;
                    if (!at->offset || !at->size)
                        continue;
                    for (k = 0; k < 64; k++)
                        if (mem[at->offset + k]) nz++;
                    fprintf(stderr, "  [GPU]   attr%-2u off 0x%08X type %u"
                                    " size %u stride %-3u  %u/64 bytes set\n",
                            a, at->offset, at->type, at->size, at->stride, nz);
                }
            }
            {
                /* Raw bytes at the array, in case the values read as zero:
                 * that looks the same whether the offset is wrong or the
                 * buffer genuinely has not been filled yet. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t k;
                fprintf(stderr, "  [GPU]   bytes @0x%08X:", s_gpu.attr[0].offset);
                for (k = 0; k < 32; k++)
                    fprintf(stderr, " %02X", mem[s_gpu.attr[0].offset + k]);
                fprintf(stderr, "\n");
                fprintf(stderr, "  [GPU]   indices:");
                for (k = 0; k < s_gpu.idx_count && k < 8; k++)
                    fprintf(stderr, " %u", s_gpu.idx[k]);
                fprintf(stderr, "\n");
            }
            for (i = 0; i < s_gpu.idx_count && i < 3; i++) {
                if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], v))
                    fprintf(stderr, "  [GPU]   v[%u] = %.3f %.3f %.3f %.3f\n",
                            s_gpu.idx[i], v[0], v[1], v[2], v[3]);
            }
        }
    }
}

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    static int inited;
    if (!inited) {
        inited = 1;
        s_vsh.dirty = 1;
        s_vsh_trace.enabled = getenv("RECOMP_VSH_TRACE") != NULL;
        for (int i = 0; i < 16; ++i) s_vsh.current[i][3] = 1.0f;
        s_gpu.min_x = s_gpu.min_y = 1e30f;
        s_gpu.max_x = s_gpu.max_y = -1e30f;
    }
    /* Bring-up: the first parameters each surface method carries. A wrong
     * pitch or clip is indistinguishable from a method never arriving unless
     * the values are visible. */
    if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
        static int shown[8];
        int slot = -1;
        switch (method) {
        case NV097_SET_SURFACE_CLIP_HORIZONTAL: slot = 0; break;
        case NV097_SET_SURFACE_CLIP_VERTICAL:   slot = 1; break;
        case NV097_SET_SURFACE_FORMAT:          slot = 2; break;
        case NV097_SET_SURFACE_PITCH:           slot = 3; break;
        case NV097_SET_SURFACE_COLOR_OFFSET:    slot = 4; break;
        case NV097_SET_COLOR_CLEAR_VALUE:       slot = 5; break;
        case NV097_CLEAR_SURFACE:               slot = 6; break;
        default: break;
        }
        if (slot >= 0 && shown[slot]++ < 4)
            fprintf(stderr, "  [GPU] subch %u method 0x%04X param 0x%08X\n",
                    subch, method, param);
    }

    if (s_vsh_trace.enabled && subch < 8
            && method >= NV097_SET_TRANSFORM_PROGRAM && method < NV097_SET_TRANSFORM_CONSTANT)
        ++s_vsh_trace.program_words[subch];
    if (subch != 0) {                      /* 3D class lives on subchannel 0 */
        note_unhandled(method);
        return;
    }
    if (method < 0x2000 && !(method & 3)) {
        s_methods[method/4] = param;
        s_method_seen[method/4] = 1;
    }
    if (s_vsh_trace.enabled) {
        unsigned i = (unsigned)(s_vsh_trace.recent_count++ % 2048);
        s_vsh_trace.recent[i].method = method;
        s_vsh_trace.recent[i].param = param;
    }
    if ((method >= NV097_SET_TRANSFORM_PROGRAM && method < 0x0C00)
            || (method >= NV097_SET_TRANSFORM_EXECUTION_MODE && method <= NV097_SET_TRANSFORM_CONSTANT_LOAD))
        note_program_write(method, param);
    if (method >= NV097_SET_TRANSFORM_PROGRAM && method < NV097_SET_TRANSFORM_CONSTANT) {
        unsigned component = (method & 15) / 4;
        if (s_vsh.load < NV2A_VS_MAX_INSTRUCTIONS) {
            if (s_vsh.words[s_vsh.load][component] != param || !(s_vsh.loaded[s_vsh.load] & (1u << component))) {
                s_vsh.dirty = 1;
                if (s_vsh_trace.enabled) ++s_vsh_trace.changed;
            }
            s_vsh.words[s_vsh.load][component] = param;
            s_vsh.loaded[s_vsh.load] |= (uint8_t)(1u << component);
        } else if (s_vsh_trace.enabled) ++s_vsh_trace.dropped;
        if (component == 3 && s_vsh.load < NV2A_VS_MAX_INSTRUCTIONS) s_vsh.load++;
        return;
    }
    if (method >= NV097_SET_TRANSFORM_CONSTANT && method < 0x0C00) {
        if (s_vsh_trace.enabled) {
            ++s_vsh_trace.constants;
            if (s_vsh.constant_load >= NV2A_VS_MAX_CONSTANTS) ++s_vsh_trace.constant_dropped;
        }
        unsigned component = (method & 15) / 4;
        if (s_vsh.constant_load < NV2A_VS_MAX_CONSTANTS)
            memcpy(&s_vsh.constants[s_vsh.constant_load][component], &param, sizeof(float));
        if (component == 3 && s_vsh.constant_load < NV2A_VS_MAX_CONSTANTS) s_vsh.constant_load++;
        return;
    }
    if (method >= NV097_SET_VERTEX_DATA4F_M && method < NV097_SET_VERTEX_DATA4F_M + 16*16) {
        unsigned word = (method - NV097_SET_VERTEX_DATA4F_M) / 4;
        memcpy(&s_vsh.current[word / 4][word % 4], &param, sizeof(float));
        if (s_vsh_trace.enabled) {
            ++s_vsh_trace.current4f;
            s_vsh_trace.current_written[word/4] |= (uint8_t)(1u << (word%4));
        }
        return;
    }
    if (method >= NV097_SET_VERTEX_DATA4UB && method < NV097_SET_VERTEX_DATA4UB + 16*4) {
        unsigned a = (method - NV097_SET_VERTEX_DATA4UB) / 4;
        for (int k = 0; k < 4; ++k) s_vsh.current[a][k] = ((param >> (8*k)) & 255) / 255.0f;
        if (s_vsh_trace.enabled) {
            ++s_vsh_trace.current4ub;
            s_vsh_trace.current_written[a] = 15;
        }
        return;
    }
    switch (method) {
    case NV097_SET_TRANSFORM_EXECUTION_MODE:
        if (s_vsh_trace.enabled) ++s_vsh_trace.modes;
        s_vsh.mode = param & 3; break;
    case NV097_SET_TRANSFORM_PROGRAM_LOAD:
        if (s_vsh_trace.enabled) {
            ++s_vsh_trace.loads;
            if (param < 32) s_vsh_trace.load_values |= 1u << param;
            else s_vsh_trace.load_high++;
        }
        s_vsh.load = param; break;
    case NV097_SET_TRANSFORM_PROGRAM_START:
        if (s_vsh_trace.enabled) {
            ++s_vsh_trace.starts;
            /* A final value of 0 does not mean the title never selected
             * another program; it means the last selection was 0. Record
             * every distinct one. */
            if (param < 32) s_vsh_trace.start_values |= 1u << param;
        }
        if (s_vsh.start != param) s_vsh.dirty = 1;
        s_vsh.start = param; break;
    case NV097_SET_TRANSFORM_CONSTANT_LOAD: s_vsh.constant_load = param; break;
    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        s_gpu.clip_x = param & 0xFFFF;
        s_gpu.clip_w = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_CLIP_VERTICAL:
        s_gpu.clip_y = param & 0xFFFF;
        s_gpu.clip_h = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_FORMAT:
        s_gpu.format = param;
        break;
    case NV097_SET_SURFACE_PITCH:
        s_gpu.pitch = param & 0xFFFF;      /* colour pitch; zeta is the top half */
        break;
    case NV097_SET_SURFACE_COLOR_OFFSET:
        s_gpu.color_offset = param;
        break;
    case NV097_SET_COLOR_CLEAR_VALUE:
        s_gpu.clear_color = param;
        break;
    case NV097_CLEAR_SURFACE:
        clear_surface(param);
        break;

    case NV097_SET_BEGIN_END:
        if (param) {
            s_gpu.prim = param;
            s_gpu.idx_count = 0;
            s_gpu.inline_count = 0;
        } else {
            /* An inline batch names its vertices 0..n-1, so the topology code
             * below is the same one the indexed path uses. */
            if (s_gpu.inline_count) {
                inline_layout();
                if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
                    static int shown_inline;
                    if (shown_inline++ < 40) {
                        uint32_t a, k;
                        fprintf(stderr, "  [GPU] inline batch: prim %u, %u dwords,"
                                " stride %u dw\n", s_gpu.prim,
                                s_gpu.inline_count, s_inline_stride);
                        fprintf(stderr, "  [GPU]   viewport seen=%d"
                                " scale %.3f %.3f %.3f %.3f"
                                " offset %.3f %.3f %.3f %.3f\n",
                                s_gpu.vp_seen,
                                s_gpu.vp_scale[0], s_gpu.vp_scale[1],
                                s_gpu.vp_scale[2], s_gpu.vp_scale[3],
                                s_gpu.vp_offset[0], s_gpu.vp_offset[1],
                                s_gpu.vp_offset[2], s_gpu.vp_offset[3]);
                        for (a = 0; a < NV_VERTEX_ATTRS; a++)
                            if (s_gpu.attr[a].size)
                                fprintf(stderr, "  [GPU]   attr%-2u type %u size %u"
                                        " -> dw off %u\n", a, s_gpu.attr[a].type,
                                        s_gpu.attr[a].size, s_inline_off[a]);
                        fprintf(stderr, "  [GPU]   words:");
                        for (k = 0; k < s_gpu.inline_count && k < 24; k++)
                            fprintf(stderr, " %08X", s_gpu.inline_words[k]);
                        fprintf(stderr, "\n");
                    }
                }
                if (s_inline_stride) {
                    uint32_t nv = s_gpu.inline_count / s_inline_stride, k;
                    if (nv > NV_MAX_INDICES)
                        nv = NV_MAX_INDICES;
                    for (k = 0; k < nv; k++)
                        s_gpu.idx[k] = (uint16_t)k;
                    s_gpu.idx_count = nv;
                }
            }
            draw_primitive();
            s_gpu.prim = 0;
            s_gpu.inline_count = 0;    /* after the draw: fetch_vertex reads it */
        }
        break;

    case NV097_INLINE_ARRAY:
        if (s_gpu.prim && s_gpu.inline_count < NV_MAX_INLINE_WORDS)
            s_gpu.inline_words[s_gpu.inline_count++] = param;
        break;

    case NV097_ARRAY_ELEMENT16:
        /* Two 16-bit indices per parameter word. */
        if (s_gpu.prim && s_gpu.idx_count + 2 <= NV_MAX_INDICES) {
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param & 0xFFFF);
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param >> 16);
        }
        break;
    default:
        if (method >= NV097_SET_VERTEX_DATA_ARRAY_OFFSET
                && method < NV097_SET_VERTEX_DATA_ARRAY_OFFSET + NV_VERTEX_ATTRS * 4) {
            s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4].offset = param;
        } else if (method >= NV097_SET_VERTEX_DATA_ARRAY_FORMAT
                && method < NV097_SET_VERTEX_DATA_ARRAY_FORMAT + NV_VERTEX_ATTRS * 4) {
            VertexAttr *a = &s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4];
            a->type   =  param        & 0x0F;
            a->size   = (param >> 4)  & 0x0F;
            a->stride = (param >> 8)  & 0xFF;
        } else if (method >= NV097_SET_VIEWPORT_OFFSET
                && method < NV097_SET_VIEWPORT_OFFSET + 16) {
            memcpy(&s_gpu.vp_offset[(method - NV097_SET_VIEWPORT_OFFSET) / 4],
                   &param, sizeof(float));
            s_gpu.vp_seen = 1;
        } else if (method >= NV097_SET_VIEWPORT_SCALE
                && method < NV097_SET_VIEWPORT_SCALE + 16) {
            memcpy(&s_gpu.vp_scale[(method - NV097_SET_VIEWPORT_SCALE) / 4],
                   &param, sizeof(float));
            s_gpu.vp_seen = 1;
        } else {
            note_unhandled(method);
        }
        break;
    }
}

/* Find where the title actually wrote its quad.
 *
 * The GPU is pointed at a buffer that stays zero, which says the data went
 * somewhere else -- and the only way to find somewhere else is to look for the
 * data. A screen-space quad for a 640x480 target contains 640.0f and 480.0f as
 * floats, which is a distinctive enough pair to search guest RAM for. Whatever
 * address that turns up is where the title's writes are landing, and the
 * difference from the programmed offset is the bug. */
static void find_quad_vertices(void)
{
    const uint32_t W = 0x44200000u;   /* 640.0f */
    const uint32_t H = 0x43F00000u;   /* 480.0f */
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for 640.0f/480.0f pairs...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 8 && hits < 12; i++) {
        if (ram[i] != W && ram[i] != H)
            continue;
        /* Both values within a few words of each other: a lone 640.0f is
         * common, the pair much less so. */
        {
            int has_w = 0, has_h = 0;
            uint32_t k;
            for (k = 0; k < 8; k++) {
                if (ram[i + k] == W) has_w = 1;
                if (ram[i + k] == H) has_h = 1;
            }
            if (!has_w || !has_h)
                continue;
        }
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 8; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 8;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none found -- the quad is not in RAM in"
                        " that form\n");
    fflush(stderr);
}

/* Locate NaN-filled transform matrices in guest RAM.
 *
 * A matrix arriving as NaN says the maths went wrong somewhere upstream, and
 * the only way to find where is to find the matrix and watch who writes it.
 * Three consecutive real-indefinite values is a distinctive enough signature:
 * ordinary data does not contain runs of 0xFFC00000. */
static void find_nan_matrices(void)
{
    const uint32_t NAN_NEG = 0xFFC00000u;
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for NaN matrices...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 20 && hits < 10; i++) {
        if (ram[i] != NAN_NEG || ram[i + 1] != NAN_NEG || ram[i + 2] != NAN_NEG)
            continue;
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 16;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none in RAM -- the NaNs are computed into"
                        " registers, not stored\n");
    fflush(stderr);
}

/* Print guest dwords named by RECOMP_PEEK, as hex and as float.
 *
 * Chasing a value backwards means reading it, and a value that is only wrong
 * for one frame in a thousand cannot be caught by stopping. Both
 * interpretations are printed because the question is usually "is this a
 * pointer or a number", and guessing wrong costs a run. */
static void peek_addresses(void)
{
    const char *spec = getenv("RECOMP_PEEK");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256];
    char *tok, *ctx = NULL;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t va = (uint32_t)strtoul(tok, NULL, 0);
        uint32_t v;
        float f;
        if (va < 0x1000u || va >= 0x04000000u)
            continue;
        v = *(const uint32_t *)(mem + va);
        memcpy(&f, &v, 4);
        fprintf(stderr, "  [PEEK] 0x%08X = %08X  (%g)\n", va, v, f);
    }
    fflush(stderr);
}

/* Walk a pointer chain and print every step.
 *
 * RECOMP_PEEK_CHAIN="0x1315A8,8,0x10,0" starts at that address, and for each
 * offset dereferences the current pointer and adds it. Following a chain by
 * hand costs one run per level; this costs one run for the whole chain, and
 * prints where it goes wrong when a level is null.
 */
static void peek_chain(void)
{
    const char *spec = getenv("RECOMP_PEEK_CHAIN");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256], *tok, *ctx = NULL;
    uint32_t cur = 0;
    int step = 0;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t off = (uint32_t)strtoul(tok, NULL, 0);
        if (step == 0) {
            cur = off;
            fprintf(stderr, "  [CHAIN] start 0x%08X\n", cur);
        } else {
            if (cur < 0x1000u || cur + 4 >= 0x04000000u) {
                fprintf(stderr, "  [CHAIN] step %d: 0x%08X is not a guest"
                                " pointer -- chain ends\n", step, cur);
                return;
            }
            cur = *(const uint32_t *)(mem + cur) + off;
            fprintf(stderr, "  [CHAIN] step %d: deref +0x%X -> 0x%08X\n",
                    step, off, cur);
        }
        step++;
    }
    if (cur >= 0x1000u && cur + 4 < 0x04000000u)
        fprintf(stderr, "  [CHAIN] final value at 0x%08X = 0x%08X\n",
                cur, *(const uint32_t *)(mem + cur));
    fflush(stderr);
}

void nv2a_pb_exec_report(void)
{
    if (s_vsh_trace.enabled) {
        fprintf(stderr, "[VSH-TRACE] upload words by subchannel:");
        for (int i = 0; i < 8; ++i)
            fprintf(stderr, " %d=%llu", i, (unsigned long long)s_vsh_trace.program_words[i]);
        fprintf(stderr, " changed=%llu dropped=%llu loads=%llu starts=%llu modes=%llu constants=%llu constant-dropped=%llu current4f=%llu current4ub=%llu decodes=%llu unique-hashes=%u load=%u constant-load=%u\n",
                (unsigned long long)s_vsh_trace.changed, (unsigned long long)s_vsh_trace.dropped,
                (unsigned long long)s_vsh_trace.loads, (unsigned long long)s_vsh_trace.starts,
                (unsigned long long)s_vsh_trace.modes, (unsigned long long)s_vsh_trace.constants,
                (unsigned long long)s_vsh_trace.constant_dropped, (unsigned long long)s_vsh_trace.current4f,
                (unsigned long long)s_vsh_trace.current4ub, (unsigned long long)s_vsh_trace.decodes,
                s_vsh_trace.unique, s_vsh.load, s_vsh.constant_load);
        /* Which program slots the title ever selected. A final start of 0 is
         * only the last selection; the bitmaps say whether there was ever
         * another one to miss. */
        fprintf(stderr, "[VSH-TRACE] distinct START slots 0x%08X, LOAD slots"
                        " 0x%08X, LOAD >= 32: %llu\n",
                s_vsh_trace.start_values, s_vsh_trace.load_values,
                (unsigned long long)s_vsh_trace.load_high);
    }
    fprintf(stderr, "[TEXTURE] prepared=%u rejected=%u\n", s_copy.batches, s_copy.rejected);
    fprintf(stderr, "[VSH] executed batches=%u rejected=%u mode=%u start=%u slots=%d\n",
            s_vsh.batches, s_vsh.rejected, s_vsh.mode, s_vsh.start, s_vsh.decoded.length);
    for (size_t i = 0; i < sizeof s_vsh_reject / sizeof s_vsh_reject[0]; ++i) {
        if (!s_vsh_reject[i].reason) break;
        fprintf(stderr, "[VSH]   %8u  %s (first detail %u)\n",
                s_vsh_reject[i].n, s_vsh_reject[i].reason, s_vsh_reject[i].detail);
    }
    peek_addresses();
    peek_chain();
    if (getenv("RECOMP_FIND_NAN")) {
        /* Every report, not once: the matrix is fine early on and only turns
         * to NaN later, so a single scan at startup finds nothing and says
         * nothing. */
        find_nan_matrices();
    }
    if (getenv("RECOMP_FIND_QUAD")) {
        static int done;
        if (!done) { done = 1; find_quad_vertices(); }
    }
    int i, j;

    fprintf(stderr, "[GPU] surface 0x%08X pitch %u clip %ux%u+%u+%u"
                    " clears %u | %u unhandled methods (%d distinct)\n",
            s_gpu.color_offset, s_gpu.pitch, s_gpu.clip_w, s_gpu.clip_h,
            s_gpu.clip_x, s_gpu.clip_y, s_gpu.clears,
            s_gpu.unhandled_total, s_unhandled_count);
    fprintf(stderr, "[GPU] draws %u (%u with coordinates), %u indices;"
                    " x %.1f..%.1f  y %.1f..%.1f\n",
            s_gpu.draws, s_gpu.nonzero_draws, s_gpu.verts,
            s_gpu.min_x, s_gpu.max_x, s_gpu.min_y, s_gpu.max_y);
    /* One picture per report rather than per clear: a title clears hundreds of
     * times a second and nobody wants that many files. */
    dump_surface_bmp();

    /* Drawn and skipped separately: "nothing appeared" and "every batch needed
     * a vertex program we do not run" look identical on screen, and only one
     * of them means the rasteriser is broken. */
    int n_top;
    fprintf(stderr, "[GPU] rasterised %u triangles; %u batches skipped as not"
                    " screen-space, %u triangles fully off-surface\n",
            s_gpu.tris_drawn, s_gpu.batches_untransformed,
            s_gpu.tris_skipped_offscreen);

    /* Top ten by frequency: selection sort over a small table, once every few
     * seconds, is not worth a better algorithm. RECOMP_PB_EXEC_TOP raises the
     * cut, because "which methods does this title use at all" is a different
     * question from "which dominate", and answering it by guessing at NV2A
     * register numbers is how you implement the wrong one. */
    {
        static int top = -1;
        if (top < 0) {
            const char *e = getenv("RECOMP_PB_EXEC_TOP");
            top = e ? atoi(e) : 10;
            if (top <= 0) top = 10;
        }
        n_top = top;
    }
    for (i = 0; i < n_top && i < s_unhandled_count; i++) {
        int best = i;
        for (j = i + 1; j < s_unhandled_count; j++)
            if (s_unhandled[j].count > s_unhandled[best].count)
                best = j;
        if (best != i) {
            PbUnhandled t = s_unhandled[i];
            s_unhandled[i] = s_unhandled[best];
            s_unhandled[best] = t;
        }
        fprintf(stderr, "  [GPU]   0x%04X x%u\n",
                s_unhandled[i].method, s_unhandled[i].count);
    }
    if (getenv("RECOMP_PB_EXEC_PROGRAM"))
        nv2a_pb_exec_dump_program();
    fflush(stderr);
}
