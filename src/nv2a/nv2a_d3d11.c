/*
 * Native raster path for Windows, structurally identical to nv2a_metal.m.
 *
 * Same contract, same rejections, same surface bookkeeping: the batch state
 * the CPU rasteriser already prepared goes to the GPU, and anything this file
 * does not understand returns -1 so the software path still draws it.
 *
 * Two things differ from the Metal twin, both forced by the API rather than
 * chosen. Metal reads the destination pixel inside the fragment shader under a
 * raster order group, so it implements depth, stencil and blending itself in
 * one RGBA32Float surface with Z packed into alpha. D3D11 has no portable
 * equivalent, so those three become real pipeline state against a real
 * R8G8B8A8 target and a real D24S8 depth buffer -- which is what the hardware
 * is for, and is why this needs no programmable blending. And Metal samples
 * the guest's texture bytes directly in the shader; a D3D11 sampler cannot
 * read Morton-swizzled or block-compressed guest layouts, so each texture is
 * unpacked once through the CPU rasteriser's own decoder and cached.
 *
 * The combiner evaluation is a transliteration of the Metal fragment shader,
 * driven by the same words out of the same constant buffer. That is
 * deliberate: it is the version that has been measured correct against JSRF on
 * macOS, and keeping the two hosts bit-comparable is the entire reason this
 * build exists. See the note beside build_shaders() for why the D3D8 layer's
 * register-combiner compiler is not used here.
 */
#include "d3d8_internal.h"           /* COBJMACROS, d3d11.h, device accessors */
#include <d3dcompiler.h>
#include "nv2a_d3d11.h"
#include "recomp_gpu_own.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

/* ================================================================
 * Shaders
 * ================================================================
 *
 * The vertex stage is fixed; the fragment stage is generated per combiner
 * configuration and cached, because Wine's HLSL compiler cannot take the other
 * shape. The first version of this file carried one uniform-driven
 * interpreter, the exact transliteration of the Metal fragment shader, with a
 * `static float4 r[14]` register file indexed by the combiner words at
 * runtime. Measured against this bottle's d3dcompiler_47, which is Wine's own
 * over libvkd3d-shader: a trivial shader, a cbuffer, a texture sample, a
 * projected sample, four samplers, a sample inside a branch, discard, a
 * literal-indexed local array and a DYNAMICALLY indexed cbuffer array all
 * compile; a module-scope array segfaults the compiler, taking the process
 * with it. So the register file becomes fourteen named locals and every index
 * is resolved here instead. What is emitted is the "locals only" shape, which
 * the same probe compiled cleanly.
 *
 * Why not d3d8_combiners_get_shader(), which does generate per-state HLSL: the
 * decoder that feeds it, d3d8_combiners_from_render_states(), reads the four
 * packed inputs of an ICW starting from the LOW byte and the AB destination
 * from the LOW nibble of an OCW. NV2A hardware puts A in the high byte and the
 * CD destination in the low nibble -- nouveau's NV20_3D_RC_IN_RGB_A__MASK is
 * 0xff000000 and NV20_3D_RC_OUT_RGB_CD_OUTPUT__MASK is 0x0000000f -- which is
 * what nv2a_metal.m decodes and what produced correct JSRF frames. The words
 * held here are hardware words, so that decoder would need a byte reversal and
 * a nibble swap per register; on top of that NV2ATextureCopy carries no
 * final-combiner inputs, so the final stage would have to be synthesised
 * rather than translated, and it initialises R0 to all of T0 where the
 * hardware initialises only R0.alpha. Three transforms this build cannot
 * verify, to reach a shader that still differs from the macOS one. What is
 * emitted below is instead a line-for-line transliteration of the Metal
 * fragment shader, so the two hosts agree by construction and a frame captured
 * on one is a reference for the other.
 */
static const char *const vertex_source =
"cbuffer VSParams : register(b0) {\n"
"    float4 viewport;      /* x,y: clip width and height in pixels */\n"
"    float4 texscale[4];   /* xy: scales texel coordinates into [0,1] */\n"
"};\n"
"struct VSIn {\n"
"    float4 p:POSITION; float4 d0:COLOR0; float4 d1:COLOR1;\n"
"    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
"};\n"
"struct VSOut {\n"
"    float4 p:SV_POSITION; float4 d0:COLOR0; float4 d1:COLOR1;\n"
"    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
"};\n"
/* s_outputs already holds NV2A screen space, so this is the same mapping the
 * Metal vertex stage does: pixels to clip space, Z from the guest's 24-bit
 * range, everything premultiplied by w so the divide restores it. */
"VSOut vs_main(VSIn v) {\n"
"    VSOut o;\n"
"    float z = clamp(v.p.z, 0.0, 16777215.0) / 16777215.0;\n"
"    o.p  = float4((v.p.x / viewport.x * 2 - 1) * v.p.w,\n"
"                  (1 - v.p.y / viewport.y * 2) * v.p.w, z * v.p.w, v.p.w);\n"
"    o.d0 = v.d0; o.d1 = v.d1;\n"
"    o.t0 = float4(v.t0.xy * texscale[0].xy, v.t0.z, v.t0.w);\n"
"    o.t1 = float4(v.t1.xy * texscale[1].xy, v.t1.z, v.t1.w);\n"
"    o.t2 = float4(v.t2.xy * texscale[2].xy, v.t2.z, v.t2.w);\n"
"    o.t3 = float4(v.t3.xy * texscale[3].xy, v.t3.z, v.t3.w);\n"
"    return o;\n"
"}\n";

/* The pixel stage's shared preamble: the same VSOut, the texture declarations
 * for the bound units only, and the one value that varies per draw without
 * changing the program. */
static const char *const pixel_preamble =
"struct VSOut {\n"
"    float4 p:SV_POSITION; float4 d0:COLOR0; float4 d1:COLOR1;\n"
"    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
"};\n"
"cbuffer PSParams : register(b0) { uint4 pfrag; };  /* x: alpha reference */\n";

static const char *const clear_pixel_source =
"struct VSOut {\n"
"    float4 p:SV_POSITION; float4 d0:COLOR0; float4 d1:COLOR1;\n"
"    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
"};\n"
"cbuffer ClearParams : register(b0) { float4 clear_color; };\n"
"float4 ps_clear(VSOut i) : SV_TARGET { return clear_color; }\n";

/* Everything about a batch that changes the emitted program. alpha_ref is not
 * here on purpose: it rides in the constant buffer, so a fade does not compile
 * 256 shaders. */
typedef struct {
    uint32_t combiner_count, texture_mask, untextured, modulate;
    uint32_t add_specular, alpha_test, dither;
    uint32_t cicw[8], aicw[8], cocw[8], aocw[8];
} ShaderKey;

typedef struct { ShaderKey key; ID3D11PixelShader *shader; int failed; } ShaderEntry;
#define SHADER_CACHE_SIZE 64
static ShaderEntry shader_cache[SHADER_CACHE_SIZE];
static unsigned shader_used, shader_evictions;

static void shader_key(const NV2ATextureCopy *s, ShaderKey *k)
{
    unsigned i;
    memset(k, 0, sizeof(*k));
    k->combiner_count = s->combiner_count;
    k->texture_mask = s->texture_mask;
    k->untextured = s->untextured;
    k->modulate = s->modulate;
    k->add_specular = s->add_specular;
    k->alpha_test = s->alpha_test;
    k->dither = s->dither;
    /* Only the active stages are emitted, so only they may key the cache. */
    for (i = 0; i < s->combiner_count && i < 8; i++) {
        k->cicw[i] = s->color_icw[i];
        k->aicw[i] = s->alpha_icw[i];
        k->cocw[i] = s->color_ocw[i];
        k->aocw[i] = s->alpha_ocw[i];
    }
}

/* A bounded appender: every emit goes through it, and one truncation poisons
 * the whole program rather than producing HLSL that happens to still parse. */
typedef struct { char *at; char *end; int overflow; } Emit;
static void emit(Emit *e, const char *fmt, ...)
{
    va_list args;
    int wrote;
    if (e->overflow) return;
    va_start(args, fmt);
    wrote = vsnprintf(e->at, (size_t)(e->end - e->at), fmt, args);
    va_end(args);
    if (wrote < 0 || wrote >= (int)(e->end - e->at)) { e->overflow = 1; return; }
    e->at += wrote;
}

/* One packed combiner input byte as an HLSL expression.
 *
 * Register 0 reads zero and is never written, which is how the NV2A spells
 * "unused"; 14 and 15 are the final combiner's EF_PROD and V1R0_SUM, which a
 * general stage cannot name, and they fold onto the same zero rather than
 * running off the end of the register file the way the Metal shader's
 * r[source] would. The alpha half of a combiner reads the BLUE component when
 * the replicate bit is clear -- that is NV2A behaviour, not a typo. */
static void emit_input(Emit *e, uint32_t code, int rgb)
{
    unsigned reg = code & 15;
    const char *swizzle = rgb ? ((code & 16) ? ".aaa" : ".rgb")
                              : ((code & 16) ? ".a"   : ".b");
    char x[32];
    if (reg > 13) reg = 0;
    snprintf(x, sizeof(x), "r%u%s", reg, swizzle);
    switch ((code >> 5) & 7) {
    case 0: emit(e, "max(0.0,%s)", x); break;
    case 1: emit(e, "(1.0-min(1.0,max(0.0,%s)))", x); break;
    case 2: emit(e, "(2.0*max(0.0,%s)-1.0)", x); break;
    case 3: emit(e, "(1.0-2.0*max(0.0,%s))", x); break;
    case 4: emit(e, "(max(0.0,%s)-0.5)", x); break;
    case 5: emit(e, "(0.5-max(0.0,%s))", x); break;
    case 6: emit(e, "(%s)", x); break;
    default: emit(e, "(-%s)", x); break;
    }
}

/* The three destinations of one channel of one stage, in the Metal shader's
 * order: the CD product, then AB, then their sum. The order is not cosmetic --
 * two of them may name the same register. */
