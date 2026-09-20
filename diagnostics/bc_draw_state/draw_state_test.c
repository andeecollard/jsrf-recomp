/* BC-inspired invariant: each submitted draw owns its texture data and state.
 * An independent RGB565 pixel oracle, not merely batch-on == batch-off.
 * DXT3 pages match the font's 256x256 dimensions; no copyrighted assets.
 * No sync between draws: replacement and eviction happen while work is queued.
 */
#include "nv2a_metal.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define W 256u
#define H 64u
#define PAGE_BYTES (256u * 256u)
#define DRAWS 256u
static uint8_t pages[DRAWS][PAGE_BYTES], target[W * H * 2];
static uint16_t expected[DRAWS];
static const uint16_t colors[] = {0xf800, 0x07e0, 0x001f, 0xffff};

/* Diagnostic timestamp only; this executable has no guest clock. */
double xbox_TraceSeconds(void) { return 0.0; }

static void fill_page(uint8_t *p, uint16_t color)
{
    for (unsigned i = 0; i < PAGE_BYTES; i += 16) {
        memset(p + i, 0xff, 8); /* DXT3 explicit opaque alpha */
        p[i+8] = p[i+10] = color & 255;
        p[i+9] = p[i+11] = color >> 8;
        memset(p + i + 12, 0, 4); /* endpoint zero at every texel */
    }
}

static int draw_tile(NV2ATextureCopy *s, const uint8_t *page, unsigned tile)
{
    static const unsigned xy[6][2] = {{0,0},{8,0},{0,8},{0,8},{8,0},{8,8}};
    float v[6][16][4] = {0};
    for (unsigned i = 0; i < 6; ++i) {
        v[i][0][0] = (tile % 32) * 8 + xy[i][0];
        v[i][0][1] = (tile / 32) * 8 + xy[i][1];
        v[i][0][3] = v[i][9][3] = 1;
        v[i][9][0] = v[i][9][1] = 0.5f;
    }
    int result = nv2a_metal_draw(s, page, PAGE_BYTES, target, sizeof target,
                               NULL, 0, v, 6, 5);
    if (result != 2) {
        fprintf(stderr, "draw %u rejected: %s (triangles=%d)\n",
                tile, nv2a_metal_last_reject(), result);
        return 0;
    }
    return 1;
}

int main(void)
{
    const char *names[] = {"alternating pages", "same-address replacement",
                           "cache eviction", "overlapping last draw wins"};
    for (unsigned phase = 0; phase < 4; ++phase) {
        NV2ATextureCopy s = {0};
        s.width = s.height = 256; s.pitch = 1024;
        s.clip_w = W; s.clip_h = H; s.target_pitch = W * 2;
        s.target_bpp = 2; s.levels = 1; s.texture_mask = 1;
        s.dxt3 = 1; s.min_filter = 1;
        nv2a_metal_invalidate(target);
        memset(target, 0, sizeof target);
        for (unsigned i = 0; i < DRAWS; ++i)
            fill_page(pages[i], colors[i % 4]);
        for (unsigned i = 0; i < DRAWS; ++i) {
            unsigned page = phase == 0 ? i % 2 : phase == 1 ? 0 : i;
            expected[i] = colors[phase == 0 ? i % 2 : i % 4];
            if (phase == 1) fill_page(pages[0], expected[i]);
            s.texture_offset = page * PAGE_BYTES;
            if (!draw_tile(&s, pages[page], i)) return 1;
        }
        if (phase == 3) {
            for (unsigned i = 0; i < DRAWS; ++i) {
                unsigned page = (i + 1) % DRAWS;
                expected[i] = colors[page % 4];
                s.texture_offset = page * PAGE_BYTES;
                if (!draw_tile(&s, pages[page], i)) return 1;
            }
        }
        /* Poison caller-owned state/data after encoding, before the only sync. */
        memset(&s, 0, sizeof s);
        memset(pages, 0, sizeof pages);
        if (!nv2a_metal_sync()) { fprintf(stderr, "sync failed\n"); return 1; }
        unsigned bad = 0;
        for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) {
            unsigned off = (y * W + x) * 2;
            uint16_t got = target[off] | (target[off+1] << 8);
            uint16_t want = expected[(y / 8) * 32 + x / 8];
            if (got != want && bad++ < 3)
                fprintf(stderr, "%s pixel %u,%u: %04x != %04x\n",
                        names[phase], x, y, got, want);
        }
        if (bad) { fprintf(stderr, "%u incorrect pixels\n", bad); return 1; }
        printf("PASS %s: %u exact pixels\n", names[phase], W * H);
    }
    nv2a_metal_report();
    return 0;
}
