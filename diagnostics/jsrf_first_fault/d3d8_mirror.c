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
 * thing written, and it never reaches PGRAPH. */
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include "d3d8_host.h"
#include "nv2a_pusher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int d3d8m_on(void)
{
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("RECOMP_D3D8_MIRROR");
        m = e && e[0] && strcmp(e, "0") != 0;
        fprintf(stderr, "[D3D8-MIRROR] RECOMP_D3D8_MIRROR=%s\n", m ? "on" : "off");
    }
    return m;
}
static uint32_t m_tex[4], m_serial;
static const uint32_t m_state_methods[11] = D3D8_HOST_STATE_METHODS;
static uint32_t m_state_val[11], m_state_seen;

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
}
static unsigned long long m_no_token;

void d3d8m_set_texture(uint32_t stage, uint32_t tex)
{
    if (stage < 4) m_tex[stage] = tex;
}

void d3d8m_after_draw(void)
{
    D3D8HostDrawCheck c;
    uint32_t tok, dev, put;
    if (!d3d8m_on()) return;
    memset(&c, 0, sizeof c);
    c.serial = ++m_serial;
    for (unsigned u = 0; u < 4; ++u) {
        uint32_t t = m_tex[u];
        c.tex[u] = t;
        if (t) { c.data[u] = MEM32(t + 4u); c.format[u] = MEM32(t + 0xCu); c.size[u] = MEM32(t + 0x10u); }
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
                   c.xf_world[12] += 10.0f; } }                                /* and the world translation */
        tok = d3d8_host_enqueue_check(&c);
    if (!tok) { ++m_no_token; return; }
    dev = MEM32(0x0019DCE0u); put = MEM32(dev);
    if (put >= MEM32(dev + 4u)) {                  /* the XDK's own reservation, as Clear uses it */
        PUSH32(esp, 0x0019932Cu);
        RECOMP_ABI_CALL(0x001916B0u, sub_001916B0);
        put = eax;
    }
    RECOMP_MEM_WRITE32(0x0019932Eu, 0x001993A0u, put,
                       (1u << 18) | (NV2A_HOST_TOKEN_SUBCHANNEL << 13) | NV2A_HOST_TOKEN_METHOD);
    RECOMP_MEM_WRITE32(0x0019932Eu, 0x001993A0u, put + 4u, tok);
    RECOMP_MEM_WRITE32(0x00199399u, 0x001993A0u, dev, put + 8u);
    if (m_serial % 20000u == 0) d3d8_host_report("mirror");
}
