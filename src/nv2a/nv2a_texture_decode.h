#ifndef NV2A_TEXTURE_DECODE_H
#define NV2A_TEXTURE_DECODE_H

/* GUEST TEXTURE -> LINEAR RGBA8, ON THE CPU, ONCE PER UPLOAD.
 *
 * G27. The Metal backend has no hardware texture sampling: guest textures are
 * bound as `const device uchar*` and sampled by hand in the fragment shader.
 * A single DXT1 texel there costs two integer modulos, eight byte-wise scalar
 * loads, 565 unpacking and a 2-bit index extract -- times four for bilinear,
 * times eight for trilinear, per fragment, per texture unit. An RGBA8 texel
 * additionally runs `morton()`, which is a loop of up to eleven iterations.
 * Measured consequence: `sync` (which is waitUntilCompleted, so it IS GPU
 * execution time) costs 6.94-9 ms per flip for a 640x480 scene on an M1 Max.
 *
 * The fix is to hand Metal an MTLTexture and let the sampler hardware do it.
 * This file is the first half of that: it turns any guest format into linear
 * RGBA8 that MTLTexture can take. The cost moves to the right side of a very
 * favourable ratio -- the player's session logged 50,341 texture uploads
 * against 13,100,361 sample requests.
 *
 * FAITHFULNESS IS THE WHOLE REQUIREMENT. This must produce exactly what the
 * shader's texel() produces, or the change trades frame time for a different
 * picture, which is not a win. Every rule below is transcribed from texel()
 * and sample_level() in nv2a_metal.m, including the ones that look wrong:
 *
 *   - RGBA8 is stored BGRA and Morton-swizzled: the shader returns
 *     (t[at+2], t[at+1], t[at], t[at+3]).
 *   - DXT1 with c0 <= c1 and index 3 returns FULLY TRANSPARENT BLACK, not
 *     black -- float4(0), alpha included.
 *   - DXT3 colour is always the four-colour interpolation, never the
 *     punch-through variant, whatever c0 and c1 are. Alpha is the 4-bit
 *     nibble scaled by 1/15.
 *   - SZ_X1R5G5B5 (0x03) and SZ_A4R4G4B4 (0x04) are Morton-swizzled at two
 *     bytes a texel, like RGBA8 at four. 555 has alpha forced to 1 (its top
 *     bit is undefined); 4444 has REAL alpha, and every channel is n/15, so
 *     0xF is 255 -- not 0xF0. The shader decoded both as the linear fallback
 *     below until 24 Sep 2026.
 *   - The 16-bit fallback is R5G6B5 with alpha forced to 1.
 *
 * texture_decode_test.c is the check, and it is written against these rules
 * rather than against this implementation. */

#include <stddef.h>
#include <stdint.h>

/* The guest format byte, as the gate in nv2a_pb_exec.c already spells it. */
#define NV2A_TEXFMT_X1R5G5B5   0x03
#define NV2A_TEXFMT_A4R4G4B4   0x04
#define NV2A_TEXFMT_RGBA8      0x06
#define NV2A_TEXFMT_RGBA8_ALT  0x07
#define NV2A_TEXFMT_DXT1       0x0C
#define NV2A_TEXFMT_DXT3       0x0E
#define NV2A_TEXFMT_LINEAR565  0x11

/* Bytes one mip level of `fmt` occupies at w x h, with `pitch` for the
 * linear formats. Matches the `base +=` arithmetic in sample_level(). */
size_t nv2a_texture_level_bytes(int fmt, unsigned w, unsigned h,
                                unsigned pitch);

/* The pitch the NEXT level down uses, matching sample_level()'s own update. */
unsigned nv2a_texture_next_pitch(int fmt, unsigned w, unsigned pitch);

/* Decode one level into `dst`, which must hold w*h*4 bytes, as R,G,B,A.
 *
 * `src` is the guest bytes for THIS level (the caller has already walked the
 * mip chain) and `src_size` bounds them: a decode that would read past it
 * fails rather than reading guest memory it was not given. Returns 1 on
 * success, 0 if the format is unsupported or the bounds do not hold, and
 * writes nothing to dst on failure. */
int nv2a_texture_decode_rgba8(const uint8_t *src, size_t src_size,
                              unsigned w, unsigned h, unsigned pitch,
                              int fmt, uint8_t *dst);

/* Exposed for the test: the swizzle the RGBA8 path walks per texel. */
unsigned nv2a_texture_morton(unsigned x, unsigned y, unsigned w, unsigned h);

#endif /* NV2A_TEXTURE_DECODE_H */
