/* See nv2a_texture_decode.h for why this exists and what it must match. */

#include "nv2a_texture_decode.h"

#include <string.h>

unsigned nv2a_texture_morton(unsigned x, unsigned y, unsigned w, unsigned h)
{
    /* Transcribed from morton() in nv2a_metal.m. The interleave stops
     * contributing bits for an axis once the run length passes that axis's
     * extent, which is what makes non-square textures work. */
    unsigned index = 0, bit = 0, b;
    for (b = 1; b < w || b < h; b <<= 1) {
        if (b < w) { if (x & b) index |= 1u << bit; ++bit; }
        if (b < h) { if (y & b) index |= 1u << bit; ++bit; }
    }
    return index;
}

static unsigned blocks(unsigned n) { return (n + 3u) / 4u; }

size_t nv2a_texture_level_bytes(int fmt, unsigned w, unsigned h,
                                unsigned pitch)
{
    switch (fmt) {
    case NV2A_TEXFMT_DXT1:
        return (size_t)blocks(w) * blocks(h) * 8u;
    case NV2A_TEXFMT_DXT3:
        return (size_t)blocks(w) * blocks(h) * 16u;
    case NV2A_TEXFMT_RGBA8:
    case NV2A_TEXFMT_RGBA8_ALT:
        return (size_t)w * h * 4u;
    default:
        return (size_t)pitch * h;
    }
}

unsigned nv2a_texture_next_pitch(int fmt, unsigned w, unsigned pitch)
{
    unsigned nw = w / 2u; if (!nw) nw = 1u;
    switch (fmt) {
    case NV2A_TEXFMT_DXT1:            return blocks(nw) * 8u;
    case NV2A_TEXFMT_DXT3:            return blocks(nw) * 16u;
    case NV2A_TEXFMT_RGBA8:
    case NV2A_TEXFMT_RGBA8_ALT:       return nw * 4u;
    default:                          return pitch;
    }
}

/* 565 -> 8888, by the shader's arithmetic: each channel is scaled by its own
 * maximum, not by a bit replication. Rounded rather than truncated so the
 * byte matches what the shader's float would quantise to. */
static void unpack565(unsigned c, unsigned char *r, unsigned char *g,
                      unsigned char *b)
{
    *r = (unsigned char)(((c >> 11) & 31u) * 255u / 31u);
    *g = (unsigned char)(((c >> 5)  & 63u) * 255u / 63u);
    *b = (unsigned char)(( c        & 31u) * 255u / 31u);
}

