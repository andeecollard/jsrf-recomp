#ifndef D3D8_HOST_H
#define D3D8_HOST_H
/* HOST WORK BEHIND A RING TOKEN (G37).
 *
 * A D3D entry point replaced by host code computes the NV2A commands its
 * original would have emitted, queues them here, and writes one host token
 * (nv2a_pusher.h) into the guest ring in their place. When the ring consumer
 * reaches the token it replays the queued commands through the same dispatch
 * a ring command takes -- same thread, same order, same executor state -- so
 * the rest of the frame cannot tell the difference.
 *
 * This is a step, not the destination: the replay still goes through the
 * NV2A executor. What it establishes is the part every later step needs --
 * D3D semantics computed by host code and delivered in ring order.
 *
 * Producer: the guest thread calling D3D. Consumer: the pusher thread. A slot
 * is published with release ordering before the token that names it is
 * written, and freed by the consumer after replay; a producer that finds its
 * slot still busy gets 0 back and must fall back to the original. */
#include <stdint.h>
#include "d3d8_ff_combiner.h"
#include "d3d8_ff_vertex_state.h"

#define D3D8_HOST_MAX_METHODS 64u

/* Queue `n` (subchannel 0) method/parameter pairs. Returns the token parameter
 * to write, or 0 if the queue is full or n is out of range. */
uint32_t d3d8_host_enqueue(const uint32_t *methods, const uint32_t *params, unsigned n);

/* Installs the pusher's token handler. Idempotent; d3d8_host_enqueue calls it. */
void d3d8_host_install(void);

/* ---- G39: the host's D3D state mirror, checked draw by draw ----
 *
 * The replaced/observed D3D entry points keep a host-side mirror of what D3D
 * has bound. After each draw, the mirror is snapshotted into a check item and
 * a token is written behind the draw's commands; when the ring consumer
 * reaches it, the executor has just run that draw, and the snapshot is
 * compared with what the executor reconstructed from the NV2A commands.
 * Agreement means the host can describe the draw from D3D alone -- which is
 * what a renderer fed at the D3D boundary needs. */
