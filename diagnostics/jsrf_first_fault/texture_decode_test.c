/* G27 step one: does the CPU decoder produce what the SHADER produces?
 *
 * The point of moving guest textures into an MTLTexture is to stop the
 * fragment shader decoding them by hand. That only counts as a win if the
 * pixels are the same afterwards, so this file checks the decoder against the
 * rules transcribed in nv2a_texture_decode.h -- which were read out of
 * texel() in nv2a_metal.m -- and not against the decoder's own code.
 *
 * Each case is built from bytes chosen so the right answer can be written
 * down by hand. Where the shader does something surprising, the surprise is
 * the assertion: DXT1 index 3 under c0 <= c1 is TRANSPARENT black, and DXT3
 * never takes that branch at all. */

#include "../../src/nv2a/nv2a_texture_decode.h"

#include <stdio.h>
#include <string.h>

static int fail;
#define CHECK(c, ...) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); ++fail; } } while (0)

static void px(const uint8_t *d, unsigned w, unsigned x, unsigned y,
               int r, int g, int b, int a, const char *what)
{
    const uint8_t *p = d + ((size_t)y * w + x) * 4;
    CHECK(p[0] == r && p[1] == g && p[2] == b && p[3] == a,
          "%s at (%u,%u): got %u,%u,%u,%u want %d,%d,%d,%d",
          what, x, y, p[0], p[1], p[2], p[3], r, g, b, a);
}

/* ── the swizzle ────────────────────────────────────────────────────────── */

static void test_morton(void)
{
    /* 4x4: bits interleave x0,y0,x1,y1, so (1,0)->1, (0,1)->2, (1,1)->3,
     * (2,0)->4, (3,3)->15. Written out rather than looped. */
    CHECK(nv2a_texture_morton(0,0,4,4) == 0,  "morton(0,0)");
    CHECK(nv2a_texture_morton(1,0,4,4) == 1,  "morton(1,0)");
    CHECK(nv2a_texture_morton(0,1,4,4) == 2,  "morton(0,1)");
    CHECK(nv2a_texture_morton(1,1,4,4) == 3,  "morton(1,1)");
    CHECK(nv2a_texture_morton(2,0,4,4) == 4,  "morton(2,0)");
    CHECK(nv2a_texture_morton(3,3,4,4) == 15, "morton(3,3)");

    /* Non-square is the case that breaks naive interleaves: once b passes h,
     * only x contributes, so an 8x2 texture addresses linearly above y. */
    /* x contributes bit 0 and y bit 1, so these are 1 and 2 the other way
      * round from the obvious guess -- the first version of this test had
      * them swapped and the decoder was right. */
    CHECK(nv2a_texture_morton(1,0,8,2) == 1, "morton(1,0) 8x2");
    CHECK(nv2a_texture_morton(0,1,8,2) == 2, "morton(0,1) 8x2");
    CHECK(nv2a_texture_morton(7,1,8,2) == 15, "morton(7,1) 8x2");

    /* Every texel of a 4x4 must land on a distinct index, or the decode
     * silently drops pixels. A bijection check catches that; spot values
     * do not. */
    {
        int seen[16]; unsigned x, y; memset(seen, 0, sizeof seen);
        for (y = 0; y < 4; ++y) for (x = 0; x < 4; ++x) {
            unsigned m = nv2a_texture_morton(x, y, 4, 4);
            CHECK(m < 16 && !seen[m], "morton not a bijection at (%u,%u)=%u",
                  x, y, m);
            if (m < 16) seen[m] = 1;
        }
    }
}

/* ── mip arithmetic ─────────────────────────────────────────────────────── */

