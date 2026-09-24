/* G39: the host's mirror of D3D texture bindings, checked against the NV2A
 * executor draw by draw. Copied into a SCRATCH gen as recomp_zz_d3d8_mirror.c
 * by stage_d3d8_census.py --mirror; RECOMP_D3D8_MIRROR=1 arms it.
 *
 * SetTexture's wrapper records (stage, texture) after the original ran; each
 * DrawIndexedVertices / DrawVertices wrapper, after the original ran, snapshots
 * the bound textures' Data/Format/Size (XDK D3DPixelContainer: +4, +0xC, +0x10)
 * into a check item and writes a host token behind the draw's commands. The
 * token reaches the executor right after the draw it describes, and d3d8_host.c
 * compares the two. Read-only with respect to the title: the token is the only
 * thing written, and it never reaches PGRAPH.
 *
 * G41 adds vertex streams and indices: each draw wrapper passes the draw's
 * three arguments, the snapshot derives every NV2A array slot's offset and
 * format from D3D's stream table and vertex shader object (d3d8m_streams),
 * and SetStreamSource / SetIndices hooks are carried along as a cross-check.
 *
 * G43 adds the fixed-function combiners: hooks on the builder 0x197F90 and the
 * fog updater 0x195610 (both internal, both run lazily from the flusher
 * 0x1964A0) record their inputs as they emit, and each draw carries those and
 * the same inputs read at the draw, plus TEXTUREFACTOR (RenderState[129]).
 *
 * G42 adds the fixed-function vertex state: hooks on the texture-transform
 * updater 0x1957F0 and the light updater 0x195F80 (internal, lazy, run from
 * the flusher on dirty 0x400 / 0x1000), and the fog updater's own registers
 * from the 0x195610 hook; texgen and fog colour are immediate and read at the
 * draw. SPECULAR_ENABLE has two writers, so the builder and light hooks number
 * their emissions.
 *
 * G51.1 adds the host's own 2D draws (d3d8_host_2d.c). With
 * RECOMP_D3D8_HOST_2D=shadow (which arms the mirror too), each draw whose
 * vertex shader handle is an XYZRHW FVF also gets a token BEFORE its commands
 * (d3d8m_before_draw), so the host can snapshot the render target the draw
 * starts from; the check behind the draw carries the index pointer and the
 * few extra Simple-pushed states the host's fragment stage needs. */
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include "d3d8_host.h"
#include "d3d8_host_2d.h"
#include "nv2a_pusher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* G43: a final report and the combiner-setup tally at exit. Most runs here
 * end in kill -9, so d3d8_host.c also prints the tally every 100,000
 * fixed-function draws; this only catches a clean exit. */
static void d3d8m_exit(void)
{
    d3d8_host_report("exit");
    d3d8_host_ffc_tally_report("exit", 20);
}
static int d3d8m_on(void)
{
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("RECOMP_D3D8_MIRROR");
        m = e && e[0] && strcmp(e, "0") != 0;
        fprintf(stderr, "[D3D8-MIRROR] RECOMP_D3D8_MIRROR=%s\n", m ? "on" : "off");
        /* G51.1: the host's 2D shadow is fed by the mirror's checks. */
        if (!m && d3d8_host_2d_mode()) {
            m = 1;
            fprintf(stderr, "[D3D8-MIRROR] armed by RECOMP_D3D8_HOST_2D\n");
        }
        if (m) atexit(d3d8m_exit);
    }
    return m;
}
static uint32_t m_tex[4], m_serial;
static const uint32_t m_state_methods[11] = D3D8_HOST_STATE_METHODS;
static uint32_t m_state_val[11], m_state_seen;
/* G51.1: the Simple pushes the host's 2D fragment stage needs beyond those. */
static const uint32_t m_x_methods[8] = D3D8_HOST_2D_EXTRA_METHODS;
static uint32_t m_x_val[8], m_x_seen;

/* SetRenderState_ZEnable / _StencilEnable(value): these reach the GPU through
 * their own setters, not Simple. What the renderer needs is "enabled or not",
 * so the mirror holds value != 0 against the register's 0/1. */
static void m_set_state(uint32_t method, uint32_t value)
{
    for (unsigned k = 0; k < 11; ++k)
        if (m_state_methods[k] == method) { m_state_val[k] = value; m_state_seen |= 1u << k; }
}
/* SetVertexShaderConstant(Register, pConstantData, ConstantCount): copy what
 * D3D was handed, after the original ran. NV2A slot = Register + 96. */
