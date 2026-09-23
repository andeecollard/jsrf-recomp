/* The host DSOUND model (src/apu/dsound_host.c), checked without the title.
 * G48 phase 2; docs/jsrf/plans/JSRF_PLAN_2026-09-23_LIFT_AT_THE_DSOUND_BOUNDARY.md. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu_state.h"
#include "dsound_host.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

#define RAM (1u << 20)
static uint8_t ram[RAM];
static const uint8_t *mem(uint32_t va, uint32_t len)
{
    return (va < RAM && len <= RAM - va) ? ram + va : NULL;
}
static void put16(uint32_t va, int16_t s) { ram[va] = (uint8_t)s; ram[va + 1] = (uint8_t)((uint16_t)s >> 8); }

static const dsh_format MONO16 = { DSH_TAG_PCM, 1, 48000, 16, 2 };

/* 1. The decoder agrees with the APU's tested one on hardware-legal blocks,
 *    and unlike it accepts a nonzero reserved byte (the hardware does). */
static void test_adpcm_matches_apu_decoder(void)
{
    srand(7);
    for (int ch = 1; ch <= 2; ++ch)
        for (int trial = 0; trial < 200; ++trial) {
            uint8_t blk[72];
            for (int i = 0; i < 36 * ch; ++i) blk[i] = (uint8_t)rand();
            for (int c = 0; c < ch; ++c) { blk[c * 4 + 2] = (uint8_t)(rand() % 89); blk[c * 4 + 3] = 0; }
            int16_t a[65 * 2], b[65 * 2];
            memset(a, 0, sizeof a); memset(b, 0, sizeof b);
            int na = adpcm_decode_block(a, blk, 36u * (unsigned)ch, ch);
            int nb = dsh_adpcm_decode_block(b, blk, 36u * (unsigned)ch, ch);
            CHECK(na == 65 && nb == 65, "frames apu=%d host=%d", na, nb);
            CHECK(!memcmp(a, b, sizeof(int16_t) * 65u * (unsigned)ch),
                  "ch=%d trial=%d decodes differ", ch, trial);
        }
    uint8_t pad[36]; memset(pad, 0x08, sizeof pad);
    int16_t o[65];
    CHECK(dsh_adpcm_decode_block(o, pad, 36, 1) == 65, "the 0x08 pad block decodes");
    CHECK(dsh_adpcm_decode_block(o, pad, 35, 1) == 0, "a short block is refused");
}

/* 2. PCM at the output rate, unity gain: the output is the input. */
static void test_pcm_passthrough_and_end(void)
{
    dsh_reset();
    for (int i = 0; i < 100; ++i) put16(0x1000 + 2u * i, (int16_t)(i * 300 - 15000));
    CHECK(dsh_buffer_create(0x80001000u, &MONO16, 0x1000, 200) == 0, "create");
    dsh_set_headroom(0x80001000u, 0);
    CHECK(dsh_get_status(0x80001000u) == 0, "not playing before Play");
    dsh_play(0x80001000u, 0);
    CHECK(dsh_get_status(0x80001000u) == DSH_STATUS_PLAYING, "playing");
    int16_t out[2 * 64];
    dsh_mix(out, 64);
    int ok = 1;
    for (int i = 0; i < 64; ++i)
        ok &= out[2 * i] == (int16_t)(i * 300 - 15000) && out[2 * i + 1] == out[2 * i];
    CHECK(ok, "mono PCM at 48 kHz, 0 dB, 0 headroom comes out unchanged on both channels");
    uint32_t p, w;
    dsh_get_current_position(0x80001000u, &p, &w);
    CHECK(p == 128, "play cursor 64 frames = 128 bytes, got %u", p);
    dsh_mix(out, 64);
    CHECK(dsh_get_status(0x80001000u) == 0, "a one-shot stops at its end");
    dsh_get_current_position(0x80001000u, &p, NULL);
    CHECK(p == 0, "and its cursor returns to 0, got %u", p);
    CHECK(out[2 * 36] == 0 && out[2 * 63] == 0, "silence after the end");
}