typedef struct {
    uint32_t serial;           /* D3D draw number, for the report */
    uint32_t tex[4];           /* D3DBaseTexture* per stage, 0 = none */
    uint32_t data[4], format[4], size[4];   /* ->Data, ->Format, ->Size */
    /* The device's current colour and depth surfaces (device +0x2070/+0x2074)
     * and their Data/Format/Size. Size packs width-1 (bits 0-11), height-1
     * (12-23) and pitch/64-1 (24-31). */
    uint32_t rt, rt_data, rt_format, rt_size;
    uint32_t zs, zs_data, zs_format, zs_size;
    /* Viewport as D3D stores it (device +0x9D0..+0x9E4: X, Y, W, H, MinZ, MaxZ)
     * and the supersample scales (+0x454/+0x458). */
    int32_t vp_x, vp_y, vp_w, vp_h; float vp_minz, vp_maxz, ss_x, ss_y;
    /* The last value D3D pushed through SetRenderState_Simple for each method
     * in D3D8_HOST_STATE_METHODS, and which of them it has pushed at all. */
    uint32_t st_val[11], st_seen;
    /* Vertex shader constants as D3D was handed them by SetVertexShaderConstant:
     * NV2A slot = D3D register + 96. vc_written marks slots D3D has written. */
    float    vc[192][4];
    uint32_t vc_written[6];
    /* The pixel shader definition D3D was given (SetPixelShader's handle +8),
     * 57 words; ps_bound = 0 means fixed-function combiners. */
    uint32_t ps_bound, ps[57];
    /* D3D's deferred texture-stage state (0x19DEE0): 4 stages x 32 words. */
    uint32_t tss[4][32];
    /* The current vertex shader (device +0x384 handle). Programmable shaders
     * carry a ready-made fragment at object+0x114 (length object+0xC dwords)
     * of SET_TRANSFORM_PROGRAM packets, loaded at slot 0; vs_words are the
     * program words extracted from it. vs_kind: 0 none/fixed, 1 programmable,
     * 2 programmable but its fragment did not parse. */
    uint32_t vs_kind, vs_handle, vs_nwords, vs_words[136 * 4];
    /* SetTransform: WORLD (6), VIEW (0), PROJECTION (1) in XDK 4134, as handed to D3D. */
    float    xf_world[16], xf_view[16], xf_proj[16];
    uint32_t xf_seen;          /* bit0 world, bit1 view, bit2 proj */
    /* ---- G41: vertex streams and indices ----
     * draw_kind: 0 unknown, 1 DrawVertices(prim, start, count), 2
     * DrawIndexedVertices(prim, count, pIndexData). idx[] holds the first
     * nidx (<= D3D8_HOST_IDX_N) indices D3D was handed; for DrawVertices the
     * implied run start..start+count-1. */
    uint32_t draw_kind, prim, count, start, nidx;
    uint16_t idx[16];
    /* The device's streams at the draw (D3D_g_Stream, 0x19DCE8, 12 bytes each:
     * Stride, Offset, pVertexBuffer) and each vertex buffer's Data (+4). */
    uint32_t st_stride[16], st_offset[16], st_vb[16], st_data[16];
    /* The index state: BaseVertexIndex (device +0x1C), the index buffer
     * (device +0x38C) and D3D's index base (0x19DED4 = pIB->Data). */
    uint32_t base_vertex, ib, ib_data;
    /* What D3D's array setup (sub_00196520) derives per NV2A array slot from
     * the vertex shader object's attribute records (device +0x380; record at
     * obj + 16*attr + 0x14: stream, offset, format) -- the 0x1720 offset
     * Data + attr.offset + Stream.Offset + base*Stride and the 0x1760 word
     * Stride << 8 | attr.format. va_on marks slots D3D enables (format size
     * nibble nonzero and a vertex buffer bound). */
    uint32_t va_on, va_stream[16], va_offset[16], va_format[16];
    /* Cross-check: what the SetStreamSource / SetIndices hooks were handed. */
    uint32_t hk_stride[16], hk_vb[16], hk_stream_seen, hk_ib, hk_base, hk_ib_seen;
    /* ---- G43: fixed-function combiners ----
     * D3D builds them LAZILY: the flusher 0x1964A0 runs the builder 0x197F90
     * only when dirty bit 0x800 of 0x19DED8 is set, and the fog updater
     * 0x195610 (SPECULAR_FOG_CW0/1) only on 0x2000. So the executor's
     * registers at a draw are whatever those last emitted. The mirror keeps
     * both the builder's inputs at its last EMITTING entry (a hook on
     * 0x197F90 / 0x195610 in the staged gen) and the same inputs read at the
     * draw; the check transcribes the former and reports how often the
     * latter would have differed. ffc_valid = the mirror filled this block. */
    uint32_t ffc_valid;
    uint32_t ffc_ps;                    /* device +0x370 at the draw: nonzero = not fixed-function */
    D3D8FFCombinerIn ffc_cur;           /* the builder's inputs, read at the draw */
    uint32_t ffc_emit_seen;             /* 0x197F90 has emitted since the mirror armed */
    uint32_t ffc_emit_fresh;            /* ... and did so inside this draw's flush */
    D3D8FFCombinerIn ffc_emit;          /* its inputs at the last emission */
    uint32_t tfactor;                   /* D3D_g_RenderState[129] (0x19E2E4) at the draw */
    /* Fog updater inputs {RS[82] FOGENABLE, RS[93] SPECULARENABLE, device
     * +0x370, device +0x374}: at the draw, and at its last CW-writing entry. */
    uint32_t fog_cur[4], fog_emit_seen, fog_emit[4];
    uint32_t ffc_control;               /* RECOMP_D3D8_MIRROR_CONTROL: perturb one transcribed word */
    /* ---- G42: fixed-function vertex state ----
     * Texgen (TexCoordIndex 0x18F060) and fog colour (0x18EB80) are written
     * at once, so they are transcribed from the state at the draw (tss[][28]
     * above, ffv_fog_color). Texture transforms (0x1957F0, dirty 0x400),
     * lighting (0x195F80, dirty 0x1000) and fog (0x195610, dirty 0x2000) are
     * lazy like the combiners: each is carried as its inputs at its last
     * EMITTING entry (a hook in the staged gen) and as the same inputs read
     * at the draw. ffv_valid = the mirror filled this block. */
    uint32_t ffv_valid;
    uint32_t ffv_vs_flags;              /* [device+0x380]+4 at the draw; & 0x12 = not fixed-function 3D */
    uint32_t ffv_fog_color;             /* D3D_g_RenderState[119] FOGCOLOR at the draw */
    uint32_t tx_emit_seen, tx_emit_fresh;
    D3D8FFTexXformIn tx_emit, tx_cur;
    uint32_t fg_emit_seen, fg_emit_fresh;
    D3D8FFFogIn fg_emit, fg_cur;
    uint32_t lt_emit_seen, lt_emit_fresh;
    D3D8FFLightIn lt_emit, lt_cur;
    /* SPECULAR_ENABLE (0x03B8) has two writers: the light updater and the
     * combiner builder (G43, when device+8 bit 0x40 changes). Whichever wrote
     * last holds the register; the mirror numbers the emissions. */
    uint32_t lt_emit_seq, sp_emit_seq, sp_emit_val;
    uint32_t ffv_control;               /* RECOMP_D3D8_MIRROR_CONTROL: perturb one word per group */
    /* ---- G51.1: pre-transformed 2D, drawn by the host (d3d8_host_2d.c) ----
     * idx_ptr is DrawIndexedVertices' pIndexData (0 for DrawVertices): the
     * host fetches every index, not only the first D3D8_HOST_IDX_N. x_val is
     * D3D's last SetRenderState_Simple push of each method in
     * D3D8_HOST_2D_EXTRA_METHODS, and x_seen which of them it pushed at all. */
    uint32_t idx_ptr;
    uint32_t x_val[8], x_seen;
} D3D8HostDrawCheck;
/* G51.1: the Simple-pushed methods the host's 2D draw needs beyond
 * D3D8_HOST_STATE_METHODS: CULL_FACE_ENABLE, DITHER_ENABLE, BLEND_COLOR,
 * COLOR_MASK, CULL_FACE, FRONT_FACE, ZMIN_MAX_CONTROL, FOG_ENABLE. */
