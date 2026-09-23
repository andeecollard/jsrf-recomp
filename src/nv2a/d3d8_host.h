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
} D3D8HostDrawCheck;
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
} D3D8ExecDrawTextures;
void d3d8_host_set_exec_source(void (*get)(D3D8ExecDrawTextures *out));
uint32_t d3d8_host_enqueue_check(const D3D8HostDrawCheck *c);

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
} D3D8HostStats;
/* G41's comparison on its own, for the unit test: counts into the stats and
 * returns 1 if every executor array and the indices agree with D3D. */
int d3d8_host_check_streams(const D3D8HostDrawCheck *c, const D3D8ExecDrawTextures *e);
void d3d8_host_get_stats(D3D8HostStats *out);
void d3d8_host_report(const char *why);
#endif
