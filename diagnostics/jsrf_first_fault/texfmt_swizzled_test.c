/*
 * The two swizzled texture formats the gate used to refuse outright.
 *
 * A [TEXFMT] census on 21 Sep 2026 found that SZ_X1R5G5B5 (0x03) and
 * SZ_X8R8G8B8 (0x07) were rejected on 100% of the draws that used them --
 * 674 and 543 in one tutorial run, and 69,950 rejections of 3,118,918 draws
 * in the player's real session. The reason string said "texture format / mip
 * layout", which names the gate and not the input, so the logs had been
 * carrying the answer for weeks without anyone able to read it.
 *
 * TWO THINGS ARE EASY TO GET WRONG HERE, and each has its own case:
 *
 *   X8R8G8B8 shares the A8R8G8B8 code path, and its top byte is UNDEFINED.
 *   Sampling it as alpha gives a texture that is transparent wherever the
 *   padding happens to be zero -- which is most of the time, and looks like
 *   the object simply not drawing. The fix is not "accept format 7"; it is
 *   "accept format 7 AND force alpha opaque".
 *
 *   Both formats are SWIZZLED (Morton order), not linear. Decoding either
 *   row-major produces a recognisable but scrambled image, which is the kind
 *   of wrong that survives a glance at a screenshot.
 */
#include <stdio.h>
#include <string.h>

#include "../../src/nv2a/nv2a_texture_copy.h"

#define W 4u
#define H 4u

static int failures;

static void check(const char *what, long got, long want)
{
    if (got == want) { printf("  ok   %-46s %ld\n", what, got); return; }
    printf("  FAIL %-46s got %ld want %ld\n", what, got, want);
    ++failures;
}

/* Rectangular Morton order, written out independently of the implementation
 * so the test does not agree with a bug by construction. */