#define D3D8_HOST_2D_EXTRA_METHODS { 0x308, 0x310, 0x34C, 0x358, 0x39C, 0x3A0, 0x1D78, 0x2A4 }
/* G43: the combiner registers compared, one word each, in this order. */
#define D3D8_HOST_FFC_N 51u
typedef struct {
    uint32_t w[D3D8_HOST_FFC_N];
} D3D8CombinerRegs;
/* NV2A method of word k of D3D8CombinerRegs. Inline so the executor, which
 * some unit tests build without d3d8_host.c, can use it. */
static inline uint32_t d3d8_host_ffc_method(unsigned k)
{
    if (k == 0) return 0x1E60u;                      /* COMBINER_CONTROL */
    if (k < 9)  return 0x0AC0u + 4u * (k - 1u);      /* COLOR_ICW[0..7] */
    if (k < 17) return 0x1E40u + 4u * (k - 9u);      /* COLOR_OCW[0..7] */
    if (k < 25) return 0x0260u + 4u * (k - 17u);     /* ALPHA_ICW[0..7] */
    if (k < 33) return 0x0AA0u + 4u * (k - 25u);     /* ALPHA_OCW[0..7] */
    if (k < 41) return 0x0A60u + 4u * (k - 33u);     /* FACTOR0[0..7] */
    if (k < 49) return 0x0A80u + 4u * (k - 41u);     /* FACTOR1[0..7] */
    return k == 49 ? 0x0288u : 0x028Cu;              /* SPECULAR_FOG_CW0/1 */
}
/* A name for word k, for the report. */
const char *d3d8_host_ffc_name(unsigned k, char *buf, unsigned n);
#define D3D8_HOST_IDX_N 16u
/* Register <- definition word, exactly as SetPixelShader (0x199BE0) emits them. */
#define D3D8_HOST_PS_PAIRS { {0x260,0}, {0x264,1}, {0x268,2}, {0x26C,3}, {0x270,4}, {0x274,5}, {0x278,6}, {0x27C,7}, {0xA60,10}, {0xA64,11}, {0xA68,12}, {0xA6C,13}, {0xA70,14}, {0xA74,15}, {0xA78,16}, {0xA7C,17}, {0xA80,18}, {0xA84,19}, {0xA88,20}, {0xA8C,21}, {0xA90,22}, {0xA94,23}, {0xA98,24}, {0xA9C,25}, {0xAA0,26}, {0xAA4,27}, {0xAA8,28}, {0xAAC,29}, {0xAB0,30}, {0xAB4,31}, {0xAB8,32}, {0xABC,33}, {0xAC0,34}, {0xAC4,35}, {0xAC8,36}, {0xACC,37}, {0xAD0,38}, {0xAD4,39}, {0xAD8,40}, {0xADC,41}, {0x17F8,42}, {0x1E20,43}, {0x1E24,44}, {0x1E40,45}, {0x1E44,46}, {0x1E48,47}, {0x1E4C,48}, {0x1E50,49}, {0x1E54,50}, {0x1E58,51}, {0x1E5C,52}, {0x1E60,53}, {0x1E74,55}, {0x1E78,56} }
#define D3D8_HOST_PS_N 54
#define D3D8_HOST_STATE_METHODS { 0x300, 0x304, 0x30C, 0x32C, 0x33C, 0x340, 0x344, 0x348, 0x350, 0x354, 0x35C }