/* 3. Looping inside a region, and the default headroom. */
static void test_loop_region_and_headroom(void)
{
    dsh_reset();
    for (int i = 0; i < 100; ++i) put16(0x2000 + 2u * i, (int16_t)(1000 + i));
    dsh_buffer_create(0x80002000u, &MONO16, 0x2000, 200);
    dsh_set_loop_region(0x80002000u, 20, 40);           /* frames 10..29 */
    dsh_set_headroom(0x80002000u, 0);
    dsh_play(0x80002000u, DSH_PLAY_LOOPING);
    CHECK(dsh_get_status(0x80002000u) == (DSH_STATUS_PLAYING | DSH_STATUS_LOOPING), "looping status");
    int16_t out[2 * 60];
    dsh_mix(out, 60);
    CHECK(out[2 * 29] == 1029 && out[2 * 30] == 1010 && out[2 * 50] == 1010,
          "wraps from frame 29 to 10: %d %d %d", out[2 * 29], out[2 * 30], out[2 * 50]);
    uint32_t p; dsh_get_current_position(0x80002000u, &p, NULL);
    CHECK(p == 2u * 20u, "cursor inside the region: %u", p);

    dsh_buffer_create(0x80002100u, &MONO16, 0x2000, 200);   /* default headroom 600 */
    dsh_stop(0x80002000u);
    dsh_play(0x80002100u, 0);
    dsh_mix(out, 1);
    CHECK(abs(out[0] - (int)lrintf(1000.0f * powf(10.0f, -0.3f))) <= 1,
          "default 6 dB headroom: %d", out[0]);
}

/* 4. Frequency and volume. */
static void test_frequency_and_volume(void)
{
    dsh_reset();
    for (int i = 0; i < 100; ++i) put16(0x3000 + 2u * i, (int16_t)(i * 100));
    dsh_buffer_create(0x80003000u, &MONO16, 0x3000, 200);
    dsh_set_headroom(0x80003000u, 0);
    dsh_set_frequency(0x80003000u, 24000);
    dsh_play(0x80003000u, 0);
    int16_t out[2 * 10];
    dsh_mix(out, 10);
    CHECK(out[2] == 50 && out[4] == 100 && out[18] == 450, "half rate interpolates: %d %d %d",
          out[2], out[4], out[18]);
    dsh_set_frequency(0x80003000u, 0);
    dsh_set_volume(0x80003000u, -600);
    dsh_set_current_position(0x80003000u, 2 * 50);
    dsh_mix(out, 1);
    CHECK(abs(out[0] - (int)lrintf(5000.0f * powf(10.0f, -0.3f))) <= 1, "-6 dB volume: %d", out[0]);
}

/* 5. ADPCM playback through guest memory, cursors block-aligned. */
static void test_adpcm_voice(void)
{
    dsh_reset();
    const dsh_format ad = { DSH_TAG_XBOX_ADPCM, 1, 48000, 4, 36 };
    srand(3);
    for (int b = 0; b < 4; ++b) {
        for (int i = 0; i < 36; ++i) ram[0x4000 + 36 * b + i] = (uint8_t)rand();
        ram[0x4000 + 36 * b + 2] = 20; ram[0x4000 + 36 * b + 3] = 0;
    }
    int16_t ref[65 * 4];
    for (int b = 0; b < 4; ++b) dsh_adpcm_decode_block(ref + 65 * b, ram + 0x4000 + 36 * b, 36, 1);
    dsh_buffer_create(0x80004000u, &ad, 0, 0);
    dsh_set_buffer_data(0x80004000u, 0x4000, 144);
    dsh_set_headroom(0x80004000u, 0);
    dsh_play(0x80004000u, 0);
    int16_t out[2 * 200];
    dsh_mix(out, 200);
    int ok = 1;
    for (int i = 0; i < 200; ++i) ok &= out[2 * i] == ref[i];
    CHECK(ok, "ADPCM voice plays its decoded samples across block seams");
    uint32_t p; dsh_get_current_position(0x80004000u, &p, NULL);
    CHECK(p == 36u * 3u, "ADPCM cursor is block-aligned: %u", p);
}

/* 6. Release, reuse and refusals. */
static void test_table(void)
{
    dsh_reset();
    const dsh_format bad = { 2, 1, 22050, 4, 36 };      /* MS ADPCM: not ours */
    CHECK(dsh_buffer_create(0x80005000u, &bad, 0, 0) == -1, "a foreign format is refused");
    for (uint32_t h = 0; h < 1500; ++h) CHECK(dsh_buffer_create(0x80100000u + 0x48u * h, &MONO16, 0, 0) == 0, "create %u", h);
    for (uint32_t h = 0; h < 1500; ++h) dsh_buffer_release(0x80100000u + 0x48u * h);
    for (uint32_t h = 0; h < 1500; ++h) CHECK(dsh_buffer_create(0x80900000u + 0x48u * h, &MONO16, 0, 0) == 0, "reuse %u", h);
    dsh_stats s; dsh_get_stats(&s);
    CHECK(s.buffers == 1500 && s.released == 1500 && s.refused_format == 1,
          "stats buffers=%lu released=%lu refused=%lu", s.buffers, s.released, s.refused_format);
    CHECK(!dsh_buffer_exists(0x80100000u) && dsh_buffer_exists(0x80900000u), "released handles are gone");
    dsh_play(0x80900000u, 0);
    int16_t out[4];
    dsh_mix(out, 2);
    dsh_get_stats(&s);
    CHECK(s.missing_data == 1 && out[0] == 0, "a playing voice with no data is counted, not mixed");
}