static float m_vc[192][4];
static uint32_t m_vc_written[6];
void d3d8m_vs_constant(uint32_t reg, uint32_t data, uint32_t count)
{
    int32_t first = (int32_t)reg + 96;
    for (uint32_t i = 0; i < count && i < 192u; ++i) {
        int32_t slot = first + (int32_t)i;
        if (slot < 0 || slot >= 192) continue;
        for (int k = 0; k < 4; ++k) { uint32_t u = MEM32(data + 16u * i + 4u * (uint32_t)k); memcpy(&m_vc[slot][k], &u, 4); }
        m_vc_written[slot / 32] |= 1u << (slot % 32);
    }
}
static uint32_t m_ps_handle;
static float m_xf[3][16]; static uint32_t m_xf_seen;
/* SetTransform(State, pMatrix): WORLD, VIEW, PROJECTION. */
void d3d8m_set_transform(uint32_t state, uint32_t pm)
{
    /* XDK 4134 numbering, measured 23 Sep: 0 VIEW, 1 PROJECTION, 2-5 TEXTURE0-3,
     * 6 WORLD, 7-9 WORLD1-3. */
    int k = state == 6u ? 0 : state == 0u ? 1 : state == 1u ? 2 : -1;
    if (k < 0) return;
    for (unsigned i = 0; i < 16; ++i) { uint32_t u = MEM32(pm + 4u * i); memcpy(&m_xf[k][i], &u, 4); }
    m_xf_seen |= 1u << k;
}
void d3d8m_set_pixel_shader(uint32_t handle) { m_ps_handle = handle; }
void d3d8m_zenable(uint32_t v)       { m_set_state(0x30Cu, v != 0); }
void d3d8m_stencilenable(uint32_t v) { m_set_state(0x32Cu, v != 0); }

/* SetRenderState_Simple(ecx = method header, edx = value): record each state
 * method's last value -- the render state a host renderer would receive. A
 * header with a count above 1 is not a render-state push and is ignored. */
void d3d8m_simple(uint32_t hdr, uint32_t value)
{
    uint32_t method = hdr & 0x1FFCu;
    if (((hdr >> 18) & 0x7FFu) != 1u || ((hdr >> 13) & 7u)) return;
    for (unsigned k = 0; k < 11; ++k)
        if (m_state_methods[k] == method) { m_state_val[k] = value; m_state_seen |= 1u << k; }
    for (unsigned k = 0; k < 8; ++k)
        if (m_x_methods[k] == method) { m_x_val[k] = value; m_x_seen |= 1u << k; }
}
static unsigned long long m_no_token;

void d3d8m_set_texture(uint32_t stage, uint32_t tex)
{
    if (stage < 4) m_tex[stage] = tex;
}

/* G41 cross-check: what SetStreamSource(StreamNumber, pStreamData, Stride)
 * and SetIndices(pIndexData, BaseVertexIndex) were handed, after the original
 * ran. The check itself reads the device's own state at the draw. */
static uint32_t m_hk_stride[16], m_hk_vb[16], m_hk_stream_seen, m_hk_ib, m_hk_base, m_hk_ib_seen;
void d3d8m_set_stream_source(uint32_t stream, uint32_t vb, uint32_t stride)
{
    if (stream < 16) { m_hk_vb[stream] = vb; m_hk_stride[stream] = stride; m_hk_stream_seen |= 1u << stream; }
}
void d3d8m_set_indices(uint32_t ib, uint32_t base) { m_hk_ib = ib; m_hk_base = base; m_hk_ib_seen = 1; }

/* G41: D3D's own stream/index state at the draw, and what its array setup
 * (sub_00196520, called by both draws) derives from it per NV2A array slot:
 *   D3D_g_Stream[16] at 0x19DCE8, 12 bytes: Stride, Offset, pVertexBuffer
 *   device +0x380   the vertex shader object; obj+4 bit 0x10 selects the
 *                   slot->attribute table at 0x22E554 (+0x10 when set)
 *   obj + 16*attr + 0x14/0x18/0x1C  the attribute's stream, offset, format
 *   0x1720+4i = pVB->Data + attr.offset + Stream.Offset + base*Stride
 *   0x1760+4i = Stride << 8 | attr.format      (format 0x02 = disabled)
 * base is device +0x1C for DrawIndexedVertices and 0 for DrawVertices. */