static void emit_outputs(Emit *e, uint32_t word, const char *swizzle,
                         const char *ab, const char *cd)
{
    unsigned dst[3] = { word & 15u, (word >> 4) & 15u, (word >> 8) & 15u };
    const char *value[3];
    char sum[64];
    unsigned k;
    snprintf(sum, sizeof(sum), "%s+%s", ab, cd);
    value[0] = cd; value[1] = ab; value[2] = sum;
    for (k = 0; k < 3; k++) {
        if (!dst[k] || dst[k] > 13) continue;
        emit(e, "    r%u%s = clamp(%s,-1.0,1.0);\n", dst[k], swizzle, value[k]);
    }
}

static int emit_pixel_shader(const ShaderKey *k, char *buf, size_t size)
{
    Emit e = { buf, buf + size, 0 };
    unsigned u, stage;

    for (u = 0; u < 4; u++)
        if (k->texture_mask & (1u << u))
            emit(&e, "Texture2D tex%u:register(t%u); SamplerState smp%u:register(s%u);\n",
                 u, u, u, u);
    emit(&e, "%s", pixel_preamble);
    emit(&e, "float4 ps_main(VSOut i) : SV_TARGET {\n");
    emit(&e, "    float4 c = float4(1,1,1,1);\n");
    for (u = 0; u < 4; u++) {
        if (k->texture_mask & (1u << u))
            emit(&e, "    float4 t%u = tex%u.Sample(smp%u, i.t%u.xy / i.t%u.w);\n",
                 u, u, u, u, u);
        else
            emit(&e, "    float4 t%u = float4(0,0,0,0);\n", u);
    }
    if (k->combiner_count) {
        emit(&e, "    float4 r0=0,r1=0,r2=0,r3=0,r4=i.d0,r5=i.d1,r6=0,r7=0;\n");
        emit(&e, "    float4 r8=t0,r9=t1,r10=t2,r11=t3,r12=0,r13=0;\n");
        /* The spare register's alpha starts as texture 0's, and as one when no
         * texture is bound; its colour starts at zero. */
        emit(&e, "    r12.a = %s;\n", (k->texture_mask & 1) ? "r8.a" : "1.0");
        for (stage = 0; stage < k->combiner_count && stage < 8; stage++) {
            emit(&e, "    {\n        float3 ab = ");
            emit_input(&e, k->cicw[stage] >> 24, 1);
            emit(&e, " * ");
            emit_input(&e, (k->cicw[stage] >> 16) & 255, 1);
            emit(&e, ";\n        float3 cd = ");
            emit_input(&e, (k->cicw[stage] >> 8) & 255, 1);
            emit(&e, " * ");
            emit_input(&e, k->cicw[stage] & 255, 1);
            emit(&e, ";\n        float aa = ");
            emit_input(&e, k->aicw[stage] >> 24, 0);
            emit(&e, " * ");
            emit_input(&e, (k->aicw[stage] >> 16) & 255, 0);
            emit(&e, ";\n        float ad = ");
            emit_input(&e, (k->aicw[stage] >> 8) & 255, 0);
            emit(&e, " * ");
            emit_input(&e, k->aicw[stage] & 255, 0);
            emit(&e, ";\n");
            emit_outputs(&e, k->cocw[stage], ".rgb", "ab", "cd");
            emit_outputs(&e, k->aocw[stage], ".a", "aa", "ad");
            emit(&e, "    }\n");
        }
        emit(&e, "    c = saturate(r12%s);\n",
             k->add_specular ? " + float4(r5.rgb,0)" : "");
    } else if (!k->untextured) {
        emit(&e, "    c = t0;\n");
        emit(&e, "    c.a = saturate(i.d0.a)%s;\n", k->modulate ? " * c.a" : "");
        if (k->modulate) emit(&e, "    c.rgb *= max(0.0, i.d0.rgb);\n");
    }
    if (k->alpha_test)
        emit(&e, "    if ((uint)(saturate(c.a) * 255.0 + 0.5) <= pfrag.x) discard;\n");
    if (k->dither) {
        /* Ordered quantisation towards RGB565, which is what the readback
         * writes. Metal dithers after blending; here the blend is
         * fixed-function and happens downstream, so the bias lands one step
         * earlier. Both are bounded by half a 565 step, and neither has been
         * verified against NV2A hardware. */
        emit(&e, "    {\n"
                 "        const float bayer[16] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};\n"
                 "        int2 xy = int2(i.p.xy);\n"
                 "        float bias = (bayer[(xy.y & 3) * 4 + (xy.x & 3)] + 0.5) / 16.0 - 0.5;\n"
                 "        c.rgb += bias / float3(31.0, 63.0, 31.0);\n"
                 "    }\n");
    }
    emit(&e, "    return c;\n}\n");
    return !e.overflow;
}

/* ================================================================
 * Device objects
 * ================================================================ */
static ID3D11Device *device;
static ID3D11DeviceContext *context;
static ID3D11VertexShader *vertex_shader;
static ID3D11PixelShader *clear_pixel_shader;
static ID3D11InputLayout *input_layout;
static int pipeline_ready;
static ID3D11RasterizerState *rasterizer;
static ID3D11Buffer *vs_constants, *ps_constants, *vertex_buffer, *index_buffer;
static ID3D11Query *drain_query;
static unsigned vertex_capacity, index_capacity;
static int attempted;
static const char *reject_reason;

/* One retained GPU surface PER GUEST RENDER TARGET, not one globally.
 *
 * Measured, and it is the whole reason this is a set: JSRF alternates its
 * render target every single batch -- 0x6F0000, 0x788000 and 0x81E000 in the
 * first twelve batches of a run, never twice in a row for long. A single
 * retained surface is therefore thrown away on every batch, which means a
 * 640x480 upload and a 640x480 read-back per batch, 2,980 of each to guest
 * clock 1000. That is the GPU->CPU copy per draw the design note warned would
 * be slower than the software rasteriser it replaces, and worse than slow: the
 * round trip races the guest, which writes and clears the same framebuffer
 * from its own thread, so the composed frame came back black while every
 * individual batch was bit-exact. Keyed properly, a ping-pong costs a bind and
 * the read-back happens where it was always meant to, at the flip. */
typedef struct {
    ID3D11Texture2D *tex, *stage;        /* colour, and its staging copy */
    ID3D11Texture2D *ztex, *zstage;      /* depth/stencil, and its staging copy */
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    uint8_t *ram, *zram;                 /* where in guest RAM each one lives */
    size_t ram_size, zram_size;
    uint32_t w, h, row, zrow;
    int colour_ready, depth_ready;       /* holds the guest's current content */
    int colour_pending, depth_pending;   /* drawn, not yet given back */
    /* What this surface currently holds in the guest-memory ownership map.
     * The armed range is remembered separately from ram/zram because a
     * surface can be rebound to a different guest address while armed, and
     * the release has to undo exactly the hold that was taken. */
    const uint8_t *own_ram, *own_zram;
    size_t own_ram_size, own_zram_size;
    uint64_t stamp;
} Surface;
#define SURFACE_CACHE_SIZE 4
static Surface surfaces[SURFACE_CACHE_SIZE];
static Surface *bound;
static uint64_t surface_clock, surface_evictions;

/* The bound surface's fields under the names the rest of the file already
 * uses, so the read-back and upload code reads the same either way. Field
 * names are deliberately different from these, or the member accesses in the
 * cache lookup below would expand too. */
#define color_surface       (bound->tex)
#define color_staging       (bound->stage)
#define depth_surface       (bound->ztex)
#define depth_staging       (bound->zstage)
#define color_view          (bound->rtv)
#define depth_view          (bound->dsv)
#define surface_target      (bound->ram)
#define depth_target        (bound->zram)
#define surface_target_size (bound->ram_size)
#define depth_target_size   (bound->zram_size)
#define surface_width       (bound->w)
#define surface_height      (bound->h)
#define surface_pitch       (bound->row)
#define surface_depth_pitch (bound->zrow)   /* not depth_pitch: NV2ATextureCopy has one */
#define surface_valid       (bound->colour_ready)
#define depth_valid         (bound->depth_ready)
#define surface_dirty       (bound->colour_pending)
#define depth_dirty         (bound->depth_pending)

static uint64_t texture_requests, texture_hits, texture_uploads;
static uint64_t draw_batches, drawn_triangles;
/* Read-back instrumentation. A batch count says the GPU was asked to draw; it
 * does not say a pixel ever reached guest RAM, and those are different
 * failures with different causes. */
static uint64_t sync_calls, sync_colour_writebacks, sync_failures;
static uint64_t sync_nonblack_pixels, uploads_cleared, uploads_copied;
static uint64_t range_syncs, gpu_colour_clears, gpu_depth_clears;
static int trace_surfaces;

static int resident_clears_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("RECOMP_D3D11_RESIDENT_CLEARS") ? 1 : 0;
    return enabled;
}

/* One ordered line per clear, draw, flush and flip.
 *
 * Counts said the pixels were written and then were not there, which is not a
 * sequence anyone can reason about. This is the sequence. */
int nv2a_d3d11_event_trace(void)
{
    static long budget = -1;
    if (budget < 0) {
        const char *e = getenv("RECOMP_D3D11_EVENTS");
        budget = e ? strtol(e, NULL, 0) : 0;
    }
    if (budget <= 0) return 0;
    --budget;
    return 1;
}

uint32_t nv2a_d3d11_guest_offset(const void *host)
{
    extern ptrdiff_t xbox_GetMemoryOffset(void);
    const uint8_t *base = (const uint8_t *)xbox_GetMemoryOffset();
    return (base && host) ? (uint32_t)((const uint8_t *)host - base) : 0u;
}

typedef struct { float p[4], d0[4], d1[4], t[4][4]; } Vertex;
typedef struct { float viewport[4]; float texscale[4][4]; } VSParams;
typedef struct { uint32_t frag[4]; } PSParams;   /* x: alpha reference */

static int ensure_dynamic(ID3D11Buffer **buffer, unsigned *capacity,
                          unsigned bytes, UINT bind);
static int write_dynamic(ID3D11Buffer *buffer, const void *data, size_t bytes);
static int sync_range_inner(uint8_t *target, size_t bytes);
static int invalidate_range_inner(uint8_t *target, size_t bytes);
static void publish_ownership(void);

#define RELEASE(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