static void put(uint8_t *dst, unsigned w, unsigned x, unsigned y,
                unsigned char r, unsigned char g, unsigned char b,
                unsigned char a)
{
    uint8_t *p = dst + ((size_t)y * w + x) * 4u;
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

int nv2a_texture_decode_rgba8(const uint8_t *src, size_t src_size,
                              unsigned w, unsigned h, unsigned pitch,
                              int fmt, uint8_t *dst)
{
    unsigned x, y;

    if (!src || !dst || !w || !h) return 0;
    if (nv2a_texture_level_bytes(fmt, w, h, pitch) > src_size) return 0;

    /* Validate every source address before touching dst. The level's nominal
     * byte count alone is insufficient: a malformed pitch or a non-power-of-
     * two Morton layout can put a later texel beyond the supplied buffer. */
    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x) {
            size_t at, bytes;
            if (fmt == NV2A_TEXFMT_RGBA8 || fmt == NV2A_TEXFMT_RGBA8_ALT) {
                at = (size_t)4u * nv2a_texture_morton(x, y, w, h);
                bytes = 4u;
            } else if (fmt == NV2A_TEXFMT_DXT1 || fmt == NV2A_TEXFMT_DXT3) {
                bytes = fmt == NV2A_TEXFMT_DXT1 ? 8u : 16u;
                at = (size_t)(y / 4u) * pitch + (size_t)(x / 4u) * bytes;
            } else {
                at = (size_t)y * pitch + (size_t)x * 2u;
                bytes = 2u;
            }
            if (at > src_size || bytes > src_size - at) return 0;
        }

    switch (fmt) {

    case NV2A_TEXFMT_RGBA8:
    case NV2A_TEXFMT_RGBA8_ALT:
        /* Stored BGRA and Morton-swizzled. The shader reads
         * (t[at+2], t[at+1], t[at], t[at+3]) into RGBA. */
        for (y = 0; y < h; ++y)
            for (x = 0; x < w; ++x) {
                size_t at = (size_t)4u * nv2a_texture_morton(x, y, w, h);
                if (at + 3u >= src_size) return 0;
                put(dst, w, x, y, src[at + 2], src[at + 1], src[at],
                    src[at + 3]);
            }
        return 1;

    case NV2A_TEXFMT_DXT1:
        for (y = 0; y < h; ++y)
            for (x = 0; x < w; ++x) {
                size_t at = (size_t)(y / 4u) * pitch + (size_t)(x / 4u) * 8u;
                unsigned c0, c1, pick;
                unsigned char r0,g0,b0,r1,g1,b1;
                if (at + 7u >= src_size) return 0;
                c0 = (unsigned)src[at]     | ((unsigned)src[at + 1] << 8);
                c1 = (unsigned)src[at + 2] | ((unsigned)src[at + 3] << 8);
                pick = ((unsigned)src[at + 4]
                      | ((unsigned)src[at + 5] << 8)
                      | ((unsigned)src[at + 6] << 16)
                      | ((unsigned)src[at + 7] << 24))
                       >> (2u * ((y & 3u) * 4u + (x & 3u))) & 3u;
                unpack565(c0, &r0, &g0, &b0);
                unpack565(c1, &r1, &g1, &b1);
                if (pick == 0)      put(dst, w, x, y, r0, g0, b0, 255);
                else if (pick == 1) put(dst, w, x, y, r1, g1, b1, 255);
                else if (c0 <= c1 && pick == 3) {
                    /* float4(0): transparent BLACK, alpha included. */
                    put(dst, w, x, y, 0, 0, 0, 0);
                } else {
                    /* The shader mixes x=c0 and y=c1 as x*wt + y*(1-wt). */
                    double wt = (c0 <= c1) ? 0.5
                              : (pick == 2 ? 2.0 / 3.0 : 1.0 / 3.0);
                    put(dst, w, x, y,
                        (unsigned char)(r0 * wt + r1 * (1.0 - wt) + 0.5),
                        (unsigned char)(g0 * wt + g1 * (1.0 - wt) + 0.5),
                        (unsigned char)(b0 * wt + b1 * (1.0 - wt) + 0.5),
                        255);
                }
            }
        return 1;

    case NV2A_TEXFMT_DXT3:
        for (y = 0; y < h; ++y)
            for (x = 0; x < w; ++x) {
                size_t at = (size_t)(y / 4u) * pitch + (size_t)(x / 4u) * 16u;
                unsigned i = (y & 3u) * 4u + (x & 3u);
                unsigned alpha, c0, c1, pick;
                unsigned char r0,g0,b0,r1,g1,b1;
                if (at + 15u >= src_size) return 0;
                alpha = ((unsigned)src[at + i / 2u] >> (4u * (i & 1u))) & 15u;
                at += 8u;
                c0 = (unsigned)src[at]     | ((unsigned)src[at + 1] << 8);
                c1 = (unsigned)src[at + 2] | ((unsigned)src[at + 3] << 8);
                pick = ((unsigned)src[at + 4]
                      | ((unsigned)src[at + 5] << 8)
                      | ((unsigned)src[at + 6] << 16)
                      | ((unsigned)src[at + 7] << 24)) >> (2u * i) & 3u;
                unpack565(c0, &r0, &g0, &b0);
                unpack565(c1, &r1, &g1, &b1);
                {
                    unsigned char a8 = (unsigned char)(alpha * 255u / 15u);
                    if (pick == 0)      put(dst, w, x, y, r0,g0,b0, a8);
                    else if (pick == 1) put(dst, w, x, y, r1,g1,b1, a8);
                    else {
                        /* Always the four-colour mode here -- DXT3 has no
                         * punch-through variant, and the shader has no
                         * c0<=c1 branch on this path. */
                        double wt = (pick == 2) ? 2.0 / 3.0 : 1.0 / 3.0;
                        put(dst, w, x, y,
                            (unsigned char)(r0 * wt + r1 * (1.0 - wt) + 0.5),
                            (unsigned char)(g0 * wt + g1 * (1.0 - wt) + 0.5),
                            (unsigned char)(b0 * wt + b1 * (1.0 - wt) + 0.5),
                            a8);
                    }
                }
            }
        return 1;

    default:
        /* The 16-bit fallback: y*pitch + x*2, R5G6B5, alpha forced opaque.
         * The shader takes this branch for every format it does not name,
         * so this does too rather than refusing. */
        for (y = 0; y < h; ++y)
            for (x = 0; x < w; ++x) {
                size_t at = (size_t)y * pitch + (size_t)x * 2u;
                unsigned c;
                unsigned char r,g,b;
                if (at + 1u >= src_size) return 0;
                c = (unsigned)src[at] | ((unsigned)src[at + 1] << 8);
                unpack565(c, &r, &g, &b);
                put(dst, w, x, y, r, g, b, 255);
            }
        return 1;
    }
}