static void test_levels(void)
{
    CHECK(nv2a_texture_level_bytes(NV2A_TEXFMT_DXT1, 8, 8, 0) == 32,
          "dxt1 8x8 should be 4 blocks x 8 bytes");
    CHECK(nv2a_texture_level_bytes(NV2A_TEXFMT_DXT3, 8, 8, 0) == 64,
          "dxt3 8x8 should be 4 blocks x 16 bytes");
    CHECK(nv2a_texture_level_bytes(NV2A_TEXFMT_RGBA8, 8, 8, 0) == 256,
          "rgba8 8x8");
    CHECK(nv2a_texture_level_bytes(NV2A_TEXFMT_LINEAR565, 8, 8, 16) == 128,
          "linear uses pitch*h");
    /* A 5-wide DXT1 level still costs two blocks across. */
    CHECK(nv2a_texture_level_bytes(NV2A_TEXFMT_DXT1, 5, 4, 0) == 16,
          "dxt1 rounds up to whole blocks");
    CHECK(nv2a_texture_next_pitch(NV2A_TEXFMT_DXT1, 8, 0) == 8,
          "next dxt1 pitch at w=4");
    CHECK(nv2a_texture_next_pitch(NV2A_TEXFMT_RGBA8, 8, 0) == 16,
          "next rgba8 pitch at w=4");
    CHECK(nv2a_texture_next_pitch(NV2A_TEXFMT_LINEAR565, 8, 16) == 16,
          "linear pitch does not change");
    /* The swizzled 16-bit formats follow their width, like RGBA8 at half the
     * bytes -- NOT the linear rule above, which is what they used to get. */
    CHECK(nv2a_texture_level_bytes(NV2A_TEXFMT_X1R5G5B5, 8, 4, 64) == 64,
          "x1r5g5b5 8x4 is w*h*2, whatever pitch says");
    CHECK(nv2a_texture_next_pitch(NV2A_TEXFMT_A4R4G4B4, 8, 16) == 8,
          "next a4r4g4b4 pitch at w=4");
}

/* ── RGBA8: BGRA in guest order, Morton addressed ───────────────────────── */

static void test_rgba8(void)
{
    uint8_t src[4 * 4 * 4], dst[4 * 4 * 4];
    unsigned x, y;
    memset(src, 0, sizeof src);
    /* Put a known BGRA quad at the Morton slot for (1,1), which is index 3. */
    src[3 * 4 + 0] = 0x10;   /* B */
    src[3 * 4 + 1] = 0x20;   /* G */
    src[3 * 4 + 2] = 0x30;   /* R */
    src[3 * 4 + 3] = 0x40;   /* A */
    CHECK(nv2a_texture_decode_rgba8(src, sizeof src, 4, 4, 0,
                                    NV2A_TEXFMT_RGBA8, dst), "rgba8 decode");
    px(dst, 4, 1, 1, 0x30, 0x20, 0x10, 0x40, "rgba8 BGRA->RGBA");
    /* and nothing else picked it up */
    for (y = 0; y < 4; ++y) for (x = 0; x < 4; ++x)
        if (!(x == 1 && y == 1))
            px(dst, 4, x, y, 0, 0, 0, 0, "rgba8 untouched");
}

/* ── the swizzled 16-bit formats: Morton at two bytes, 555 and 4444 ──── */