/* One writer at a time.
 *
 * Everything here used to be reached from whichever guest thread was pushing
 * the command stream, and a D3D11 immediate context has never been safe to
 * share. The ownership map adds a second kind of caller -- any guest thread
 * that happens to load from a resident surface -- so the entry points now
 * serialise. It is a leaf lock: nothing inside this file waits on anything
 * else while holding it, and the internal callers below take the unlocked
 * inner forms, so the pusher cannot deadlock against itself through
 * texture_view() or the reconcile path. */
static volatile long device_lock;
static volatile unsigned long device_owner;   /* thread id, never 0 when held */
static unsigned device_depth;

/* Recursive on purpose.
 *
 * The first version was not, and it hung on the first draw the renderer
 * rejected: reject() invalidates every surface, and the invalidate entry point
 * is public. Splitting every entry point into a locked outer and an unlocked
 * inner is still worth doing -- it keeps the reconciliation from running twice
 * per call -- but it is not a property this file can be relied on to preserve
 * as it grows, and the failure mode is a spin at 100% with no output. Counting
 * re-entry costs one thread-id compare and makes the mistake harmless. */
static void device_acquire(void)
{
    unsigned long self = GetCurrentThreadId();
    if (device_owner == self) { ++device_depth; return; }
    while (__atomic_exchange_n(&device_lock, 1L, __ATOMIC_ACQUIRE))
        SwitchToThread();
    device_owner = self;
    device_depth = 1;
}

static void device_release(void)
{
    if (--device_depth) return;
    device_owner = 0;
    __atomic_store_n(&device_lock, 0L, __ATOMIC_RELEASE);
}

const char *nv2a_d3d11_last_reject(void) { return reject_reason ? reject_reason : "none"; }

void nv2a_d3d11_report(void)
{
    fprintf(stderr, "[D3D11] textures: %llu requests, %llu cache hits, %llu uploads;"
            " %llu batches, %llu triangles; %u fragment programs (%u evicted)\n",
            (unsigned long long)texture_requests, (unsigned long long)texture_hits,
            (unsigned long long)texture_uploads, (unsigned long long)draw_batches,
            (unsigned long long)drawn_triangles, shader_used, shader_evictions);
    fprintf(stderr, "[D3D11] read-back: %llu syncs, %llu wrote colour, %llu failed;"
            " %llu non-black pixels; uploads %llu cleared / %llu copied\n",
            (unsigned long long)sync_calls, (unsigned long long)sync_colour_writebacks,
            (unsigned long long)sync_failures, (unsigned long long)sync_nonblack_pixels,
            (unsigned long long)uploads_cleared, (unsigned long long)uploads_copied);
    fprintf(stderr, "[D3D11] surfaces: %llu binds, %llu evictions\n",
            (unsigned long long)surface_clock, (unsigned long long)surface_evictions);
    fprintf(stderr, "[D3D11] coherence: %llu range syncs; %llu colour / %llu depth clears stayed on GPU\n",
            (unsigned long long)range_syncs,
            (unsigned long long)gpu_colour_clears,
            (unsigned long long)gpu_depth_clears);
    recomp_gpu_own_report();
}

/* ================================================================
 * Initialization
 * ================================================================ */
static int compile(const char *source, const char *entry, const char *profile,
                   ID3DBlob **out)
{
    ID3DBlob *errors = NULL;
    HRESULT hr = D3DCompile(source, strlen(source), "nv2a_d3d11",
                            NULL, NULL, entry, profile,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] %s compile failed: 0x%08X %s\n--- HLSL ---\n%s\n",
                entry, (unsigned)hr,
                errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "",
                source);
        RELEASE(errors);
        return 0;
    }
    RELEASE(errors);
    return 1;
}

/* The fragment program for this batch, compiled once per distinct combiner
 * configuration. A configuration that fails to compile is remembered as failed
 * so the run does not try it again every batch. */
static ID3D11PixelShader *pixel_shader_for(const NV2ATextureCopy *s)
{
    char hlsl[16384];
    ShaderKey key;
    ShaderEntry *slot;
    ID3DBlob *code = NULL;
    ID3D11PixelShader *shader = NULL;
    unsigned i;

    shader_key(s, &key);
    for (i = 0; i < shader_used; i++)
        if (!memcmp(&shader_cache[i].key, &key, sizeof(key)))
            return shader_cache[i].failed ? NULL : shader_cache[i].shader;
    if (shader_used >= SHADER_CACHE_SIZE) {
        /* Nothing clever: the table is sized well above the handful of
         * configurations a title actually uses, and if that is ever wrong the
         * counter says so rather than the frame rate saying it. */
        ++shader_evictions;
        return NULL;
    }
    if (!emit_pixel_shader(&key, hlsl, sizeof(hlsl))) {
        fprintf(stderr, "[D3D11] fragment program did not fit in %u bytes\n",
                (unsigned)sizeof(hlsl));
        slot = &shader_cache[shader_used++];
        slot->key = key;
        slot->failed = 1;
        return NULL;
    }
    slot = &shader_cache[shader_used++];
    slot->key = key;
    if (!compile(hlsl, "ps_main", "ps_4_0", &code)) { slot->failed = 1; return NULL; }
    if (FAILED(ID3D11Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(code),
            ID3D10Blob_GetBufferSize(code), NULL, &shader))) {
        RELEASE(code);
        slot->failed = 1;
        return NULL;
    }
    RELEASE(code);
    slot->shader = shader;
    return shader;
}

static int build_shaders(void)
{
    static const D3D11_INPUT_ELEMENT_DESC elements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 80, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 96, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3DBlob *vs = NULL, *ps = NULL;
    HRESULT hr;

    if (!compile(vertex_source, "vs_main", "vs_4_0", &vs)) return 0;
    hr = ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(vs),
            ID3D10Blob_GetBufferSize(vs), NULL, &vertex_shader);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateInputLayout(device, elements, 7,
                ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), &input_layout);
    RELEASE(vs);
    if (SUCCEEDED(hr) && compile(clear_pixel_source, "ps_clear", "ps_4_0", &ps)) {
        hr = ID3D11Device_CreatePixelShader(device,
                ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps),
                NULL, &clear_pixel_shader);
        RELEASE(ps);
    } else if (SUCCEEDED(hr)) {
        hr = E_FAIL;
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] shader objects failed: 0x%08X\n", (unsigned)hr);
        return 0;
    }
    return 1;
}

static ID3D11Buffer *make_constant_buffer(unsigned bytes)
{
    D3D11_BUFFER_DESC desc;
    ID3D11Buffer *buffer = NULL;
    memset(&desc, 0, sizeof(desc));
    desc.ByteWidth = (bytes + 15) & ~15u;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(device, &desc, NULL, &buffer))) return NULL;
    return buffer;
}

static int initialize(void)
{
    D3D11_RASTERIZER_DESC rd;

    if (attempted) return pipeline_ready;
    attempted = 1;
    device = d3d8_GetD3D11Device();
    context = d3d8_GetD3D11Context();
    if (!device || !context) {
        fprintf(stderr, "[D3D11] no device; the D3D8 layer has not been brought up\n");
        return 0;
    }
    if (!build_shaders()) return 0;

    vs_constants = make_constant_buffer(sizeof(VSParams));
    ps_constants = make_constant_buffer(sizeof(PSParams));
    if (!vs_constants || !ps_constants) {
        fprintf(stderr, "[D3D11] constant buffers failed\n");
        return 0;
    }
    /* Facing is decided on the CPU, exactly as the Metal path decides it, so
     * that both hosts cull the same triangles from the same screen-space
     * areas. Leaving it to the rasteriser would reverse with the Y flip. */
    memset(&rd, 0, sizeof(rd));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(device, &rd, &rasterizer))) {
        fprintf(stderr, "[D3D11] rasterizer state failed\n");
        return 0;
    }
    {
        /* The fence the read-back waits on.
         *
         * nv2a_metal.m waits for its command buffer before reading the surface
         * back; this had nothing equivalent, and relied on Map() of a staging
         * copy to imply the GPU had finished. Measured against JSRF, it does
         * not: the copy returned the surface as it stood BEFORE the queued
         * draws ran, so every read-back wrote the pre-draw contents to guest
         * RAM, cleared its own dirty flag, and left the finished pixels
         * stranded on a surface nothing would flush again. The frame was black
         * while the GPU held a picture. */
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof(qd));
        qd.Query = D3D11_QUERY_EVENT;
        if (FAILED(ID3D11Device_CreateQuery(device, &qd, &drain_query))) {
            fprintf(stderr, "[D3D11] event query failed\n");
            return 0;
        }
    }
    fprintf(stderr, "[D3D11] native raster pipeline ready\n");
    pipeline_ready = 1;
    return 1;
}

/* ================================================================
 * Pipeline state caches
 * ================================================================ */