static void d3d8m_streams(D3D8HostDrawCheck *c, uint32_t kind, uint32_t a1, uint32_t a2, uint32_t a3)
{
    uint32_t d = MEM32(0x0019DCE0u), obj, tbl;
    c->draw_kind = kind; c->prim = a1;
    c->base_vertex = MEM32(d + 0x1Cu); c->ib = MEM32(d + 0x38Cu); c->ib_data = MEM32(0x0019DED4u);
    if (kind == 2) {                 /* DrawIndexedVertices(prim, count, pIndexData) */
        c->count = a2;
        c->nidx = a2 < D3D8_HOST_IDX_N ? a2 : D3D8_HOST_IDX_N;
        for (uint32_t k = 0; k < c->nidx; ++k) c->idx[k] = MEM16(a3 + 2u * k);
    } else {                         /* DrawVertices(prim, start, count) */
        c->start = a2; c->count = a3;
        c->nidx = a3 < D3D8_HOST_IDX_N ? a3 : D3D8_HOST_IDX_N;
        for (uint32_t k = 0; k < c->nidx; ++k) c->idx[k] = (uint16_t)(a2 + k);
    }
    for (unsigned s = 0; s < 16; ++s) {
        c->st_stride[s] = MEM32(0x0019DCE8u + 12u * s);
        c->st_offset[s] = MEM32(0x0019DCECu + 12u * s);
        c->st_vb[s]     = MEM32(0x0019DCF0u + 12u * s);
        c->st_data[s]   = c->st_vb[s] ? MEM32(c->st_vb[s] + 4u) : 0;
    }
    obj = MEM32(d + 0x380u);
    if (obj) {
        uint32_t base = kind == 2 ? c->base_vertex : 0;
        tbl = 0x0022E554u + (MEM32(obj + 4u) & 0x10u);
        for (unsigned i = 0; i < 16; ++i) {
            uint32_t at = obj + 16u * MEM8(tbl + i), s = MEM32(at + 0x14u) & 15u, fmt = MEM32(at + 0x1Cu);
            c->va_stream[i] = MEM32(at + 0x14u);
            c->va_format[i] = (c->st_stride[s] << 8) + fmt;
            if (fmt == 2u || !c->st_vb[s]) continue;
            c->va_offset[i] = c->st_data[s] + MEM32(at + 0x18u) + c->st_offset[s] + base * c->st_stride[s];
            if ((fmt >> 4) & 0xFu) c->va_on |= 1u << i;
        }
    }
    memcpy(c->hk_stride, m_hk_stride, sizeof c->hk_stride); memcpy(c->hk_vb, m_hk_vb, sizeof c->hk_vb);
    c->hk_stream_seen = m_hk_stream_seen; c->hk_ib = m_hk_ib; c->hk_base = m_hk_base; c->hk_ib_seen = m_hk_ib_seen;
}

/* G43: the fixed-function combiner builder's inputs (ff_combiner_notes.md):
 * TSS words at 0x19DEE0 + 0x80*stage, RenderState[108] POINTSPRITEENABLE and
 * [93] SPECULARENABLE (0x19E0E0 + 4*index, XDK 4134 numbering), m_Textures
 * NULL-ness (device +0xA78), device +0x370 (pixel shader) and device +8. */
static void d3d8m_ffc_read(D3D8FFCombinerIn *in)
{
    uint32_t d = MEM32(0x0019DCE0u);
    memset(in, 0, sizeof *in);
    for (unsigned s = 0; s < 4; ++s) {
        for (unsigned k = 0; k < 32; ++k) in->tss[s][k] = MEM32(0x0019DEE0u + 4u * (32u * s + k));
        if (MEM32(d + 0xA78u + 4u * s)) in->texture_bound_mask |= 1u << s;
    }
    in->point_sprite_enable = MEM32(0x0019E0E0u + 4u * D3D8FF_RS_POINTSPRITEENABLE);
    in->specular_enable     = MEM32(0x0019E0E0u + 4u * D3D8FF_RS_SPECULARENABLE);
    in->pixel_shader        = MEM32(d + 0x370u);
    in->device_flags        = MEM32(d + 8u);
}
/* The fog updater's inputs: RenderState[82] FOGENABLE, [93], device +0x370/+0x374. */
static void d3d8m_fog_read(uint32_t f[4])
{
    uint32_t d = MEM32(0x0019DCE0u);
    f[0] = MEM32(0x0019E0E0u + 4u * D3D8FF_RS_FOGENABLE);
    f[1] = MEM32(0x0019E0E0u + 4u * D3D8FF_RS_SPECULARENABLE);
    f[2] = MEM32(d + 0x370u); f[3] = MEM32(d + 0x374u);
}
static D3D8FFCombinerIn m_ffc_emit;
static uint32_t m_ffc_emit_seen, m_ffc_emits, m_ffc_emits_at_draw, m_fog_emit[4], m_fog_emit_seen;

/* ---- G42: fixed-function vertex state (ff_lighting_fog_notes.md) ---- */
static uint32_t m_seq, m_sp_seq, m_sp_val;
static D3D8FFTexXformIn m_tx_emit;  static uint32_t m_tx_emit_seen, m_tx_emits, m_tx_emits_at_draw;
static D3D8FFFogIn      m_fg_emit;  static uint32_t m_fg_emit_seen, m_fg_emits, m_fg_emits_at_draw;
static D3D8FFLightIn    m_lt_emit;  static uint32_t m_lt_emit_seen, m_lt_emits, m_lt_emits_at_draw, m_lt_seq;
#define RS(i) MEM32(0x0019E0E0u + 4u * (uint32_t)(i))
static uint32_t d3d8m_vs_flags(uint32_t d)
{
    uint32_t o = MEM32(d + 0x380u);
    return o ? MEM32(o + 4u) : 0;
}
/* 0x1957F0's inputs: the vertex shader object's flags (+4) and texcoord
 * sizes (+0x10), TEXTURETRANSFORMFLAGS / TEXCOORDINDEX per stage, and the
 * texture matrices as SetTransform stored them (device +0x750 + 0x40*state,
 * TEXTURE0 = state 2). */