static void test_sz16(void)
{
    uint8_t src[4 * 4 * 2], dst[4 * 4 * 4];
    unsigned x, y;

    /* A4R4G4B4 0xF8C3 at the Morton slot for (1,1), index 3 = byte 6:
     * a=F r=8 g=C b=3, each n/15, so 255,136,204,51 -- 0xF is 255, not 240.
     * And 0x0F00 at (2,0), index 4 = byte 8: red with alpha ZERO, the value
     * a forced-opaque decode gets wrong. */
    memset(src, 0, sizeof src);
    src[6] = 0xC3; src[7] = 0xF8;
    src[8] = 0x00; src[9] = 0x0F;
    CHECK(nv2a_texture_decode_rgba8(src, sizeof src, 4, 4, 8,
                                    NV2A_TEXFMT_A4R4G4B4, dst), "4444 decode");
    px(dst, 4, 1, 1, 136,204,51,255, "4444 swizzled, /15");
    px(dst, 4, 2, 0, 255,0,0,0,      "4444 alpha is real");
    for (y = 0; y < 4; ++y) for (x = 0; x < 4; ++x)
        if (!(x == 1 && y == 1) && !(x == 2 && y == 0))
            px(dst, 4, x, y, 0,0,0,0, "4444 untouched");

    /* X1R5G5B5 0xC3E1 at (1,1): top bit SET but not alpha, r=16 g=31 b=1.
     * 16/31 rounds to 132 (truncation would say 131); 1/31 to 8. 0x7C00 at
     * (2,0) is red, top bit clear, and still opaque. */
    memset(src, 0, sizeof src);
    src[6] = 0xE1; src[7] = 0xC3;
    src[8] = 0x00; src[9] = 0x7C;
    CHECK(nv2a_texture_decode_rgba8(src, sizeof src, 4, 4, 8,
                                    NV2A_TEXFMT_X1R5G5B5, dst), "555 decode");
    px(dst, 4, 1, 1, 132,255,8,255, "555 swizzled, top bit ignored");
    px(dst, 4, 2, 0, 255,0,0,255,   "555 opaque with top bit clear");
    px(dst, 4, 0, 1, 0,0,0,255,     "555 zero is opaque black");

    /* Non-square: 8x2 puts (7,1) at Morton 15, byte 30, which a row-major
     * 16-byte pitch would put at byte 30 too -- so use (0,1) instead: Morton
     * 2 (byte 4) against row-major byte 16. */
    {
        uint8_t n[8 * 2 * 2], out[8 * 2 * 4];
        memset(n, 0, sizeof n);
        n[4] = 0x00; n[5] = 0xFF;          /* opaque red */
        CHECK(nv2a_texture_decode_rgba8(n, sizeof n, 8, 2, 16,
                                        NV2A_TEXFMT_A4R4G4B4, out), "4444 8x2");
        px(out, 8, 0, 1, 255,0,0,255, "4444 8x2 reads the Morton slot");
    }

    /* Bounds: a 4x4 level is 32 bytes; 31 must be refused. */
    CHECK(!nv2a_texture_decode_rgba8(src, 31, 4, 4, 8,
                                     NV2A_TEXFMT_X1R5G5B5, dst),
          "short 555 buffer should be refused");
}

/* ── DXT1, including the branch that is easy to get wrong ───────────────── */

static void put_block(uint8_t *b, unsigned c0, unsigned c1, unsigned idx)
{
    b[0] = (uint8_t)(c0 & 0xFF); b[1] = (uint8_t)(c0 >> 8);
    b[2] = (uint8_t)(c1 & 0xFF); b[3] = (uint8_t)(c1 >> 8);
    b[4] = (uint8_t)(idx      ); b[5] = (uint8_t)(idx >>  8);
    b[6] = (uint8_t)(idx >> 16); b[7] = (uint8_t)(idx >> 24);
}

static void test_dxt1(void)
{
    uint8_t src[8], dst[4 * 4 * 4];
    const unsigned WHITE = 0xFFFFu;   /* 565 all ones */
    const unsigned BLACK = 0x0000u;

    /* c0 > c1 so this is the FOUR-colour mode. Texel (0,0) index 0 -> c0,
     * (1,0) index 1 -> c1, (2,0) index 2 -> 2/3 c0, (3,0) index 3 -> 1/3 c0. */
    put_block(src, WHITE, BLACK, (0u<<0)|(1u<<2)|(2u<<4)|(3u<<6));
    CHECK(nv2a_texture_decode_rgba8(src, sizeof src, 4, 4, 8,
                                    NV2A_TEXFMT_DXT1, dst), "dxt1 decode");
    px(dst, 4, 0, 0, 255,255,255,255, "dxt1 index0 = c0");
    px(dst, 4, 1, 0, 0,0,0,255,       "dxt1 index1 = c1");
    px(dst, 4, 2, 0, 170,170,170,255, "dxt1 index2 = 2/3 c0");
    px(dst, 4, 3, 0, 85,85,85,255,    "dxt1 index3 = 1/3 c0");

    /* c0 <= c1: the THREE-colour mode, and index 3 is transparent black --
     * alpha 0, not 255. This is the rule most implementations get wrong. */
    put_block(src, BLACK, WHITE, (3u<<0)|(2u<<2));
    CHECK(nv2a_texture_decode_rgba8(src, sizeof src, 4, 4, 8,
                                    NV2A_TEXFMT_DXT1, dst), "dxt1 punchthrough");
    px(dst, 4, 0, 0, 0,0,0,0,         "dxt1 c0<=c1 index3 is TRANSPARENT");
    /* 0.5*0 + 0.5*255 = 127.5, and a float -> unorm8 conversion ROUNDS, so
      * this is 128. Truncating to 127 is the easy mistake. */
    px(dst, 4, 1, 0, 128,128,128,255, "dxt1 c0<=c1 index2 is the 1/2 mix");
}