static D3D11_COMPARISON_FUNC depth_comparison(uint32_t f)
{
    if (!f) f = 0x203;                       /* the CPU path's LEQUAL default */
    if (f < 0x200 || f > 0x207) return D3D11_COMPARISON_ALWAYS;
    return (D3D11_COMPARISON_FUNC)(f - 0x200 + 1);
}
static D3D11_COMPARISON_FUNC stencil_comparison(uint32_t f)
{
    if (f < 0x200 || f > 0x207) return D3D11_COMPARISON_ALWAYS;
    return (D3D11_COMPARISON_FUNC)(f - 0x200 + 1);
}
static D3D11_STENCIL_OP stencil_operation(uint32_t op)
{
    switch (op) {
    case 0:      return D3D11_STENCIL_OP_ZERO;
    case 0x1e01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1e02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1e03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150a: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}
/* Keep in step with blend_factor in nv2a_texture_copy.c and bfactor in
 * nv2a_metal.m: the same accept test admits all three, so a factor missing
 * here would render as ONE_MINUS_SRC_ALPHA on Windows and correctly on macOS,
 * which is the kind of divergence the differential oracle exists to catch. */
static D3D11_BLEND blend_factor(uint32_t f)
{
    switch (f) {
    case 0x000:  return D3D11_BLEND_ZERO;
    case 0x001:  return D3D11_BLEND_ONE;
    case 0x300:  return D3D11_BLEND_SRC_COLOR;
    case 0x301:  return D3D11_BLEND_INV_SRC_COLOR;
    case 0x302:  return D3D11_BLEND_SRC_ALPHA;
    case 0x303:  return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x306:  return D3D11_BLEND_DEST_COLOR;
    case 0x307:  return D3D11_BLEND_INV_DEST_COLOR;
    default:     return D3D11_BLEND_ZERO;
    }
}

#define STATE_CACHE_SIZE 32
typedef struct { uint32_t key[3]; ID3D11BlendState *state; } BlendEntry;
typedef struct { uint32_t key[11]; ID3D11DepthStencilState *state; } DepthEntry;
static BlendEntry blend_cache[STATE_CACHE_SIZE];
static DepthEntry depth_cache[STATE_CACHE_SIZE];
static unsigned blend_used, depth_used;

static ID3D11BlendState *blend_state(const NV2ATextureCopy *s)
{
    uint32_t key[3] = { s->blend, s->blend_src, s->blend_dst };
    D3D11_BLEND_DESC desc;
    unsigned i;
    ID3D11BlendState *state = NULL;

    for (i = 0; i < blend_used; i++)
        if (!memcmp(blend_cache[i].key, key, sizeof(key))) return blend_cache[i].state;
    memset(&desc, 0, sizeof(desc));
    desc.RenderTarget[0].BlendEnable = s->blend ? TRUE : FALSE;
    desc.RenderTarget[0].SrcBlend  = blend_factor(s->blend_src);
    desc.RenderTarget[0].DestBlend = blend_factor(s->blend_dst);
    desc.RenderTarget[0].BlendOp   = D3D11_BLEND_OP_ADD;
    /* The Metal shader applies the source-alpha factors to all four channels;
     * nothing reads destination alpha back, so this only keeps them equal. */
    desc.RenderTarget[0].SrcBlendAlpha  = desc.RenderTarget[0].SrcBlend;
    desc.RenderTarget[0].DestBlendAlpha = desc.RenderTarget[0].DestBlend;
    desc.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(ID3D11Device_CreateBlendState(device, &desc, &state))) return NULL;
    if (blend_used < STATE_CACHE_SIZE) {
        memcpy(blend_cache[blend_used].key, key, sizeof(key));
        blend_cache[blend_used++].state = state;
    }
    return state;
}

static ID3D11DepthStencilState *depth_state(const NV2ATextureCopy *s)
{
    uint32_t key[11] = { s->depth_test, s->depth_write, s->depth_func,
        s->stencil_test, s->stencil_write, s->stencil_mask, s->stencil_func_mask,
        s->stencil_func, s->stencil_fail, s->stencil_zfail, s->stencil_zpass };
    D3D11_DEPTH_STENCIL_DESC desc;
    unsigned i;
    ID3D11DepthStencilState *state = NULL;

    for (i = 0; i < depth_used; i++)
        if (!memcmp(depth_cache[i].key, key, sizeof(key))) return depth_cache[i].state;
    memset(&desc, 0, sizeof(desc));
    desc.DepthEnable = s->depth_test ? TRUE : FALSE;
    desc.DepthWriteMask = (s->depth_test && s->depth_write)
        ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc = depth_comparison(s->depth_func);
    desc.StencilEnable = s->stencil_test ? TRUE : FALSE;
    desc.StencilReadMask = (UINT8)(s->stencil_func_mask & 255);
    desc.StencilWriteMask = s->stencil_write ? (UINT8)(s->stencil_mask & 255) : 0;
    desc.FrontFace.StencilFunc = stencil_comparison(s->stencil_func);
    desc.FrontFace.StencilFailOp = stencil_operation(s->stencil_fail);
    desc.FrontFace.StencilDepthFailOp = stencil_operation(s->stencil_zfail);
    desc.FrontFace.StencilPassOp = stencil_operation(s->stencil_zpass);
    desc.BackFace = desc.FrontFace;
    if (FAILED(ID3D11Device_CreateDepthStencilState(device, &desc, &state))) return NULL;
    if (depth_used < STATE_CACHE_SIZE) {
        memcpy(depth_cache[depth_used].key, key, sizeof(key));
        depth_cache[depth_used++].state = state;
    }
    return state;
}

typedef struct { uint32_t key[4]; float bias; ID3D11SamplerState *state; } SamplerEntry;
static SamplerEntry sampler_cache[STATE_CACHE_SIZE];
static unsigned sampler_used;

/* NV2A min filter 1..6 is the GL ladder: odd values take the nearest texel,
 * values from 3 up consult the mip chain, and 5 and 6 interpolate between two
 * levels. The hardware sampler does all of it, so the shader has no LOD math. */
static ID3D11SamplerState *sampler_state(const NV2ATextureCopy *s)
{
    uint32_t key[4] = { s->linear, s->min_filter, s->repeat, s->levels };
    D3D11_SAMPLER_DESC desc;
    unsigned i;
    unsigned filter = 0;
    ID3D11SamplerState *state = NULL;

    for (i = 0; i < sampler_used; i++)
        if (!memcmp(sampler_cache[i].key, key, sizeof(key))
                && sampler_cache[i].bias == s->lod_bias) return sampler_cache[i].state;
    if ((s->min_filter & 1) == 0) filter |= 0x10;         /* minification linear */
    if (s->linear) filter |= 0x04;                        /* magnification linear */
    if (s->min_filter >= 5) filter |= 0x01;               /* linear between levels */
    memset(&desc, 0, sizeof(desc));
    desc.Filter = (D3D11_FILTER)filter;
    desc.AddressU = desc.AddressV = desc.AddressW = s->repeat
        ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.MipLODBias = s->lod_bias;
    desc.MaxAnisotropy = 1;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = (s->min_filter >= 3 && s->levels > 1) ? (float)(s->levels - 1) : 0.0f;
    if (FAILED(ID3D11Device_CreateSamplerState(device, &desc, &state))) return NULL;
    if (sampler_used < STATE_CACHE_SIZE) {
        memcpy(sampler_cache[sampler_used].key, key, sizeof(key));
        sampler_cache[sampler_used].bias = s->lod_bias;
        sampler_cache[sampler_used++].state = state;
    }
    return state;
}

/* ================================================================
 * Texture cache
 * ================================================================ */
#define TEXTURE_CACHE_SIZE 128
#define TEXTURE_DECODE_LIMIT (2048u * 2048u)
typedef struct {
    const uint8_t *source;
    size_t size;
    uint8_t *shadow;                     /* the bytes the upload was made from */
    ID3D11Texture2D *texture;
    ID3D11ShaderResourceView *view;
    uint64_t stamp;
} TextureEntry;
static TextureEntry texture_cache[TEXTURE_CACHE_SIZE];
static ID3D11ShaderResourceView *dummy_view;
static uint64_t texture_clock;

static ID3D11ShaderResourceView *upload_texture(const NV2ATextureCopy *t,
        const uint8_t *data, size_t bytes, TextureEntry *slot)
{
    D3D11_SUBRESOURCE_DATA levels[13];
    D3D11_TEXTURE2D_DESC desc;
    D3D11_SHADER_RESOURCE_VIEW_DESC view;
    ID3D11Texture2D *texture = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    unsigned count = t->levels ? t->levels : 1, level;
    size_t total = 0, offset = 0;
    uint8_t *decoded;
    unsigned w = t->width, h = t->height;

    for (level = 0; level < count; level++) {
        total += (size_t)w * h * 4;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    decoded = (uint8_t *)malloc(total);
    if (!decoded) return NULL;
    for (level = 0; level < count; level++) {
        unsigned lw = 0, lh = 0;
        if (!nv2a_texture_copy_decode_level(t, data, bytes, level,
                decoded + offset, total - offset, &lw, &lh)) { free(decoded); return NULL; }
        levels[level].pSysMem = decoded + offset;
        levels[level].SysMemPitch = lw * 4;
        levels[level].SysMemSlicePitch = 0;
        offset += (size_t)lw * lh * 4;
    }
    memset(&desc, 0, sizeof(desc));
    desc.Width = t->width;
    desc.Height = t->height;
    desc.MipLevels = count;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, levels, &texture))) {
        free(decoded);
        return NULL;
    }
    free(decoded);
    memset(&view, 0, sizeof(view));
    view.Format = desc.Format;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = count;
    if (FAILED(ID3D11Device_CreateShaderResourceView(device,
            (ID3D11Resource *)texture, &view, &srv))) {
        RELEASE(texture);
        return NULL;
    }
    {
        uint8_t *shadow = (uint8_t *)malloc(bytes ? bytes : 1);
        if (!shadow) { RELEASE(srv); RELEASE(texture); return NULL; }
        memcpy(shadow, data, bytes);
        RELEASE(slot->view);
        RELEASE(slot->texture);
        free(slot->shadow);
        slot->shadow = shadow;
    }
    slot->source = data;
    slot->size = bytes;
    slot->texture = texture;
    slot->view = srv;
    slot->stamp = ++texture_clock;
    ++texture_uploads;
    return srv;
}

/* A view for an unbound unit. The shader never samples it, but D3D11 wants
 * something valid in the slot. */
static ID3D11ShaderResourceView *empty_view(void)
{
    D3D11_TEXTURE2D_DESC desc;
    D3D11_SUBRESOURCE_DATA initial;
    ID3D11Texture2D *texture = NULL;
    static const uint32_t pixel = 0;

    if (dummy_view) return dummy_view;
    memset(&desc, 0, sizeof(desc));
    desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    initial.pSysMem = &pixel;
    initial.SysMemPitch = 4;
    initial.SysMemSlicePitch = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, &initial, &texture))) return NULL;
    if (FAILED(ID3D11Device_CreateShaderResourceView(device,
            (ID3D11Resource *)texture, NULL, &dummy_view))) {
        RELEASE(texture);
        return NULL;
    }
    RELEASE(texture);
    return dummy_view;
}