static void d3d8m_tx_read(D3D8FFTexXformIn *in)
{
    uint32_t d = MEM32(0x0019DCE0u), o = MEM32(d + 0x380u);
    memset(in, 0, sizeof *in);
    in->vs_flags = o ? MEM32(o + 4u) : 0;
    in->vs_tex_sizes = o ? MEM32(o + 0x10u) : 0;
    for (unsigned s = 0; s < 4; ++s) {
        in->ttf[s] = MEM32(0x0019DEE0u + 4u * (32u * s + D3D8FF_TSS_TEXTURETRANSFORMFLAGS));
        in->tci[s] = MEM32(0x0019DEE0u + 4u * (32u * s + D3D8FF_TSS_TEXCOORDINDEX));
        for (unsigned i = 0; i < 16; ++i) in->matrix[s][i] = MEM32(d + 0x7D0u + 0x40u * s + 4u * i);
    }
}
/* 0x195610's fog inputs: RenderState[82..87] and the word at 0x19B0F4. */
static void d3d8m_fg_read(D3D8FFFogIn *in)
{
    memset(in, 0, sizeof *in);
    in->enable = RS(D3D8FF_RS_FOGENABLE); in->table_mode = RS(D3D8FF_RS_FOGTABLEMODE);
    in->start = RS(D3D8FF_RS_FOGSTART); in->end = RS(D3D8FF_RS_FOGEND); in->density = RS(D3D8FF_RS_FOGDENSITY);
    in->range_enable = RS(D3D8FF_RS_RANGEFOGENABLE);
    in->equal_scale = MEM32(0x0019B0F4u);
}
/* 0x195F80's inputs: RenderState[92..105] and [122], device +8, the front
 * and back materials (+0x9F0, +0xA34), VIEW (+0x750), the eye vector at
 * 0x19B0F8, and the enabled-light list (head +0x398, next at record +0x8C,
 * 0x90-byte records), at most eight as the guest takes them. */
static void d3d8m_lt_read(D3D8FFLightIn *in)
{
    uint32_t d = MEM32(0x0019DCE0u), p;
    memset(in, 0, sizeof *in);
    in->vs_flags = d3d8m_vs_flags(d);
    in->lighting = RS(D3D8FF_RS_LIGHTING); in->specular_enable = RS(D3D8FF_RS_SPECULARENABLE);
    in->local_viewer = RS(D3D8FF_RS_LOCALVIEWER); in->color_vertex = RS(D3D8FF_RS_COLORVERTEX);
    for (unsigned k = 0; k < 8; ++k) in->mat_source[k] = RS(D3D8FF_RS_BACKSPECULARMATERIALSOURCE + (int)k);
    in->back_ambient = RS(D3D8FF_RS_BACKAMBIENT); in->ambient = RS(D3D8FF_RS_AMBIENT);
    in->two_sided = RS(D3D8FF_RS_TWOSIDEDLIGHTING);
    in->device_flags = MEM32(d + 8u);
    for (unsigned i = 0; i < 17; ++i) { in->material[i] = MEM32(d + 0x9F0u + 4u * i); in->back_material[i] = MEM32(d + 0xA34u + 4u * i); }
    for (unsigned i = 0; i < 16; ++i) in->view[i] = MEM32(d + 0x750u + 4u * i);
    for (unsigned i = 0; i < 3; ++i) in->eye[i] = MEM32(0x0019B0F8u + 4u * i);
    in->list_head = p = MEM32(d + 0x398u);
    while (p && in->nlights < 8u) {
        for (unsigned i = 0; i < D3D8FF_LIGHT_WORDS; ++i) in->light[in->nlights][i] = MEM32(p + 4u * i);
        in->nlights++;
        p = MEM32(p + 0x8Cu);
    }
}
/* The transcription hard-codes the XBE's read-only constants; the two it
 * takes from D3D's writable data (the rsqrt pair) are checked once here. */
static void d3d8m_g42_constants(void)
{
    static int done;
    if (done) return;
    done = 1;
    fprintf(stderr, "[D3D8-MIRROR] G42 constants: rsqrt %08X %08X (want 3EF0A3D7 3FBC28F6), fog equal-range scale %08X,"
                    " eye %08X %08X %08X%s\n", MEM32(0x0022E574u), MEM32(0x0022E578u), MEM32(0x0019B0F4u),
            MEM32(0x0019B0F8u), MEM32(0x0019B0FCu), MEM32(0x0019B100u),
            MEM32(0x0022E574u) == 0x3EF0A3D7u && MEM32(0x0022E578u) == 0x3FBC28F6u ? "" : " -- RSQRT CONSTANTS DIFFER");
}
/* Hooked on entry to 0x1957F0. With a programmable or pass-through shader
 * (object flags 0x12) it returns at once: not an emission. */