static unsigned morton(unsigned x, unsigned y)
{
    return (x & 1u) | ((y & 1u) << 1) | ((x & 2u) << 1) | ((y & 2u) << 2);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "x8r8g8b8";
    NV2ATextureCopy s;
    unsigned char src[W * H * 4], out[W * H * 4];
    unsigned ow = 0, oh = 0, x, y;

    memset(&s, 0, sizeof s);
    s.width = W; s.height = H; s.levels = 1;

    if (!strcmp(mode, "x8r8g8b8")) {
        s.rgba8 = 1; s.xrgb8 = 1; s.pitch = W * 4;
        /* Stored B,G,R,X. The X byte is deliberately 0 everywhere: that is
         * exactly the padding that would come back as alpha=0 if the format
         * were treated as A8R8G8B8. */
        for (y = 0; y < H; ++y) for (x = 0; x < W; ++x) {
            unsigned char *p = src + 4u * morton(x, y);
            p[0] = (unsigned char)(0x10 * x);   /* B */
            p[1] = (unsigned char)(0x20 * y);   /* G */
            p[2] = 0xFF;                        /* R */
            p[3] = 0x00;                        /* X -- undefined, NOT alpha */
        }
        check("decoded", nv2a_texture_copy_decode_level(&s, src, sizeof src, 0,
                                                        out, sizeof out, &ow, &oh), 1);
        check("width", ow, W); check("height", oh, H);
        for (y = 0; y < H; ++y) for (x = 0; x < W; ++x) {
            const unsigned char *q = out + ((size_t)y * W + x) * 4;
            if (q[0] != 0xFF || q[1] != (unsigned char)(0x20 * y)
                    || q[2] != (unsigned char)(0x10 * x)) {
                printf("  FAIL texel (%u,%u) rgb = %02X %02X %02X\n", x, y, q[0], q[1], q[2]);
                ++failures;
            }
        }
        if (!failures) printf("  ok   %-46s\n", "swizzled texels land in the right place");
        /* THE ONE THAT MATTERS. */
        check("alpha forced opaque despite X byte = 0", out[3], 255);

    } else if (!strcmp(mode, "x1r5g5b5")) {
        s.sz16 = 1; s.pitch = W * 2;
        for (y = 0; y < H; ++y) for (x = 0; x < W; ++x) {
            /* Top bit set to 1 to prove it is ignored, not shifted into red. */
            unsigned v = 0x8000u | (31u << 10) | ((y * 8u) << 5) | (x * 8u);
            unsigned char *p = src + 2u * morton(x, y);
            p[0] = (unsigned char)(v & 0xFF);
            p[1] = (unsigned char)(v >> 8);
        }
        check("decoded", nv2a_texture_copy_decode_level(&s, src, sizeof src, 0,
                                                        out, sizeof out, &ow, &oh), 1);
        check("width", ow, W); check("height", oh, H);
        check("red is full, not shifted by the X bit", out[0], 255);
        check("alpha opaque", out[3], 255);
        for (y = 0; y < H; ++y) for (x = 0; x < W; ++x) {
            const unsigned char *q = out + ((size_t)y * W + x) * 4;
            unsigned want_g = (unsigned)((y * 8u) / 31.0f * 255 + 0.5f);
            unsigned want_b = (unsigned)((x * 8u) / 31.0f * 255 + 0.5f);
            if (q[1] != want_g || q[2] != want_b) {
                printf("  FAIL texel (%u,%u) g=%02X want %02X, b=%02X want %02X\n",
                       x, y, q[1], want_g, q[2], want_b);
                ++failures;
            }
        }
        if (!failures) printf("  ok   %-46s\n", "swizzled 555 texels decode correctly");

    } else if (!strcmp(mode, "a4r4g4b4")) {
        /* The last format in the shipped set, and the one place a plausible
         * implementation is quietly wrong: a 4-bit channel spans the FULL
         * range, so 0xF is 255, not 0xF0. Expanding by a left shift costs
         * 1/17th of every channel and can never produce white -- a uniform
         * dimming that survives any number of screenshot reviews.
         *
         * Unlike 0x03 and 0x07 this format carries real alpha, so the
         * opaque-forcing those two need must NOT happen here. */
        s.sz16 = 1; s.argb4 = 1; s.pitch = W * 2;
        for (y = 0; y < H; ++y) for (x = 0; x < W; ++x) {
            unsigned v = (0xFu << 12) | (0xFu << 8) | ((y * 5u) << 4) | (x * 5u);
            unsigned char *p = src + 2u * morton(x, y);
            p[0] = (unsigned char)(v & 0xFF);
            p[1] = (unsigned char)(v >> 8);
        }
        check("decoded", nv2a_texture_copy_decode_level(&s, src, sizeof src, 0,
                                                        out, sizeof out, &ow, &oh), 1);
        check("width", ow, W); check("height", oh, H);
        /* THE TWO THAT MATTER. A shift-by-four expansion gives 240 for both. */
        check("R nibble 0xF expands to 255, not 240", out[0], 255);
        check("A nibble 0xF expands to 255, not 240", out[3], 255);
        for (y = 0; y < H; ++y) for (x = 0; x < W; ++x) {
            const unsigned char *q = out + ((size_t)y * W + x) * 4;
            unsigned want_g = (unsigned)((y * 5u) / 15.0f * 255 + 0.5f);
            unsigned want_b = (unsigned)((x * 5u) / 15.0f * 255 + 0.5f);
            if (q[1] != want_g || q[2] != want_b) {
                printf("  FAIL texel (%u,%u) g=%02X want %02X, b=%02X want %02X\n",
                       x, y, q[1], want_g, q[2], want_b);
                ++failures;
            }
        }
        if (!failures) printf("  ok   %-46s\n", "swizzled 4444 texels decode correctly");

    } else if (!strcmp(mode, "a4r4g4b4-alpha")) {
        /* The other direction: 0x04 alpha must be READ, not forced opaque.
         * Sharing sz16 with X1R5G5B5 -- which does force it -- is exactly the
         * shape of mistake that would make every 4444 texture solid. */
        s.sz16 = 1; s.argb4 = 1; s.pitch = W * 2;
        memset(src, 0, sizeof src);
        for (y = 0; y < H; ++y) for (x = 0; x < W; ++x) {
            unsigned v = (4u << 12);               /* alpha 4/15, rgb zero */
            unsigned char *p = src + 2u * morton(x, y);
            p[0] = (unsigned char)(v & 0xFF);
            p[1] = (unsigned char)(v >> 8);
        }
        check("decoded", nv2a_texture_copy_decode_level(&s, src, sizeof src, 0,
                                                        out, sizeof out, &ow, &oh), 1);
        check("A4R4G4B4 alpha is read, not forced opaque", out[3], 68); /* 4/15*255 */

    } else if (!strcmp(mode, "alpha-regression")) {
        /* A8R8G8B8 must KEEP its alpha -- the fix must not make every
         * 32-bit texture opaque. This is the other half of the trap. */
        s.rgba8 = 1; s.xrgb8 = 0; s.pitch = W * 4;
        memset(src, 0, sizeof src);
        for (y = 0; y < H; ++y) for (x = 0; x < W; ++x)
            src[4u * morton(x, y) + 3] = 0x40;
        check("decoded", nv2a_texture_copy_decode_level(&s, src, sizeof src, 0,
                                                        out, sizeof out, &ow, &oh), 1);
        check("A8R8G8B8 alpha still read from the byte", out[3], 0x40);

    } else if (!strcmp(mode, "lin-a8r8g8b8") || !strcmp(mode, "lin-x8r8g8b8")) {
        /* LU_IMAGE_A8R8G8B8 (0x12) / X8R8G8B8 (0x1E): the graffiti canvas.
         * ROW-MAJOR at y*pitch + x*4, not Morton, and the pitch is padded
         * past width*4 here so a decoder that assumes w*4 reads the padding
         * (0xEE) on every row after the first. A8 reads its alpha byte; X8
         * forces it opaque exactly as the swizzled 0x07 does. */
        enum { LP = W * 4 + 8 };
        unsigned char lsrc[LP * H];
        int x8 = !strcmp(mode, "lin-x8r8g8b8");
        s.lin32 = x8 ? 2 : 1; s.pitch = LP;
        memset(lsrc, 0xEE, sizeof lsrc);
        for (y = 0; y < H; ++y) for (x = 0; x < W; ++x) {
            unsigned char *p = lsrc + y * LP + x * 4;
            p[0] = (unsigned char)(0x10 * x); p[1] = (unsigned char)(0x20 * y);
            p[2] = 0x80; p[3] = (unsigned char)(0x11 * (x + 4 * y));
        }
        check("decoded", nv2a_texture_copy_decode_level(&s, lsrc, sizeof lsrc, 0,
                                                        out, sizeof out, &ow, &oh), 1);
        check("width", ow, W); check("height", oh, H);
        for (y = 0; y < H; ++y) for (x = 0; x < W; ++x) {
            const unsigned char *q = out + ((size_t)y * W + x) * 4;
            unsigned want_a = x8 ? 255u : 0x11u * (x + 4 * y);
            if (q[0] != 0x80 || q[1] != (unsigned char)(0x20 * y)
                    || q[2] != (unsigned char)(0x10 * x) || q[3] != want_a) {
                printf("  FAIL texel (%u,%u) rgba = %02X %02X %02X %02X\n",
                       x, y, q[0], q[1], q[2], q[3]);
                ++failures;
            }
        }
        if (!failures) printf("  ok   %-46s\n", x8 ? "linear X8 texels by pitch, alpha opaque"
                                                  : "linear A8 texels by pitch, alpha read");
        check("texture bytes are pitch * height",
              (long)nv2a_texture_copy_texture_bytes(&s), (long)(LP * H));

    } else {
        fprintf(stderr, "unknown case '%s'\n", mode);
        return 2;
    }

    printf("%s: %s\n", mode, failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}