static ID3D11ShaderResourceView *texture_view(const NV2ATextureCopy *t,
        const uint8_t *data, size_t bytes)
{
    TextureEntry *slot = NULL, *oldest = &texture_cache[0];
    unsigned i;

    if (!data || !bytes) return empty_view();
    /* Guest RAM may name a render target that is still authoritative on the
     * GPU. Materialise only that range before hashing/decoding the texture;
     * unrelated cached targets remain resident. A future surface-as-texture
     * view can remove even this necessary copy. */
    if (!sync_range_inner((uint8_t *)data, bytes)) return NULL;
    ++texture_requests;
    for (i = 0; i < TEXTURE_CACHE_SIZE; i++) {
        TextureEntry *entry = &texture_cache[i];
        if (entry->source == data && entry->size == bytes) {
            slot = entry;
            if (entry->view && entry->shadow && !memcmp(entry->shadow, data, bytes)) {
                entry->stamp = ++texture_clock;
                ++texture_hits;
                return entry->view;
            }
            break;
        }
        if (!entry->view) { if (!slot) slot = entry; }
        else if (entry->stamp < oldest->stamp) oldest = entry;
    }
    if (!slot) slot = oldest;
    return upload_texture(t, data, bytes, slot);
}

/* ================================================================
 * Render surfaces
 * ================================================================ */
static void release_surfaces(void)
{
    if (!bound) return;
    RELEASE(color_view);
    RELEASE(depth_view);
    RELEASE(color_surface);
    RELEASE(color_staging);
    RELEASE(depth_surface);
    RELEASE(depth_staging);
}

static int create_surfaces(uint32_t w, uint32_t h)
{
    D3D11_TEXTURE2D_DESC desc;
    HRESULT hr;

    release_surfaces();
    memset(&desc, 0, sizeof(desc));
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &color_surface);
    if (SUCCEEDED(hr)) {
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
        hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &color_staging);
    }
    if (SUCCEEDED(hr)) {
        desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        desc.CPUAccessFlags = 0;
        hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &depth_surface);
    }
    if (SUCCEEDED(hr)) {
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
        hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &depth_staging);
    }
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateRenderTargetView(device,
                (ID3D11Resource *)color_surface, NULL, &color_view);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateDepthStencilView(device,
                (ID3D11Resource *)depth_surface, NULL, &depth_view);
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] surface allocation failed: 0x%08X\n", (unsigned)hr);
        release_surfaces();
        return 0;
    }
    return 1;
}

/* Flush one retained surface to guest RAM. */
static int sync_surface(Surface *sf)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    Surface *previous = bound;
    unsigned x, y, wrote_now = 0;
    int result = 1;

    bound = sf;
    ++sync_calls;
    if (!surface_dirty && !depth_dirty) { bound = previous; return 1; }
    /* Drain first. Everything below reads what the GPU has actually done, and
     * until this returns the queued draws may not have happened. */
    if (drain_query) {
        BOOL done = FALSE;
        ID3D11DeviceContext_End(context, (ID3D11Asynchronous *)drain_query);
        ID3D11DeviceContext_Flush(context);
        while (ID3D11DeviceContext_GetData(context,
                    (ID3D11Asynchronous *)drain_query, &done, sizeof(done), 0) != S_OK)
            ;
    }
    if (surface_dirty && surface_target) {
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)color_staging,
                                         (ID3D11Resource *)color_surface);
        if (FAILED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)color_staging,
                0, D3D11_MAP_READ, 0, &mapped))) { ++sync_failures; bound = previous; return 0; }
        wrote_now = 0;
        for (y = 0; y < surface_height; ++y) {
            const uint8_t *row = (const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch;
            uint8_t *out = surface_target + (size_t)y * surface_pitch;
            for (x = 0; x < surface_width; ++x) {
                unsigned c = (unsigned)(row[x * 4 + 0] >> 3) << 11
                           | (unsigned)(row[x * 4 + 1] >> 2) << 5
                           | (unsigned)(row[x * 4 + 2] >> 3);
                out[x * 2] = (uint8_t)c;
                out[x * 2 + 1] = (uint8_t)(c >> 8);
                if (c) { ++sync_nonblack_pixels; ++wrote_now; }
            }
        }
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)color_staging, 0);
        if (trace_surfaces)
            fprintf(stderr, "[D3D11-TRACE]   writeback target=%p nonblack=%llu\n",
                    (void *)surface_target, (unsigned long long)sync_nonblack_pixels);
        if (nv2a_d3d11_event_trace())
            fprintf(stderr, "  [EV] FLUSH  target=%08X wrote %u non-black\n",
                    nv2a_d3d11_guest_offset(surface_target), wrote_now);
        ++sync_colour_writebacks;
        surface_dirty = 0;
    }
    if (depth_dirty && depth_target) {
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)depth_staging,
                                         (ID3D11Resource *)depth_surface);
        if (FAILED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)depth_staging,
                0, D3D11_MAP_READ, 0, &mapped))) { ++sync_failures; bound = previous; return 0; }
        for (y = 0; y < surface_height; ++y) {
            const uint32_t *row = (const uint32_t *)((const uint8_t *)mapped.pData
                                                     + (size_t)y * mapped.RowPitch);
            uint8_t *out = depth_target + (size_t)y * surface_depth_pitch;
            for (x = 0; x < surface_width; ++x) {
                /* D24S8 keeps depth low and stencil high; the guest's Z24S8
                 * keeps stencil in byte 0 and depth in bytes 1..3. */
                uint32_t v = row[x];
                uint32_t q = v & 0x00ffffffu;
                out[x * 4 + 0] = (uint8_t)(v >> 24);
                out[x * 4 + 1] = (uint8_t)q;
                out[x * 4 + 2] = (uint8_t)(q >> 8);
                out[x * 4 + 3] = (uint8_t)(q >> 16);
            }
        }
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)depth_staging, 0);
        depth_dirty = 0;
    }
    bound = previous;
    return result;
}

/* What every retained surface holds, for the flip trace.
 *
 * Read-only and read-back: it copies each surface down and counts, and touches
 * no flag. Paired with the [FLIPTRACE] line's guest-RAM counts for the same
 * addresses it answers the one question the black frame turns on -- whether
 * the displayed buffer was ever drawn into, and if it was, whether what the
 * GPU holds ever reached the memory the presenter reads. */
void nv2a_d3d11_surface_report(void)
{
    extern ptrdiff_t xbox_GetMemoryOffset(void);
    const uint8_t *base = (const uint8_t *)xbox_GetMemoryOffset();
    char line[640];
    int off;
    unsigned i;

    if (!device || !context) return;
    device_acquire();
    off = snprintf(line, sizeof line, "  [FLIPTRACE]   d3d11 surfaces:");
    for (i = 0; i < SURFACE_CACHE_SIZE && off > 0 && off < (int)sizeof(line) - 64; i++) {
        Surface *sf = &surfaces[i];
        D3D11_MAPPED_SUBRESOURCE mapped;
        unsigned x, y, nonblack = 0;
        if (!sf->tex) continue;
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)sf->stage,
                                         (ID3D11Resource *)sf->tex);
        if (SUCCEEDED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)sf->stage,
                0, D3D11_MAP_READ, 0, &mapped))) {
            for (y = 0; y < sf->h; ++y) {
                const uint8_t *row = (const uint8_t *)mapped.pData
                                   + (size_t)y * mapped.RowPitch;
                for (x = 0; x < sf->w; ++x)
                    if (row[x*4] || row[x*4+1] || row[x*4+2]) ++nonblack;
            }
            ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)sf->stage, 0);
        }
        /* And the same surface read back through OUR pointer, so a
         * disagreement with the [FLIPTRACE] guest-RAM scan of the same guest
         * offset means the two are not the same storage -- which no amount of
         * arithmetic agreement can rule out on its own. */
        {
            unsigned ram = 0;
            for (y = 0; y < sf->h; ++y) {
                const uint8_t *r = sf->ram + (size_t)y * sf->row;
                for (x = 0; x < sf->w; ++x)
                    if (r[x*2] || r[x*2+1]) ++ram;
            }
            off += snprintf(line + off, sizeof(line) - (size_t)off,
                            " 0x%08X=gpu%u/ram%u(%s%s)",
                            base ? (unsigned)(sf->ram - base) : 0u, nonblack, ram,
                            (sf->colour_ready && (!sf->zram || sf->depth_ready))
                                ? "ready" : "stale",
                            sf->colour_pending ? ",unflushed" : "");
        }
    }
    device_release();
    fprintf(stderr, "%s\n", line);
}

int nv2a_d3d11_sync(void)
{
    int result;
    /* Every retained surface, because the caller means "guest RAM is about to
     * be read" and does not know which target that is. */
    device_acquire();
    result = sync_range_inner(NULL, 0);
    device_release();
    return result;
}

static int ranges_overlap(const uint8_t *a, size_t a_size,
                          const uint8_t *b, size_t b_size)
{
    uintptr_t av, bv;
    if (!a || !b || !a_size || !b_size) return 0;
    av = (uintptr_t)a;
    bv = (uintptr_t)b;
    /* Subtraction after ordering avoids end-pointer overflow. */
    return av <= bv ? bv - av < a_size : av - bv < b_size;
}

/* ================================================================
 * Guest-memory ownership
 * ================================================================
 *
 * Between a draw and its write-back the GPU's copy of a surface is the newer
 * one, and guest RAM holds whatever was there before. Every consumer inside
 * this runtime asks for the write-back at its own call site; translated guest
 * code cannot, because a static recompile emits an ordinary load. The map in
 * recomp_gpu_own.c is that missing observer, and this is the half that tells
 * it which guest bytes are currently owed pixels.
 *
 * Publishing is a reconciliation rather than a set of paired calls at each
 * flag assignment: colour_pending and depth_pending are written from eight
 * places across draw, upload, clear and sync, and a scheme that needed each of
 * them to remember to arm or disarm would be wrong the first time one was
 * added. Four surfaces make the sweep free.
 */
static int own_reconcile_range(void *host, size_t bytes);

