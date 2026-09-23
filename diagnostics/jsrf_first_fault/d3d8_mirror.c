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
