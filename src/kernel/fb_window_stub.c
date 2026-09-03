/*
 * Framebuffer window: portable no-op.
 *
 * The executor in nv2a_pb_exec.c rasterises into the GUEST framebuffer and
 * then asks for a window to show it. Upstream's src/video/fb_present.c does
 * that with CreateWindowExA, which is Win32 only, and this host has no
 * equivalent yet -- the SDL window here belongs to the D3D8 GL backend and is
 * driven from a different thread.
 *
 * Stubbing the window does NOT stub the rasteriser. The executor still decodes
 * the ring, tracks the surface and draws triangles into guest memory, and the
 * result is inspectable with RECOMP_FB_DUMP=<prefix>, which writes the surface
 * to <prefix>NNN.bmp. That is enough to answer "does it draw", which is the
 * question worth answering before porting a window.
 *
 * The obvious upgrade is to blit the guest framebuffer into the existing GL
 * window as a texture, at which point these become real.
 */

#include <stdint.h>

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    (void)fb_va;
    (void)pitch;
}

void xbox_FramebufferWindowStart(void)
{
}