/* 7. Mix bins: a mono effect panned hard left, then centre at -3 dB. */
static void test_mixbins(void)
{
    dsh_reset();
    for (int i = 0; i < 10; ++i) put16(0x6000 + 2u * i, 10000);
    dsh_buffer_create(0x80006000u, &MONO16, 0x6000, 20);
    dsh_set_headroom(0x80006000u, 0);
    uint32_t bins[2] = { 0, 3 }; int32_t vols[2] = { 0, 0 };        /* front left, LFE */
    dsh_set_mixbins(0x80006000u, 2, bins, vols);
    dsh_play(0x80006000u, DSH_PLAY_LOOPING);
    int16_t out[4];
    dsh_mix(out, 1);
    CHECK(out[0] == 10000 && out[1] == 0, "front-left only: %d %d", out[0], out[1]);
    uint32_t c[1] = { 2 }; int32_t cv[1] = { 0 };
    dsh_set_mixbins(0x80006000u, 1, c, cv);
    dsh_mix(out, 1);
    CHECK(abs(out[0] - 7071) <= 1 && abs(out[1] - 7071) <= 1, "centre at -3 dB: %d %d", out[0], out[1]);
    dsh_set_mixbins(0x80006000u, 0, NULL, NULL);
    dsh_mix(out, 1);
    CHECK(out[0] == 10000 && out[1] == 10000, "n=0 restores the default: %d %d", out[0], out[1]);
}

/* 8. 3D: -6 dB per doubling past min distance, clamped at max, soft pan. */
static void test_3d(void)
{
    dsh_reset();
    for (int i = 0; i < 10; ++i) put16(0x7000 + 2u * i, 10000);
    const uint32_t h = 0x80007000u;
    dsh_buffer_create(h, &MONO16, 0x7000, 20);
    dsh_set_headroom(h, 0);
    dsh_set_3d(h, 1);
    dsh_set_3d_distances(h, 10.0f, 100.0f);
    dsh_set_listener_position(0, 0, 0);
    dsh_set_listener_orientation(0, 0, 1, 0, 1, 0);
    dsh_set_listener_factors(1.0f, 1.0f);
    dsh_play(h, DSH_PLAY_LOOPING);
    int16_t out[2];

    dsh_set_3d_position(h, 0, 0, 5);  dsh_mix(out, 1);
    CHECK(out[0] == 10000 && out[1] == 10000, "inside min distance: full, centred: %d %d", out[0], out[1]);
    dsh_set_3d_position(h, 0, 0, 20); dsh_mix(out, 1);
    CHECK(abs(out[0] - 5000) <= 1 && abs(out[1] - 5000) <= 1, "twice min: -6 dB: %d %d", out[0], out[1]);
    dsh_set_3d_position(h, 0, 0, 400); dsh_mix(out, 1);
    CHECK(abs(out[0] - 1000) <= 1, "past max: clamped at min/max: %d", out[0]);
    dsh_set_3d_position(h, 10, 0, 0); dsh_mix(out, 1);
    CHECK(out[1] == 10000 && abs(out[0] - 3000) <= 1, "hard right (+x): R full, L 30%%: %d %d", out[0], out[1]);
    dsh_set_listener_orientation(0, 0, -1, 0, 1, 0);                 /* turned round */
    dsh_mix(out, 1);
    CHECK(out[0] == 10000 && abs(out[1] - 3000) <= 1, "listener turned: now on the left: %d %d", out[0], out[1]);
    dsh_set_3d_mode(h, 2);
    dsh_set_3d_position(h, 0, 0, 400); dsh_mix(out, 1);
    CHECK(out[0] == 10000 && out[1] == 10000, "mode disabled: no 3D: %d %d", out[0], out[1]);
    dsh_set_3d(h, 0); dsh_set_3d_mode(h, 0); dsh_mix(out, 1);
    CHECK(out[0] == 10000, "a 2D buffer ignores its position: %d", out[0]);
}

int main(void)
{
    g_adpcm_hw_header = 0;
    dsh_init(mem, 48000);
    test_adpcm_matches_apu_decoder();
    test_pcm_passthrough_and_end();
    test_loop_region_and_headroom();
    test_frequency_and_volume();
    test_adpcm_voice();
    test_table();
    test_mixbins();
    test_3d();
    if (failures) { printf("dsound_host_test: %d failure(s)\n", failures); return 1; }
    puts("dsound_host_test: all checks passed");
    return 0;
}