/* ── DXT3: 4-bit alpha, and never the punch-through branch ──────────────── */

static void test_dxt3(void)
{
    uint8_t src[16], dst[4 * 4 * 4];
    memset(src, 0, sizeof src);
    src[0] = 0x0F;              /* texel 0 alpha = 15, texel 1 alpha = 0 */
    /* c0 <= c1 deliberately: DXT3 must STILL interpolate, not punch through. */
    put_block(src + 8, 0x0000u, 0xFFFFu, (3u<<0)|(0u<<2));
    CHECK(nv2a_texture_decode_rgba8(src, sizeof src, 4, 4, 16,
                                    NV2A_TEXFMT_DXT3, dst), "dxt3 decode");
    /* Weight applies to c0: pick 3 gives w=1/3, so this is 2/3 of c1=WHITE
      * = 170. In the DXT1 case above c0 and c1 are the other way round, which
      * is why the same index reads 85 there and 170 here. */
    px(dst, 4, 0, 0, 170,170,170,255, "dxt3 index3 interpolates even at c0<=c1");
    px(dst, 4, 1, 0, 0,0,0,0,      "dxt3 index0 = c0, alpha nibble 0");
}

/* ── the 16-bit fallback, and bounds ────────────────────────────────────── */

static void test_fallback_and_bounds(void)
{
    uint8_t src[2 * 2 * 2], dst[2 * 2 * 4];
    /* 565: 0xF800 is pure red, 0x07E0 pure green. */
    src[0] = 0x00; src[1] = 0xF8;
    src[2] = 0xE0; src[3] = 0x07;
    src[4] = src[5] = src[6] = src[7] = 0;
    CHECK(nv2a_texture_decode_rgba8(src, sizeof src, 2, 2, 4,
                                    NV2A_TEXFMT_LINEAR565, dst), "565 decode");
    px(dst, 2, 0, 0, 255,0,0,255, "565 red");
    px(dst, 2, 1, 0, 0,255,0,255, "565 green");

    /* A decode that would read past what it was given must refuse, not read
     * guest memory nobody handed it. */
    CHECK(!nv2a_texture_decode_rgba8(src, 4, 4, 4, 8, NV2A_TEXFMT_DXT1, dst),
          "short DXT1 buffer should be refused");
    CHECK(!nv2a_texture_decode_rgba8(src, sizeof src, 0, 4, 8,
                                     NV2A_TEXFMT_DXT1, dst),
          "zero width should be refused");
    CHECK(!nv2a_texture_decode_rgba8(NULL, 16, 4, 4, 8,
                                     NV2A_TEXFMT_DXT1, dst),
          "null source should be refused");

    /* The nominal 8x8 DXT1 size is 32 bytes, but this pitch points the
     * second block row beyond that buffer. Failure must leave dst intact. */
    {
        uint8_t compressed[32], output[8 * 8 * 4];
        memset(compressed, 0, sizeof compressed);
        memset(output, 0xA5, sizeof output);
        CHECK(!nv2a_texture_decode_rgba8(compressed, sizeof compressed,
                                         8, 8, 64, NV2A_TEXFMT_DXT1, output),
              "late DXT1 out-of-bounds read should be refused");
        for (size_t i = 0; i < sizeof output; ++i)
            CHECK(output[i] == 0xA5, "failed decode modified output byte %zu", i);
    }
}

int main(void)
{
    test_morton();
    test_levels();
    test_rgba8();
    test_sz16();
    test_dxt1();
    test_dxt3();
    test_fallback_and_bounds();
    fprintf(stderr, "%s\n", fail ? "FAILED" : "ok");
    return fail ? 1 : 0;
}