static void publish_ownership(void)
{
    static int hooked;
    unsigned i;

    if (!hooked) {
        recomp_gpu_own_set_sync(own_reconcile_range);
        hooked = 1;
    }
    for (i = 0; i < SURFACE_CACHE_SIZE; i++) {
        Surface *sf = &surfaces[i];
        int owed = sf->tex && sf->colour_pending;
        const uint8_t *want = owed ? sf->ram : NULL;
        size_t want_size = owed ? sf->ram_size : 0;

        if (!want || !want_size) { want = NULL; want_size = 0; }
        if (sf->own_ram != want || sf->own_ram_size != want_size) {
            if (sf->own_ram)
                recomp_gpu_own_release(sf->own_ram, sf->own_ram_size);
            if (want) recomp_gpu_own_hold(want, want_size);
            sf->own_ram = want;
            sf->own_ram_size = want_size;
        }

        owed = sf->tex && sf->depth_pending;
        want = owed ? sf->zram : NULL;
        want_size = owed ? sf->zram_size : 0;
        if (!want || !want_size) { want = NULL; want_size = 0; }
        if (sf->own_zram != want || sf->own_zram_size != want_size) {
            if (sf->own_zram)
                recomp_gpu_own_release(sf->own_zram, sf->own_zram_size);
            if (want) recomp_gpu_own_hold(want, want_size);
            sf->own_zram = want;
            sf->own_zram_size = want_size;
        }
    }
}

static int sync_range_inner(uint8_t *target, size_t bytes)
{
    unsigned i;
    int result = 1, hit = 0;
    if (!target || !bytes) {
        for (i = 0; i < SURFACE_CACHE_SIZE; i++)
            if (surfaces[i].tex && !sync_surface(&surfaces[i])) result = 0;
        publish_ownership();
        return result;
    }
    for (i = 0; i < SURFACE_CACHE_SIZE; i++) {
        Surface *sf = &surfaces[i];
        if (!sf->tex) continue;
        if ((ranges_overlap(target, bytes, sf->ram, sf->ram_size)
                || ranges_overlap(target, bytes, sf->zram, sf->zram_size))) {
            hit = 1;
            if (!sync_surface(sf)) result = 0;
        }
    }
    if (hit) ++range_syncs;
    publish_ownership();
    return result;
}

/* Returns how many surfaces the range actually reached, so the ownership map
 * can separate a real reconciliation from a granule-granularity false hit. */
static int invalidate_range_inner(uint8_t *target, size_t bytes)
{
    unsigned i;
    int hits = 0;

    if (!target || !bytes) {
        (void)sync_range_inner(NULL, 0);
        for (i = 0; i < SURFACE_CACHE_SIZE; i++) {
            surfaces[i].colour_ready = 0;
            surfaces[i].depth_ready = 0;
        }
        publish_ownership();
        return SURFACE_CACHE_SIZE;
    }

    /* A CPU writer needs the previous GPU contents only for resources it can
     * overlap. Flush those resources before the write, then make only the
     * affected aspect upload again. This is the same ownership transition as
     * xemu's surface memory callback, expressed at our explicit write sites. */
    for (i = 0; i < SURFACE_CACHE_SIZE; i++) {
        Surface *sf = &surfaces[i];
        int colour = ranges_overlap(target, bytes, sf->ram, sf->ram_size);
        int depth = ranges_overlap(target, bytes, sf->zram, sf->zram_size);
        if (!sf->tex || (!colour && !depth)) continue;
        ++hits;
        (void)sync_surface(sf);
        if (colour) sf->colour_ready = 0;
        if (depth) sf->depth_ready = 0;
    }
    publish_ownership();
    return hits;
}

/* The ownership map's slow path.
 *
 * It reaches here from a translated guest access whose direction is unknown:
 * XBOX_PTR is shared by loads, by stores, and by the MEM32 lvalues the lifter
 * expands `rep movs` into, which never pass through the explicit store helper.
 * Downloading alone would be right for the loads and silently wrong for those
 * block stores -- a stale GPU surface would later be written back over them --
 * so the range is handed back completely: copied down, and marked for upload
 * again on the next draw that needs it. Separating the two directions is worth
 * doing, but it needs the block operations routed through a store seam first,
 * and the counters here are what will say whether it is worth the regeneration.
 */
static int own_reconcile_range(void *host, size_t bytes)
{
    int hits;
    device_acquire();
    hits = invalidate_range_inner((uint8_t *)host, bytes);
    device_release();
    return hits;
}

int nv2a_d3d11_sync_range(uint8_t *target, size_t bytes)
{
    int result;
    device_acquire();
    result = sync_range_inner(target, bytes);
    device_release();
    return result;
}

void nv2a_d3d11_invalidate_range(uint8_t *target, size_t bytes)
{
    device_acquire();
    (void)invalidate_range_inner(target, bytes);
    device_release();
}

void nv2a_d3d11_invalidate(uint8_t *target)
{
    nv2a_d3d11_invalidate_range(target, target ? 1 : 0);
}

static int clear_color_inner(uint8_t *target, size_t target_size,
        uint32_t pitch, uint32_t width, uint32_t height,
        uint32_t components, uint32_t value)
{
    float rgba[4];
    Vertex v[3];
    VSParams vsp;
    NV2ATextureCopy state;
    ID3D11BlendState *blend;
    ID3D11DepthStencilState *zstate;
    D3D11_VIEWPORT viewport;
    const float factor[4] = { 1, 1, 1, 1 };
    unsigned i;
    int handled = 0;
    uint16_t c = (uint16_t)value;

    /* A static recompile currently has no general guest-CPU read callback for
     * framebuffer pages. Keep the GPU-authoritative transition experimental
     * until those reads can demand a range sync; otherwise JSRF consumes stale
     * RAM after its first batch and stops advancing. */
    if (!resident_clears_enabled()) return 0;
    if (!device || !context || !target || !target_size || !width || !height)
        return 0;
    /* The retained target is RGB565. Alpha is discarded on download, but the
     * clear shader currently uses an all-channel blend state. Keep partial
     * RGB writes on the CPU until it has a write-mask variant. */
    if ((components & 0x70u) != 0x70u) return 0;
    rgba[0] = (float)(c >> 11) / 31.0f;
    rgba[1] = (float)((c >> 5) & 63) / 63.0f;
    rgba[2] = (float)(c & 31) / 31.0f;
    rgba[3] = 1.0f;

    memset(&state, 0, sizeof(state));
    state.untextured = 1;
    blend = blend_state(&state);
    zstate = depth_state(&state);
    if (!clear_pixel_shader || !blend || !zstate) return 0;

    memset(v, 0, sizeof(v));
    for (i = 0; i < 3; ++i) {
        v[i].p[3] = 1.0f;
        memcpy(v[i].d0, rgba, sizeof(rgba));
    }
    /* One oversized triangle avoids a diagonal seam and covers every pixel. */
    v[1].p[0] = (float)width * 2.0f;
    v[2].p[1] = (float)height * 2.0f;
    memset(&vsp, 0, sizeof(vsp));
    vsp.viewport[0] = (float)width;
    vsp.viewport[1] = (float)height;
    if (!ensure_dynamic(&vertex_buffer, &vertex_capacity, sizeof(v),
                        D3D11_BIND_VERTEX_BUFFER)
            || !write_dynamic(vertex_buffer, v, sizeof(v))
            || !write_dynamic(vs_constants, &vsp, sizeof(vsp))
            || !write_dynamic(ps_constants, rgba, sizeof(rgba)))
        return 0;

    for (i = 0; i < SURFACE_CACHE_SIZE; i++) {
        Surface *sf = &surfaces[i];
        if (!sf->tex || sf->ram != target || sf->ram_size != target_size
                || sf->row != pitch || sf->w != width || sf->h != height)
            continue;
        {
            UINT stride = sizeof(Vertex), offset = 0;
            ID3D11DeviceContext_IASetInputLayout(context, input_layout);
            ID3D11DeviceContext_IASetVertexBuffers(context, 0, 1,
                    &vertex_buffer, &stride, &offset);
            ID3D11DeviceContext_IASetPrimitiveTopology(context,
                    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        }
        ID3D11DeviceContext_VSSetShader(context, vertex_shader, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(context, 0, 1, &vs_constants);
        ID3D11DeviceContext_PSSetShader(context, clear_pixel_shader, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(context, 0, 1, &ps_constants);
        ID3D11DeviceContext_RSSetState(context, rasterizer);
        memset(&viewport, 0, sizeof(viewport));
        viewport.Width = (float)sf->w;
        viewport.Height = (float)sf->h;
        viewport.MaxDepth = 1.0f;
        ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
        ID3D11DeviceContext_OMSetBlendState(context, blend, factor, 0xffffffffu);
        ID3D11DeviceContext_OMSetDepthStencilState(context, zstate, 0);
        ID3D11DeviceContext_OMSetRenderTargets(context, 1, &sf->rtv, NULL);
        ID3D11DeviceContext_Draw(context, 3, 0);
        sf->colour_ready = 1;
        sf->colour_pending = 1;
        sf->stamp = ++surface_clock;
        handled = 1;
    }
    if (handled) ++gpu_colour_clears;
    publish_ownership();
    return handled;
}

int nv2a_d3d11_clear_color(uint8_t *target, size_t target_size,
        uint32_t pitch, uint32_t width, uint32_t height,
        uint32_t components, uint32_t value)
{
    int handled;
    device_acquire();
    handled = clear_color_inner(target, target_size, pitch, width, height,
                                components, value);
    device_release();
    return handled;
}

int nv2a_d3d11_clear_depth_stencil(uint8_t *target, size_t target_size,
        uint32_t pitch, uint32_t width, uint32_t height,
        uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
        uint32_t components, uint32_t value)
{
    /* CrossOver's mapped D24S8 staging representation does not agree with the
     * ClearDepthStencilView representation used here: 0x12345678 returns as
     * guest 0x34567812. Until depth uses a verified shader/resource format,
     * refuse the shortcut so the caller performs its byte-exact CPU clear. */
    (void)target;
    (void)target_size;
    (void)pitch;
    (void)width;
    (void)height;
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    (void)components;
    (void)value;
    return 0;
}

static int reject(const char *reason)
{
    reject_reason = reason;
    (void)invalidate_range_inner(NULL, 0);
    return -1;
}

/* Prime the render target from guest RAM.
 *
 * Every clear runs on the CPU into guest memory and invalidates us, so this is
 * the first thing each frame does, and a whole-surface staging upload per
 * frame is real cost. Immediately after a clear the rect holds one value, so
 * the common case collapses into a Clear call and the upload never happens. */
static int upload_surface(uint8_t *target, uint8_t *depth, uint32_t depth_row)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    unsigned x, y;
    int uniform = 1;
    uint32_t first;

    first = (uint32_t)target[0] | (uint32_t)target[1] << 8;
    for (y = 0; y < surface_height && uniform; ++y) {
        const uint8_t *row = target + (size_t)y * surface_pitch;
        for (x = 0; x < surface_width; ++x)
            if (((uint32_t)row[x * 2] | (uint32_t)row[x * 2 + 1] << 8) != first) {
                uniform = 0;
                break;
            }
    }
    if (trace_surfaces)
        fprintf(stderr, "[D3D11-TRACE]   upload target=%p uniform=%d first=%04X\n",
                (void *)target, uniform, first);
    if (uniform) {
        float rgba[4];
        rgba[0] = (float)(first >> 11) / 31.0f;
        rgba[1] = (float)((first >> 5) & 63) / 63.0f;
        rgba[2] = (float)(first & 31) / 31.0f;
        rgba[3] = 1.0f;
        ID3D11DeviceContext_ClearRenderTargetView(context, color_view, rgba);
        ++uploads_cleared;
    } else {
        ++uploads_copied;
        if (FAILED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)color_staging,
                0, D3D11_MAP_WRITE, 0, &mapped))) return 0;
        for (y = 0; y < surface_height; ++y) {
            const uint8_t *row = target + (size_t)y * surface_pitch;
            uint8_t *out = (uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch;
            for (x = 0; x < surface_width; ++x) {
                unsigned c = (unsigned)row[x * 2] | (unsigned)row[x * 2 + 1] << 8;
                unsigned r5 = c >> 11, g6 = (c >> 5) & 63, b5 = c & 31;
                out[x * 4 + 0] = (uint8_t)(r5 << 3 | r5 >> 2);
                out[x * 4 + 1] = (uint8_t)(g6 << 2 | g6 >> 4);
                out[x * 4 + 2] = (uint8_t)(b5 << 3 | b5 >> 2);
                out[x * 4 + 3] = 255;
            }
        }
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)color_staging, 0);
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)color_surface,
                                         (ID3D11Resource *)color_staging);
    }

    if (!depth) {
        ID3D11DeviceContext_ClearDepthStencilView(context, depth_view,
                D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        return 1;
    }
    uniform = 1;
    first = (uint32_t)depth[0] | (uint32_t)depth[1] << 8
          | (uint32_t)depth[2] << 16 | (uint32_t)depth[3] << 24;
    for (y = 0; y < surface_height && uniform; ++y) {
        const uint8_t *row = depth + (size_t)y * depth_row;
        for (x = 0; x < surface_width; ++x) {
            uint32_t v = (uint32_t)row[x * 4] | (uint32_t)row[x * 4 + 1] << 8
                       | (uint32_t)row[x * 4 + 2] << 16 | (uint32_t)row[x * 4 + 3] << 24;
            if (v != first) { uniform = 0; break; }
        }
    }
    if (uniform) {
        ID3D11DeviceContext_ClearDepthStencilView(context, depth_view,
                D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                (float)(first >> 8) / 16777215.0f, (UINT8)(first & 255));
        return 1;
    }
    if (FAILED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)depth_staging,
            0, D3D11_MAP_WRITE, 0, &mapped))) return 0;
    for (y = 0; y < surface_height; ++y) {
        const uint8_t *row = depth + (size_t)y * depth_row;
        uint32_t *out = (uint32_t *)((uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch);
        for (x = 0; x < surface_width; ++x) {
            uint32_t v = (uint32_t)row[x * 4] | (uint32_t)row[x * 4 + 1] << 8
                       | (uint32_t)row[x * 4 + 2] << 16 | (uint32_t)row[x * 4 + 3] << 24;
            out[x] = ((v & 255u) << 24) | (v >> 8);
        }
    }
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)depth_staging, 0);
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)depth_surface,
                                     (ID3D11Resource *)depth_staging);
    return 1;
}