/* What the executor made of the draw it just ran. */
typedef struct {
    int      active;           /* 0: the executor refused or has not drawn */
    uint32_t mask;             /* texture units in use */
    uint32_t addr[4];          /* guest address of each unit's texture */
    uint32_t width[4], height[4], levels[4];
    uint32_t fmt[4];           /* the NV2A format byte, decoded class for linear */
    uint32_t target_addr, target_pitch, target_bpp;
    int      depth_used;       /* depth or stencil test on: depth_addr is meaningful */
    uint32_t depth_addr, depth_pitch;
    uint32_t win_x0, win_y0, win_x1, win_y1;   /* effective scissor, inclusive */
    uint32_t clip_x, clip_y, clip_w, clip_h;   /* surface clip */
    float    z_min, z_max;
    uint32_t st_reg[11];                       /* executor registers, same method list */
    float    vc[192][4];                       /* the executor's constant file */
    uint32_t ps_reg[D3D8_HOST_PS_N];           /* executor registers for D3D8_HOST_PS_PAIRS */
    uint32_t tex_address[4], tex_filter[4], tex_control0[4];   /* raw NV2A texture registers per unit */
    uint32_t vs_words[136 * 4];                /* the executor's program memory, slot 0 up */
    float    ff_modelview[16], ff_composite[16], ff_projection[16];   /* 0x480, 0x680, 0x440 */
    uint32_t exec_mode, prog_start, composite_ever_written, vsh_mode_internal;
    /* G41: filled whether or not `active` is set. The executor's raw vertex
     * array registers (0x1720/0x1760 + 4i) and the indices it drew last. */
    int      va_valid;
    uint32_t va_offset[16], va_format[16];
    uint32_t idx_count, idx[16];
    /* G43: filled whether or not `active` is set. The executor's latched
     * combiner registers (method shadow) in D3D8CombinerRegs order. */
    int      ffc_valid;
    D3D8CombinerRegs ffc;
    /* G42: filled whether or not `active` is set. The executor's whole method
     * shadow for subchannel 0 (methods 0..0x1FFC), as latched at the token. */
    int      regs_valid;
    uint32_t regs[0x2000 / 4];
} D3D8ExecDrawTextures;
void d3d8_host_set_exec_source(void (*get)(D3D8ExecDrawTextures *out));
uint32_t d3d8_host_enqueue_check(const D3D8HostDrawCheck *c);
/* G51.1: a token written BEFORE a pre-transformed 2D draw's commands. When
 * the ring consumer reaches it the executor has run everything before that
 * draw, so the render target holds what the draw starts from; the handler
 * (d3d8_host_2d_pre) snapshots it for the host's shadow. `serial` is the
 * serial the mirror's after-draw check of the same draw will carry. */
uint32_t d3d8_host_enqueue_2d_pre(uint32_t serial, uint32_t rt_data, uint32_t rt_format, uint32_t rt_size);