void d3d8m_texxform_entry(void)
{
    D3D8FFTexXformIn in;
    if (!d3d8m_on()) return;
    d3d8m_tx_read(&in);
    if (in.vs_flags & 0x12u) return;
    m_tx_emit = in; m_tx_emit_seen = 1; ++m_tx_emits;
}
/* Hooked on entry to 0x195F80. It always writes (the unlit path is four
 * registers), so every call is an emission. */
void d3d8m_lights_entry(void)
{
    if (!d3d8m_on()) return;
    d3d8m_g42_constants();
    d3d8m_lt_read(&m_lt_emit);
    m_lt_emit_seen = 1; ++m_lt_emits; m_lt_seq = ++m_seq;
}
/* G42b: hooked on entry to the transform updater 0x1962B0 (dirty 0x200).
 * Every call that gets past its two early-outs writes MODELVIEW; only those
 * with lighting or an eye-normal texgen also write INVERSE_MODELVIEW 0x0580,
 * and those are the emissions the executor's 0x580 comes from. Both are
 * numbered, so the check knows when MODELVIEW came from the same one. */
static D3D8FFInvMVIn m_imv_emit;
static uint32_t m_imv_emit_seen, m_imv_emits, m_imv_emits_at_draw, m_imv_seq, m_imv_any_seq;
static void d3d8m_imv_read(D3D8FFInvMVIn *in)
{
    uint32_t d = MEM32(0x0019DCE0u);
    memset(in, 0, sizeof *in);
    in->dirty = MEM32(0x0019DED8u);
    in->vs_flags = d3d8m_vs_flags(d);
    in->eye_normal_mask = MEM32(d + 0x450u);
    in->lighting = RS(D3D8FF_RS_LIGHTING); in->normalize = RS(D3D8FF_RS_NORMALIZENORMALS);
    in->vertex_blend = RS(D3D8FF_RS_VERTEXBLEND);
    for (unsigned i = 0; i < 16; ++i) { in->world[i] = MEM32(d + 0x8D0u + 4u * i); in->view[i] = MEM32(d + 0x750u + 4u * i); }
}
void d3d8m_xform_entry(void)
{
    D3D8FFInvMVIn in;
    if (!d3d8m_on()) return;
    d3d8m_imv_read(&in);
    if ((in.dirty & 0x80000000u) || (in.vs_flags & 0x12u)) return;       /* 0x1962C6, 0x1962D5 */
    m_imv_any_seq = ++m_seq;
    if (!in.eye_normal_mask && !in.lighting) return;                     /* 0x19631E: no 0x580 */
    m_imv_emit = in; m_imv_emit_seen = 1; ++m_imv_emits; m_imv_seq = m_imv_any_seq;
}
/* Hooked on entry to 0x197F90, before it runs. With a pixel shader bound it
 * returns at once and writes nothing, so that call is not an emission and the
 * registers still hold the previous one's words. */
void d3d8m_ff_builder_entry(void)
{
    D3D8FFCombinerIn in;
    if (!d3d8m_on()) return;
    d3d8m_ffc_read(&in);
    if (in.pixel_shader) return;
    m_ffc_emit = in; m_ffc_emit_seen = 1; ++m_ffc_emits;
    {   /* G42: the builder's own SPECULAR_ENABLE write, when it makes one. */
        D3D8FFCombiners o;
        d3d8_ff_combiners(&in, &o);
        if (o.specular_enable_emitted) { m_sp_seq = ++m_seq; m_sp_val = o.specular_enable; }
    }
}
/* Hooked on entry to 0x195610. It skips CW0/CW1 when device +0x370 and +0x374
 * are both nonzero; only a call that writes them is recorded. */
void d3d8m_fog_entry(void)
{
    uint32_t f[4];
    if (!d3d8m_on()) return;
    /* G42: the fog registers (FOG_ENABLE, and mode/params when on) are
     * written on every call, CW or not. */
    d3d8m_fg_read(&m_fg_emit); m_fg_emit_seen = 1; ++m_fg_emits;
    d3d8m_fog_read(f);
    if (f[2] && f[3]) return;
    memcpy(m_fog_emit, f, sizeof f); m_fog_emit_seen = 1;
}