/* ================================================================
 * Triangle assembly -- identical to the Metal path, deliberately
 * ================================================================ */
static float area(const float a[4], const float b[4], const float c[4])
{ return (b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0]); }

static int vertex_valid(const NV2ATextureCopy *s, const float (*v)[16][4], unsigned i)
{
    unsigned k, u;
    for (k = 0; k < 4; k++)
        if (!isfinite(v[i][0][k]) || !isfinite(v[i][3][k]) || !isfinite(v[i][4][k])) return 0;
    for (u = 0; u < 4; u++) if (s->texture_mask & (1u << u)) {
        for (k = 0; k < 4; k++) if (!isfinite(v[i][9+u][k])) return 0;
        if (v[i][9+u][3] <= 0) return 0;
    }
    return 1;
}

static void triangle(const NV2ATextureCopy *s, const float (*v)[16][4],
        uint32_t *out, unsigned *n, unsigned a, unsigned b, unsigned c)
{
    float ar;
    int front;
    if (!vertex_valid(s,v,a) || !vertex_valid(s,v,b) || !vertex_valid(s,v,c)) return;
    ar = area(v[a][0], v[b][0], v[c][0]);
    if (!isfinite(ar) || ar == 0) return;
    front = (ar > 0) == (s->front_cw != 0);
    if (s->cull_face == 0x408 || (s->cull_face == 0x404 && front)
            || (s->cull_face == 0x405 && !front)) return;
    out[(*n)++] = a; out[(*n)++] = b; out[(*n)++] = c;
}

static int ensure_dynamic(ID3D11Buffer **buffer, unsigned *capacity,
        unsigned bytes, UINT bind)
{
    D3D11_BUFFER_DESC desc;
    if (*buffer && *capacity >= bytes) return 1;
    RELEASE(*buffer);
    *capacity = 0;
    memset(&desc, 0, sizeof(desc));
    desc.ByteWidth = (bytes + 4095) & ~4095u;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = bind;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(device, &desc, NULL, buffer))) return 0;
    *capacity = desc.ByteWidth;
    return 1;
}

static int write_dynamic(ID3D11Buffer *buffer, const void *data, size_t bytes)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)buffer, 0,
            D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return 0;
    memcpy(mapped.pData, data, bytes);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)buffer, 0);
    return 1;
}

