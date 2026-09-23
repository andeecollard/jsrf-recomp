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
} D3D8HostDrawCheck;
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
} D3D8HostStats;
void d3d8_host_get_stats(D3D8HostStats *out);
void d3d8_host_report(const char *why);
#endif