typedef struct {
    unsigned long long enqueued, replayed, methods_replayed, full, bad_token;
    unsigned long long checks, check_inactive, units_compared, units_match,
                       units_missing, units_addr, units_shape;
    unsigned long long rt_compared, rt_match, rt_addr, rt_pitch,
                       zs_compared, zs_match, zs_missing, zs_addr, zs_pitch;
    unsigned long long vp_compared, vp_match, vp_window, vp_z;
    unsigned long long st_compared[11], st_match[11], st_unseen[11];
    unsigned long long vc_slots_compared, vc_slots_match, vc_draws_all_match, vc_draws;
    unsigned long long ps_draws, ps_draws_fixed, ps_draws_all_match, ps_words_compared, ps_words_match;
    unsigned long long ps_word_mismatch[D3D8_HOST_PS_N];
    unsigned long long tss_compared, tss_match, tss_addr, tss_mag, tss_min, tss_bias;
    unsigned long long vs_draws_prog, vs_draws_fixed, vs_draws_unparsed, vs_draws_match, vs_words_compared;
    unsigned long long ff_compared, ff_match, ff_mv, ff_comp, ff_other;
    /* G41 streams: arrays = NV2A array slots the executor enabled. */
    unsigned long long va_draws, va_draws_all_match, va_arrays, va_exact, va_in_stream,
                       va_no_d3d, va_stride, va_offset, va_format, va_exec_missing;
    unsigned long long idx_draws, idx_match, idx_count_bad, idx_value_bad, idx_indexed, idx_indexed_match;
    unsigned long long hk_stream_cmp, hk_stream_match, hk_ib_cmp, hk_ib_match;
    /* G43 fixed-function combiners. */
    unsigned long long ffc_draws, ffc_ps_skipped, ffc_all_match, ffc_builder_match, ffc_factor_match,
                       ffc_final_match, ffc_final_skipped, ffc_unresolved, ffc_no_emit, ffc_fresh,
                       ffc_lazy_differs, ffc_exec_cur_only, ffc_exec_emit_only;
    unsigned long long ffc_word_mismatch[D3D8_HOST_FFC_N];
    unsigned long long ffc_tally_overflow;
    unsigned ffc_tally_pairs;
    /* G42 fixed-function vertex state. Per group g (D3D8_HOST_FFV_*):
     * draws compared, draws with every register matching, words compared,
     * exact, within tolerance (floats only), and the laziness counters. */
    unsigned long long ffv_draws[4], ffv_all[4], ffv_words[4], ffv_exact[4], ffv_tol[4],
                       ffv_noemit[4], ffv_fresh[4], ffv_lazy[4], ffv_cur_only[4], ffv_emit_only[4],
                       ffv_unres[4];
    /* What the title uses, counted at the draws compared. */
    unsigned long long lt_lit, lt_lights[9], lt_dir, lt_point, lt_spot, lt_colormat, lt_twosided,
                       lt_specular, lt_sp_from_builder;
    unsigned long long tg_mode[4][6];            /* per stage: off, EYE, OBJECT, SPHERE, NORMAL, REFLECTION */
    unsigned long long tx_enabled[4], tx_case[D3D8FF_TX_CASES];
    unsigned long long fg_on, fg_table[4], fg_range, fg_color_nonzero;
    unsigned long long ffv_reg_mismatch_total;
} D3D8HostStats;
/* G42 check groups. */
enum { D3D8_HOST_FFV_TEXGEN = 0, D3D8_HOST_FFV_TEXXFORM = 1, D3D8_HOST_FFV_LIGHT = 2, D3D8_HOST_FFV_FOG = 3 };
/* G41's comparison on its own, for the unit test: counts into the stats and
 * returns 1 if every executor array and the indices agree with D3D. */
int d3d8_host_check_streams(const D3D8HostDrawCheck *c, const D3D8ExecDrawTextures *e);
/* G43's comparison on its own, for the unit test: the transcription of the
 * builder (on the last-emitted inputs), SetRenderState_TextureFactor and the
 * fog updater, against the executor's latched registers. Returns 1 if every
 * compared register agrees (or the draw is not compared), 0 otherwise.
 * `expect`, when not NULL, receives the transcribed register set. */
int d3d8_host_check_combiners(const D3D8HostDrawCheck *c, const D3D8ExecDrawTextures *e, D3D8CombinerRegs *expect);
/* G42's comparison on its own, for the unit test: texgen and fog colour from
 * the state at the draw; texture transforms, lighting and fog from the inputs
 * their updaters last emitted with; all against the executor's latched
 * registers (e->regs). Fixed-function-only groups (texgen, texture
 * transforms, lighting) are compared only when ffv_vs_flags & 0x12 is 0; fog
 * at every draw. Returns 1 if every compared register agrees. */
int d3d8_host_check_ff_vertex(const D3D8HostDrawCheck *c, const D3D8ExecDrawTextures *e);
/* Mismatch count of one method (0..0x1FFC) under the G42 check. */
unsigned long long d3d8_host_ffv_reg_mismatches(uint32_t method);
/* G43 discovery: the distinct (stage words, TFACTOR) -> register pairings seen
 * at fixed-function draws, the `top` most frequent printed. */
void d3d8_host_ffc_tally_report(const char *why, unsigned top);
void d3d8_host_get_stats(D3D8HostStats *out);
void d3d8_host_report(const char *why);
#endif