static int draw_inner(const NV2ATextureCopy *s, const uint8_t *texture, size_t texture_size,
        uint8_t *target, size_t target_size, uint8_t *depth, size_t depth_size,
        const float (*vertices)[16][4], unsigned count, unsigned primitive)
{
    static uint32_t indices[12288];
    static Vertex staging[4096];
    ID3D11ShaderResourceView *views[4];
    ID3D11SamplerState *samplers[4];
    ID3D11BlendState *blend;
    ID3D11DepthStencilState *zstate;
    ID3D11PixelShader *fragment;
    int reuse_for_trace = 0;
    D3D11_VIEWPORT viewport;
    VSParams vsp;
    PSParams psp;
    const float white[4] = { 1, 1, 1, 1 };
    unsigned n = 0, i, u;
    int use_zeta;
    uint8_t *next_depth;
    uint32_t next_depth_pitch;
    size_t next_depth_size;

    if (!s) return reject("null-state");
    if ((s->texture_mask & 1) && !texture) return reject("missing-texture");
    if (!target) return reject("missing-target");
    if (!vertices || count < 3 || count > 4096) return reject("vertex-count");
    if (s->target_bpp != 2) return reject("target-format");
    if (!s->clip_w || !s->clip_h || s->clip_x || s->clip_y
            || s->clip_w > 4096 || s->clip_h > 4096) return reject("clip");
    if (s->target_pitch < (uint64_t)s->clip_w * 2) return reject("target-pitch");
    for (u = 0; u < 4; u++) if (s->texture_mask & (1u << u)) {
        const NV2ATextureCopy *t;
        const uint8_t *data;
        size_t bytes;
        if (u && !s->extra_stages) return reject("missing-stage-state");
        t = u ? &s->extra_stages[u-1] : s;
        data = u ? s->extra_texture[u-1] : texture;
        bytes = u ? s->extra_size[u-1] : texture_size;
        if (!data) return reject("missing-texture");
        if (!t->width || !t->height || t->width > 4096 || t->height > 4096
                || !t->levels || t->levels > 13) return reject("texture-size");
        if (nv2a_texture_copy_texture_bytes(t) > bytes) return reject("texture-bounds");
        /* The Metal path samples the guest bytes as they stand; this one
         * unpacks them, so a texture large enough to make that expensive goes
         * back to the software rasteriser instead. */
        if ((uint64_t)t->width * t->height > TEXTURE_DECODE_LIMIT)
            return reject("texture-decode-size");
    }
    if ((uint64_t)s->target_pitch * s->clip_h > target_size) return reject("target-bounds");
    if ((s->depth_test || s->stencil_test)
            && (!depth || s->depth_pitch < (uint64_t)s->clip_w * 4
                || (uint64_t)s->depth_pitch * s->clip_h > depth_size))
        return reject("depth-bounds");

    switch (primitive) {
    case 5: for (i = 0; i+2 < count; i += 3) triangle(s,vertices,indices,&n,i,i+1,i+2); break;
    case 6: for (i = 0; i+2 < count; i++) triangle(s,vertices,indices,&n,i+(i&1),i+1-(i&1),i+2); break;
    case 7: for (i = 1; i+1 < count; i++) triangle(s,vertices,indices,&n,0,i,i+1); break;
    case 8: for (i = 0; i+3 < count; i += 4) {
                triangle(s,vertices,indices,&n,i,i+1,i+2);
                triangle(s,vertices,indices,&n,i,i+2,i+3); } break;
    case 9: for (i = 0; i+3 < count; i += 2) {
                triangle(s,vertices,indices,&n,i,i+1,i+3);
                triangle(s,vertices,indices,&n,i,i+3,i+2); } break;
    default: return reject("primitive");
    }
    if (!n) { reject_reason = NULL; return 0; }
    if (!initialize()) return reject("initialization");

    use_zeta = s->depth_test || s->stencil_test;
    next_depth = use_zeta ? depth : NULL;
    next_depth_pitch = use_zeta ? s->depth_pitch : 0;
    next_depth_size = use_zeta ? depth_size : 0;

    /* RECOMP_D3D11_TRACE=<n>: why the retained surface was thrown away, for
     * the first n batches. One re-upload per batch means the surface is not
     * being retained at all, and the reason decides whether that is the
     * guest's doing or ours. */
    /* Pick the retained surface for THIS target. A hit is a bind and nothing
     * more -- no upload, no read-back -- which is what makes the guest's
     * per-batch target ping-pong affordable. */
    {
        Surface *slot = NULL, *oldest = &surfaces[0];
        unsigned i;
        int reuse;
        for (i = 0; i < SURFACE_CACHE_SIZE; i++) {
            Surface *sf = &surfaces[i];
            if (sf->tex && sf->ram == target && sf->ram_size == target_size
                    && sf->w == s->clip_w && sf->h == s->clip_h
                    && sf->row == s->target_pitch
                    && sf->zram == next_depth && sf->zrow == next_depth_pitch
                    && sf->zram_size == next_depth_size) { slot = sf; break; }
            if (!sf->tex) { if (!slot) slot = sf; }
            else if (sf->stamp < oldest->stamp) oldest = sf;
        }
        if (!slot) {
            /* Evicting means flushing, because that surface holds pixels the
             * guest has not been given back yet. */
            slot = oldest;
            ++surface_evictions;
            if (!sync_surface(slot)) return reject("surface-sync");
            slot->colour_ready = 0;
            slot->depth_ready = 0;
        }
        reuse = slot->tex && slot->colour_ready
              && (!next_depth || slot->depth_ready);
        reuse_for_trace = reuse;
        bound = slot;
        slot->stamp = ++surface_clock;

        {
            static long trace = -1;
            static unsigned traced;
            if (trace < 0) {
                const char *e = getenv("RECOMP_D3D11_TRACE");
                trace = e ? strtol(e, NULL, 0) : 0;
            }
            trace_surfaces = traced < (unsigned)trace;
            if (trace_surfaces) {
                ++traced;
                fprintf(stderr, "[D3D11-TRACE] batch %llu target=%p clip=%ux%u"
                        " pitch=%u zeta=%d depth=%p slot=%u %s\n",
                        (unsigned long long)draw_batches, (void *)target,
                        s->clip_w, s->clip_h, s->target_pitch, use_zeta,
                        (void *)next_depth, (unsigned)(slot - surfaces),
                        reuse ? "retained" : "upload");
            }
        }

        if (!reuse) {
            if (!color_surface || surface_width != s->clip_w
                    || surface_height != s->clip_h) {
                if (!create_surfaces(s->clip_w, s->clip_h))
                    return reject("surface-allocation");
            }
            surface_target = target;
            surface_target_size = target_size;
            surface_width = s->clip_w;
            surface_height = s->clip_h;
            surface_pitch = s->target_pitch;
            depth_target = next_depth;
            depth_target_size = next_depth_size;
            surface_depth_pitch = next_depth_pitch;
            if (!upload_surface(target, next_depth, next_depth_pitch))
                return reject("surface-upload");
            surface_valid = 1;
            depth_valid = next_depth ? 1 : 0;
            surface_dirty = depth_dirty = 0;
        }
    }

    memset(&vsp, 0, sizeof(vsp));
    vsp.viewport[0] = (float)s->clip_w;
    vsp.viewport[1] = (float)s->clip_h;
    memset(&psp, 0, sizeof(psp));
    psp.frag[0] = s->alpha_ref;
    fragment = pixel_shader_for(s);
    if (!fragment) return reject("fragment-program");
    for (u = 0; u < 4; u++) {
        int active = (s->texture_mask & (1u << u)) != 0;
        const NV2ATextureCopy *t = (u && active) ? &s->extra_stages[u-1] : s;
        vsp.texscale[u][0] = vsp.texscale[u][1] = 1.0f;
        views[u] = NULL;
        samplers[u] = NULL;
        if (!active) { views[u] = empty_view(); continue; }
        /* Only the pitch-linear RGB565 image rectangle addresses in texels;
         * every other format the CPU path accepts is normalised already. */
        if (!t->rgba8 && !t->dxt1 && !t->dxt3) {
            vsp.texscale[u][0] = 1.0f / (float)t->width;
            vsp.texscale[u][1] = 1.0f / (float)t->height;
        }
        views[u] = texture_view(t, u ? s->extra_texture[u-1] : texture,
                                nv2a_texture_copy_texture_bytes(t));
        samplers[u] = sampler_state(t);
        if (!views[u] || !samplers[u]) return reject("texture-upload");
    }
    if (!views[0]) return reject("texture-upload");
    for (u = 0; u < 4; u++) if (!samplers[u]) samplers[u] = sampler_state(s);

    blend = blend_state(s);
    zstate = depth_state(s);
    if (!blend || !zstate) return reject("pipeline-state");

    for (i = 0; i < count; i++) {
        memcpy(staging[i].p,  vertices[i][0], 16);
        memcpy(staging[i].d0, vertices[i][3], 16);
        memcpy(staging[i].d1, vertices[i][4], 16);
        for (u = 0; u < 4; u++) memcpy(staging[i].t[u], vertices[i][9+u], 16);
    }
    if (!ensure_dynamic(&vertex_buffer, &vertex_capacity,
            (unsigned)(count * sizeof(Vertex)), D3D11_BIND_VERTEX_BUFFER)
        || !ensure_dynamic(&index_buffer, &index_capacity,
            (unsigned)(n * sizeof(indices[0])), D3D11_BIND_INDEX_BUFFER))
        return reject("buffer-allocation");
    if (!write_dynamic(vertex_buffer, staging, count * sizeof(Vertex))
        || !write_dynamic(index_buffer, indices, n * sizeof(indices[0]))
        || !write_dynamic(vs_constants, &vsp, sizeof(vsp))
        || !write_dynamic(ps_constants, &psp, sizeof(psp)))
        return reject("buffer-map");

    {
        UINT stride = sizeof(Vertex), offset = 0;
        ID3D11DeviceContext_IASetInputLayout(context, input_layout);
        ID3D11DeviceContext_IASetVertexBuffers(context, 0, 1, &vertex_buffer, &stride, &offset);
        ID3D11DeviceContext_IASetIndexBuffer(context, index_buffer, DXGI_FORMAT_R32_UINT, 0);
        ID3D11DeviceContext_IASetPrimitiveTopology(context,
                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
    ID3D11DeviceContext_VSSetShader(context, vertex_shader, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(context, 0, 1, &vs_constants);
    ID3D11DeviceContext_PSSetShader(context, fragment, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(context, 0, 1, &ps_constants);
    ID3D11DeviceContext_PSSetShaderResources(context, 0, 4, views);
    ID3D11DeviceContext_PSSetSamplers(context, 0, 4, samplers);
    ID3D11DeviceContext_RSSetState(context, rasterizer);
    memset(&viewport, 0, sizeof(viewport));
    viewport.Width = (float)surface_width;
    viewport.Height = (float)surface_height;
    viewport.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
    ID3D11DeviceContext_OMSetBlendState(context, blend, white, 0xffffffffu);
    ID3D11DeviceContext_OMSetDepthStencilState(context, zstate, s->stencil_ref & 255);
    ID3D11DeviceContext_OMSetRenderTargets(context, 1, &color_view,
            use_zeta ? depth_view : NULL);
    ID3D11DeviceContext_DrawIndexed(context, n, 0, 0);

    surface_dirty = 1;
    if ((s->depth_test && s->depth_write) || (s->stencil_test && s->stencil_write))
        depth_dirty = 1;
    ++draw_batches;
    drawn_triangles += n / 3;
    if (nv2a_d3d11_event_trace())
        fprintf(stderr, "  [EV] DRAW   target=%08X texture=%08X mask=%x %u triangles%s\n",
                nv2a_d3d11_guest_offset(target),
                nv2a_d3d11_guest_offset(texture), s->texture_mask, n / 3,
                reuse_for_trace ? " (retained)" : " (uploaded)");
    if (nv2a_d3d11_event_trace()) {
        /* Whether the SOURCE had anything in it. A textured copy with depth
         * off that writes black had a black texture, and the only way this
         * path gets one is by reading guest RAM that the pixels have not been
         * given back to yet. */
        size_t tb = (s->texture_mask & 1) ? nv2a_texture_copy_texture_bytes(s) : 0;
        size_t i, nz = 0;
        for (i = 0; texture && i < tb; ++i) if (texture[i]) ++nz;
        fprintf(stderr, "  [EV]        zeta=%d depth=%08X test=%u write=%u"
                " texbytes=%zu nonzero=%zu combiners=%u blend=%u\n",
                use_zeta, nv2a_d3d11_guest_offset(next_depth),
                s->depth_test, s->depth_write, tb, nz,
                s->combiner_count, s->blend);
    }
    reject_reason = NULL;
    /* RECOMP_D3D11_SYNC_EACH=1: give the pixels back immediately instead of at
     * the flip. Slow by construction, and diagnostic only -- it separates "the
     * renderer never produced them" from "the read-back happens at the wrong
     * moment", which the equivalence test cannot, because it syncs per batch
     * itself and so has never exercised the deferred path the game uses. */
    {
        static int each = -1;
        if (each < 0) each = getenv("RECOMP_D3D11_SYNC_EACH") ? 1 : 0;
        if (each) (void)sync_range_inner(NULL, 0);
    }
    publish_ownership();
    return (int)(n / 3);
}

int nv2a_d3d11_draw(const NV2ATextureCopy *s, const uint8_t *texture, size_t texture_size,
        uint8_t *target, size_t target_size, uint8_t *depth, size_t depth_size,
        const float (*vertices)[16][4], unsigned count, unsigned primitive)
{
    int result;
    device_acquire();
    result = draw_inner(s, texture, texture_size, target, target_size,
                        depth, depth_size, vertices, count, primitive);
    /* Every early return above is a rejection that leaves the pending flags
     * where they were, but an eviction or an upload can have moved them, so
     * the map is reconciled on the way out regardless of the verdict. */
    publish_ownership();
    device_release();
    return result;
}