/* Write a host token into the guest ring at the current put pointer. */
static void d3d8m_put_token(uint32_t tok)
{
    uint32_t dev = MEM32(0x0019DCE0u), put = MEM32(dev);
    if (put >= MEM32(dev + 4u)) {                  /* the XDK's own reservation, as Clear uses it */
        PUSH32(esp, 0x0019932Cu);
        RECOMP_ABI_CALL(0x001916B0u, sub_001916B0);
        put = eax;
    }
    RECOMP_MEM_WRITE32(0x0019932Eu, 0x001993A0u, put,
                       (1u << 18) | (NV2A_HOST_TOKEN_SUBCHANNEL << 13) | NV2A_HOST_TOKEN_METHOD);
    RECOMP_MEM_WRITE32(0x0019932Eu, 0x001993A0u, put + 4u, tok);
    RECOMP_MEM_WRITE32(0x00199399u, 0x001993A0u, dev, put + 8u);
}

/* G51.1: before a pre-transformed 2D draw runs, a token that lets the host
 * snapshot the render target the draw starts from. The vertex shader handle
 * and render target are D3D's at this moment, which the draw does not change
 * before it emits. The serial is the one d3d8m_after_draw will give it. */
static unsigned long long m_pre_no_token;
void d3d8m_before_draw(uint32_t kind, uint32_t a1, uint32_t a2, uint32_t a3)
{
    uint32_t d, rt, zs, tok;
    (void)kind; (void)a1; (void)a2; (void)a3;
    if (!d3d8m_on() || !d3d8_host_2d_mode()) return;
    d = MEM32(0x0019DCE0u);
    if (!d3d8_host_2d_is_fvf_xyzrhw(MEM32(d + 0x384u))) return;
    rt = MEM32(d + 0x2070u); zs = MEM32(d + 0x2074u);
    tok = d3d8_host_enqueue_2d_pre(m_serial + 1u, rt ? MEM32(rt + 4u) : 0u, rt ? MEM32(rt + 0xCu) : 0u,
                                   rt ? MEM32(rt + 0x10u) : 0u, zs ? MEM32(zs + 4u) : 0u, zs ? MEM32(zs + 0x10u) : 0u);
    if (!tok) {
        if (m_pre_no_token++ < 4)
            fprintf(stderr, "[D3D8-MIRROR] G51.1: host token queue full before draw %u; the host skips it\n", m_serial + 1u);
        return;
    }
    d3d8m_put_token(tok);
}

