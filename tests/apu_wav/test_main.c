/* apu_wav: the RECOMP_AUDIO_WAV dump is a valid WAV of exactly what was
 * written, however the run ends.
 *
 *   apu_wav_test format     <path>   write 3 granules, close, parse
 *   apu_wav_test cap        <path>   cap at 2 granules, write 3, parse
 *   apu_wav_test exit-write <path>   write 3 granules, exit() without close
 *   apu_wav_test check-3    <path>   parse what exit-write left
 *   apu_wav_test kill-write <path>   write 1.5 s, _exit() without close
 *   apu_wav_test check-kill <path>   parse what kill-write left
 *   apu_wav_test wd-write   <path>   write 1.5 s, close_nowait, _exit()
 *   apu_wav_test check-wd   <path>   parse what wd-write left: all of it
 */
#include "apu_wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#define GRANULE 256

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", name); failures++; } } while (0)

/* Sample n (frame n/2, channel n%2) of the test signal: distinct, signed. */
static int16_t sig(long n) { return (int16_t)((n * 7919) ^ (n >> 3)); }

static void write_frames(long first, long count)
{
    int16_t g[GRANULE][2];
    while (count > 0) {
        int n = count > GRANULE ? GRANULE : (int)count, i;
        for (i = 0; i < n; i++) {
            g[i][0] = sig(2 * (first + i));
            g[i][1] = sig(2 * (first + i) + 1);
        }
        apu_wav_write(&g[0][0], n);
        first += n; count -= n;
    }
}

static uint32_t le32(const unsigned char *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Parse `path`; check the header is a 48 kHz s16 stereo PCM WAV whose sizes
 * agree with each other and with the file, and that the payload is the test
 * signal. Returns the number of sample frames the header declares, or -1. */
static long parse(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char h[44];
    long fsize, frames, i;
    int ok = 1;

    if (!f) { printf("FAIL: cannot open %s\n", path); failures++; return -1; }
    fseek(f, 0, SEEK_END); fsize = ftell(f); fseek(f, 0, SEEK_SET);
    if (fread(h, 1, 44, f) != 44) {
        printf("FAIL: short header\n"); failures++; fclose(f); return -1;
    }
    CHECK("RIFF/WAVE/fmt/data tags", !memcmp(h, "RIFF", 4)
          && !memcmp(h + 8, "WAVEfmt ", 8) && !memcmp(h + 36, "data", 4));
    CHECK("fmt chunk 16, PCM", le32(h + 16) == 16 && h[20] == 1 && h[21] == 0);
    CHECK("2 channels", h[22] == 2);
    CHECK("48000 Hz", le32(h + 24) == 48000);
    CHECK("byte rate 192000", le32(h + 28) == 192000);
    CHECK("block align 4, 16-bit", h[32] == 4 && h[34] == 16);
    CHECK("RIFF size = 36 + data", le32(h + 4) == 36 + le32(h + 40));
    CHECK("data size is whole frames", le32(h + 40) % 4 == 0);
    CHECK("data fits in the file", 44 + (long)le32(h + 40) <= fsize);
    frames = (long)(le32(h + 40) / 4);
    for (i = 0; i < frames * 2 && ok; i++) {
        unsigned char b[2];
        if (fread(b, 1, 2, f) != 2) { ok = 0; break; }
        if ((int16_t)(b[0] | (b[1] << 8)) != sig(i)) ok = 0;
    }
    CHECK("payload is what was written", ok);
    fclose(f);
    return frames;
}

int main(int argc, char **argv)
{
    const char *mode, *path;
    long n;

    if (argc != 3) { fprintf(stderr, "usage: %s mode path\n", argv[0]); return 2; }
    mode = argv[1]; path = argv[2];

    if (!strcmp(mode, "format")) {
        CHECK("open", apu_wav_open(path, 0));
        CHECK("second open refused", !apu_wav_open(path, 0));
        write_frames(0, 3 * GRANULE);
        apu_wav_close();
        apu_wav_close();                         /* idempotent */
        CHECK("closed", !apu_wav_is_open());
        apu_wav_write((const int16_t *)"\1\2\3\4", 1);   /* no-op closed */
        n = parse(path);
        CHECK("data size = 3 x 256 x 4", n == 3 * GRANULE);
    } else if (!strcmp(mode, "cap")) {
        CHECK("open", apu_wav_open(path, 2.0 * GRANULE / 48000.0));
        write_frames(0, 3 * GRANULE);
        CHECK("cap closes the file", !apu_wav_is_open());
        n = parse(path);
        CHECK("capped at 2 granules", n == 2 * GRANULE);
    } else if (!strcmp(mode, "exit-write")) {
        if (!apu_wav_open(path, 0)) return 1;
        write_frames(0, 3 * GRANULE);
        exit(0);                                 /* no apu_wav_close */
    } else if (!strcmp(mode, "check-3")) {
        n = parse(path);
        CHECK("atexit finalised 3 granules", n == 3 * GRANULE);
    } else if (!strcmp(mode, "kill-write")) {
        if (!apu_wav_open(path, 0)) return 1;
        write_frames(0, 48000 + 24000);
        fflush(stdout);
        _exit(0);                                /* no atexit, no close */
    } else if (!strcmp(mode, "wd-write")) {     /* the watchdog's exit */
        if (!apu_wav_open(path, 0)) return 1;
        write_frames(0, 48000 + 24000);
        if (!apu_wav_close_nowait(500)) return 1;
        CHECK("closed", !apu_wav_is_open());
        CHECK("nothing open: nowait is a no-op", apu_wav_close_nowait(0));
        fflush(stdout);
        _exit(failures ? 1 : 0);
    } else if (!strcmp(mode, "check-wd")) {
        n = parse(path);
        CHECK("close_nowait before _exit keeps the tail", n == 72000);
    } else if (!strcmp(mode, "check-kill")) {
        n = parse(path);
        CHECK("refresh covers the first second", n >= 48000 && n <= 72000);
    } else {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 2;
    }

    if (failures == 0) { printf("apu_wav %s: ALL PASS\n", mode); return 0; }
    printf("apu_wav %s: %d FAILURE(S)\n", mode, failures);
    return 1;
}