void d3d8m_after_draw(uint32_t kind, uint32_t a1, uint32_t a2, uint32_t a3)
{
    D3D8HostDrawCheck c;
    uint32_t tok;
    if (!d3d8m_on()) return;
    memset(&c, 0, sizeof c);
    c.serial = ++m_serial;
    d3d8m_streams(&c, kind, a1, a2, a3);
    /* G51.1: every index, not only the first 16, and the extra states. */
    c.idx_ptr = kind == 2 ? a3 : 0u;
    memcpy(c.x_val, m_x_val, sizeof c.x_val); c.x_seen = m_x_seen;
    /* G51.1: for a 2D draw the host will draw, the indices and a hash of the
     * vertex bytes AS THE CALL SAW THEM. D3D has just copied these indices
     * into the ring; the title rewrites pIndexData for its next draw long
     * before the token is reached. */
    if (d3d8_host_2d_mode() && d3d8_host_2d_is_fvf_xyzrhw(MEM32(MEM32(0x0019DCE0u) + 0x384u))) {
        extern ptrdiff_t xbox_GetMemoryOffset(void);
        uint32_t imin = kind == 2 ? 0xFFFFFFFFu : a2, imax = kind == 2 ? 0u : a2 + (a3 ? a3 - 1u : 0u);
        if (kind == 2 && a2) {
            uint64_t pos; uint16_t *dst = d3d8_host_2d_idx_reserve(a2, &pos);
            if (!dst) c.idx_snap_over = 1;
            else {
                for (uint32_t k = 0; k < a2; ++k) {
                    uint16_t v = MEM16(a3 + 2u * k);
                    dst[k] = v; if (v < imin) imin = v; if (v > imax) imax = v;
                }
                d3d8_host_2d_idx_publish(pos, a2);
                c.idx_snap_pos = pos; c.idx_snap_n = a2;
            }
        }
        if (imin <= imax) {
            c.vtx_hash = d3d8_host_2d_vertex_hash((const uint8_t *)xbox_GetMemoryOffset(), 0x04000000u, &c, imin, imax);
            c.vtx_hash_ok = 1;
        }
    }
    /* G43: the combiner inputs now (after the draw, so after its flush), and
     * as the builder and fog updater last saw them when they emitted. */
    c.ffc_valid = 1;
    d3d8m_ffc_read(&c.ffc_cur);
    c.ffc_ps = c.ffc_cur.pixel_shader;
    c.ffc_emit_seen = m_ffc_emit_seen; c.ffc_emit = m_ffc_emit;
    c.ffc_emit_fresh = m_ffc_emits != m_ffc_emits_at_draw; m_ffc_emits_at_draw = m_ffc_emits;
    c.tfactor = MEM32(0x0019E0E0u + 4u * D3D8FF_RS_TEXTUREFACTOR);
    d3d8m_fog_read(c.fog_cur);
    c.fog_emit_seen = m_fog_emit_seen; memcpy(c.fog_emit, m_fog_emit, sizeof c.fog_emit);
    /* G42: the vertex-state updaters' inputs as last emitted, and now. */
    c.ffv_valid = 1;
    c.ffv_vs_flags = d3d8m_vs_flags(MEM32(0x0019DCE0u));
    c.ffv_fog_color = RS(D3D8FF_RS_FOGCOLOR);
    c.tx_emit_seen = m_tx_emit_seen; c.tx_emit = m_tx_emit; d3d8m_tx_read(&c.tx_cur);
    c.tx_emit_fresh = m_tx_emits != m_tx_emits_at_draw; m_tx_emits_at_draw = m_tx_emits;
    c.fg_emit_seen = m_fg_emit_seen; c.fg_emit = m_fg_emit; d3d8m_fg_read(&c.fg_cur);
    c.fg_emit_fresh = m_fg_emits != m_fg_emits_at_draw; m_fg_emits_at_draw = m_fg_emits;
    c.lt_emit_seen = m_lt_emit_seen; c.lt_emit = m_lt_emit; d3d8m_lt_read(&c.lt_cur);
    c.lt_emit_fresh = m_lt_emits != m_lt_emits_at_draw; m_lt_emits_at_draw = m_lt_emits;
    c.lt_emit_seq = m_lt_seq; c.sp_emit_seq = m_sp_seq; c.sp_emit_val = m_sp_val;
    c.imv_emit_seen = m_imv_emit_seen; c.imv_emit = m_imv_emit; d3d8m_imv_read(&c.imv_cur);
    c.imv_emit_fresh = m_imv_emits != m_imv_emits_at_draw; m_imv_emits_at_draw = m_imv_emits;
    c.imv_emit_seq = m_imv_seq; c.imv_any_seq = m_imv_any_seq;
    /* G40: the bound textures come from the DEVICE, not from a SetTexture
     * hook. XbSymbolDatabase's offset dump names m_Textures at device +0xA78,
     * one pointer per stage. The hook mirror is kept beside it and every
     * disagreement counted, which is what explains -- or removes -- the four
     * texture mismatches G39 left open. */
    {
        static unsigned long long agree, differ, printed;
        uint32_t d = MEM32(0x0019DCE0u);
        for (unsigned u = 0; u < 4; ++u) {
            uint32_t t = MEM32(d + 0xA78u + 4u * u);
            if (t == m_tex[u]) agree++;
            else {
                differ++;
                if (printed++ < 16)
                    fprintf(stderr, "[D3D8-MIRROR] G40 draw %u stage %u: device m_Textures=%08X, SetTexture hook=%08X\n",
                            m_serial, u, t, m_tex[u]);
            }
            c.tex[u] = t;
            if (t) { c.data[u] = MEM32(t + 4u); c.format[u] = MEM32(t + 0xCu); c.size[u] = MEM32(t + 0x10u); }
        }
        if ((m_serial % 200000u) == 0)
            fprintf(stderr, "[D3D8-MIRROR] G40 textures: device vs hook agree=%llu differ=%llu (stage-draws)\n",
                    agree, differ);
    }
    {   uint32_t d = MEM32(0x0019DCE0u);            /* D3D_g_pDevice */
        c.rt = MEM32(d + 0x2070u); c.zs = MEM32(d + 0x2074u);
        if (c.rt) { c.rt_data = MEM32(c.rt + 4u); c.rt_format = MEM32(c.rt + 0xCu); c.rt_size = MEM32(c.rt + 0x10u); }
        if (c.zs) { c.zs_data = MEM32(c.zs + 4u); c.zs_format = MEM32(c.zs + 0xCu); c.zs_size = MEM32(c.zs + 0x10u); }
        c.vp_x = (int32_t)MEM32(d + 0x9D0u); c.vp_y = (int32_t)MEM32(d + 0x9D4u);
        c.vp_w = (int32_t)MEM32(d + 0x9D8u); c.vp_h = (int32_t)MEM32(d + 0x9DCu);
        { uint32_t u; u = MEM32(d + 0x9E0u); memcpy(&c.vp_minz, &u, 4); u = MEM32(d + 0x9E4u); memcpy(&c.vp_maxz, &u, 4);
          u = MEM32(d + 0x454u); memcpy(&c.ss_x, &u, 4); u = MEM32(d + 0x458u); memcpy(&c.ss_y, &u, 4); } }
    memcpy(c.st_val, m_state_val, sizeof c.st_val); c.st_seen = m_state_seen;
    memcpy(c.vc, m_vc, sizeof c.vc); memcpy(c.vc_written, m_vc_written, sizeof c.vc_written);
    for (unsigned u = 0; u < 4; ++u)
        for (unsigned k = 0; k < 32; ++k) c.tss[u][k] = MEM32(0x0019DEE0u + 4u * (32u * u + k));
    memcpy(c.xf_world, m_xf[0], sizeof c.xf_world); memcpy(c.xf_view, m_xf[1], sizeof c.xf_view);
    memcpy(c.xf_proj, m_xf[2], sizeof c.xf_proj); c.xf_seen = m_xf_seen;
    {   uint32_t d = MEM32(0x0019DCE0u), h = MEM32(d + 0x384u);
        c.vs_handle = h;

        /* Object flag 0x10 is SetVertexShader's LoadVertexShader(h, 0) +
         * SelectVertexShader path, i.e. the program is loaded at slot 0. Measured
         * 23 Sep: every programmable object this title binds carries it. */
        if ((h & 1u) && (MEM32(h - 1u + 4u) & 0x10u)) {
            uint32_t obj = h - 1u, n = MEM32(obj + 0xCu), at = 0;
            c.vs_kind = 1;
            while (at < n && n < 4096u) {
                uint32_t hdr = MEM32(obj + 0x114u + 4u * at), cnt = (hdr >> 18) & 0x7FFu, meth = hdr & 0x1FFCu;
                if ((hdr & 0xE0030003u) != 0 || !cnt || at + 1u + cnt > n) { c.vs_kind = 2; break; }
                if (meth >= 0x0B00u && meth < 0x0B80u)
                    for (uint32_t i = 0; i < cnt && c.vs_nwords < 136u * 4u; ++i)
                        c.vs_words[c.vs_nwords++] = MEM32(obj + 0x114u + 4u * (at + 1u + i));
                at += 1u + cnt;
            }
        } }
    if (m_ps_handle) {
        /* D3D's own current view of the pixel-shader registers: SetPixelShader
         * copies the 57-word definition into D3D_g_RenderState[0..56]
         * (0x19E0E0), and SetPixelShaderConstant's per-stage constant writes go
         * through SetRenderStateNotInline into the same array. The definition
         * alone carries placeholders there (23 Sep: word 13 == 1 on every
         * programmable draw, the executor 0). */
        c.ps_bound = 1;
        for (unsigned k = 0; k < 57; ++k) c.ps[k] = MEM32(0x0019E0E0u + 4u * k);
    }
        {   /* Positive control for the surface check: RECOMP_D3D8_MIRROR_CONTROL=1
         * swaps the colour and depth addresses, so every draw MUST mismatch. */
        static int ctl = -1;
        if (ctl < 0) { const char *e = getenv("RECOMP_D3D8_MIRROR_CONTROL"); ctl = e && e[0] == '1';
                       if (ctl) fprintf(stderr, "[D3D8-MIRROR] POSITIVE CONTROL: colour/depth addresses swapped\n"); }
        if (ctl) { uint32_t t = c.rt_data; c.rt_data = c.zs_data; c.zs_data = t;
                   c.vp_x += 1; c.vp_minz += 0.5f;                      /* viewport must mismatch */
                   for (unsigned k = 0; k < 11; ++k) c.st_val[k] ^= 1u;       /* every state too */
                   for (unsigned k = 0; k < 192; ++k) c.vc[k][0] += 1.0f;     /* and every constant */
                   for (unsigned k = 0; k < 57; ++k) c.ps[k] ^= 0x1u;          /* and every shader word */
                   for (unsigned u = 0; u < 4; ++u) c.tss[u][0] ^= 0x2u;     /* and every stage's address */
                   if (c.vs_nwords) c.vs_words[0] ^= 1u;                     /* and every program */
                   c.xf_world[12] += 10.0f;                                   /* and the world translation */
                   /* G41: the first enabled array's stream moves 1 MB, so every array that
                    * stream feeds misses both the exact and the inside-a-stream
                    * test; and the first index (or the run start) is off by one. */
                   if (c.va_on) {
                       unsigned s0 = c.va_stream[__builtin_ctz(c.va_on)] & 15u;
                       c.st_data[s0] += 0x100000u;
                       for (unsigned i = 0; i < 16; ++i) if ((c.va_stream[i] & 15u) == s0) c.va_offset[i] += 0x100000u; }
                   if (c.nidx) c.idx[0] ^= 1u;
                   /* G43: the check flips one transcribed word (COLOR_ICW[0]),
                    * so every fixed-function draw must mismatch. */
                   c.ffc_control = 1;
                   /* G42: one word per group (texgen S0, TEXTURE_MATRIX_ENABLE0,
                    * LIGHTING_ENABLE or LIGHT_CONTROL, FOG_ENABLE, and
                    * INVERSE_MODELVIEW[0]), so every compared draw must mismatch. */
                   c.ffv_control = 1; } }
        tok = d3d8_host_enqueue_check(&c);
    if (!tok) { ++m_no_token; return; }
    d3d8m_put_token(tok);
    if (m_serial % 20000u == 0) d3d8_host_report("mirror");
}
